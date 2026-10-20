#include "link.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <condition_variable>
#include <cstring>

extern "C" {
#include "gbah_core.h"
#include "gbahp_net.h"
}

namespace cable {

// ---------------------------------------------------------------- Status

void Status::set_connected(const std::string &device_name)
{
	{
		std::lock_guard<std::mutex> l(m_);
		device_ = device_name;
	}
	connected_ = !device_name.empty();
	gen_++;
}

void Status::set_adb_note(const std::string &note)
{
	{
		std::lock_guard<std::mutex> l(m_);
		if (note == note_) return;
		note_ = note;
	}
	gen_++;
}

void Status::set_port_busy(uint16_t port)
{
	port_busy_ = true;
	set_adb_note("porta " + std::to_string(port) + " ocupada (outra instância aberta?)");
}

std::string Status::device() const
{
	std::lock_guard<std::mutex> l(m_);
	return device_;
}

std::string Status::note() const
{
	std::lock_guard<std::mutex> l(m_);
	return note_;
}

std::string Status::title() const
{
	std::lock_guard<std::mutex> l(m_);
	if (!device_.empty()) return "GBA Handoff - conectado: " + device_;
	if (!note_.empty()) return "GBA Handoff - " + note_;
	return "GBA Handoff";
}

// ---------------------------------------------------------------- wire builders

namespace wire {
namespace {
template <typename Enc, typename M>
std::vector<uint8_t> build(Enc enc, const M &m)
{
	size_t n = enc(&m, nullptr, 0);
	if (!n) return {};
	std::vector<uint8_t> v(n);
	if (enc(&m, v.data(), v.size()) != n) return {};
	return v;
}
} // namespace

std::vector<uint8_t> library(const std::vector<gbahp_lib_entry> &e)
{
	size_t n = gbahp_encode_library(e.data(), (uint32_t)e.size(), nullptr, 0);
	if (!n) return {};
	std::vector<uint8_t> v(n);
	if (gbahp_encode_library(e.data(), (uint32_t)e.size(), v.data(), v.size()) != n) return {};
	return v;
}
std::vector<uint8_t> state_push(const gbahp_state_push &p) { return build(gbahp_encode_state_push, p); }
std::vector<uint8_t> state_ack(const uint8_t sha1[20], const gbah_ver &v, uint8_t status)
{
	gbahp_state_ack a{};
	memcpy(a.sha1_rom, sha1, 20);
	a.version = v.counter;
	memcpy(a.origin_device, v.dev, 16);
	a.status = status;
	return build(gbahp_encode_state_ack, a);
}
std::vector<uint8_t> control(uint8_t cmd, const uint8_t sha1[20], uint32_t arg)
{
	gbahp_control c{};
	c.cmd = cmd;
	if (sha1) memcpy(c.sha1, sha1, 20);
	c.arg = arg;
	return build(gbahp_encode_control, c);
}
std::vector<uint8_t> thumb(const uint8_t sha1[20], const void *data, uint32_t len)
{
	gbahp_thumb t{};
	memcpy(t.sha1, sha1, 20);
	t.thumb = (const uint8_t *)data;
	t.thumb_len = len;
	return build(gbahp_encode_thumb, t);
}
std::vector<uint8_t> rom_request(const uint8_t sha1[20])
{
	gbahp_rom_request r{};
	memcpy(r.sha1, sha1, 20);
	return build(gbahp_encode_rom_request, r);
}
} // namespace wire

// ---------------------------------------------------------------- Outbox (writer thread of one session)

// Every send goes through this queue so that (a) the main thread never blocks on a socket,
// (b) frames are never interleaved, (c) a 32 MiB ROM goes out in chunks and small messages
// (PONG, ACK, CONTROL) overtake it between chunks.
struct Outbox {
	std::mutex m;
	std::condition_variable cv;
	std::deque<std::vector<uint8_t>> msgs;
	size_t queued = 0;
	struct Rom {
		std::string path;
		uint8_t sha1[20];
		std::vector<uint8_t> data;
		uint64_t off = 0;
		bool loaded = false;
	};
	std::deque<Rom> roms;
	bool closed = false;
	bool sending = false;
	gbahp_sock sock = GBAHP_BAD_SOCK;
	std::thread th;

	void run()
	{
		std::vector<uint8_t> chunk;
		for (;;) {
			std::vector<uint8_t> msg;
			Rom *rom = nullptr;
			{
				std::unique_lock<std::mutex> l(m);
				cv.wait(l, [&] { return closed || !msgs.empty() || !roms.empty(); });
				if (closed) return;
				if (!msgs.empty()) {
					msg = std::move(msgs.front());
					msgs.pop_front();
					queued -= msg.size();
					sending = true;
				} else {
					rom = &roms.front();   // only this thread pops roms; deque keeps references valid on push_back
				}
			}
			if (!msg.empty()) {
				bool ok = write(msg.data(), msg.size());
				{
					std::lock_guard<std::mutex> l(m);
					sending = false;
				}
				if (!ok) return;
				continue;
			}
			if (!rom->loaded) {
				void *buf = nullptr;
				size_t n = 0;
				if (gbah_fs_read_all(rom->path.c_str(), &buf, &n) == 0 && n > 0) {
					rom->data.assign((uint8_t *)buf, (uint8_t *)buf + n);
				}
				gbah_fs_free(buf);
				rom->loaded = true;
				if (rom->data.empty()) {
					SDL_Log("link: cannot read ROM '%s' to send it", rom->path.c_str());
					pop_rom();
					continue;
				}
			}
			gbahp_rom_push p{};
			memcpy(p.sha1, rom->sha1, 20);
			p.total_size = rom->data.size();
			p.offset = rom->off;
			size_t n = rom->data.size() - (size_t)rom->off;
			if (n > GBAHP_ROM_CHUNK) n = GBAHP_ROM_CHUNK;
			p.data = rom->data.data() + rom->off;
			p.data_len = (uint32_t)n;
			chunk.resize(n + 128);
			size_t fl = gbahp_encode_rom_push(&p, chunk.data(), chunk.size());
			if (!fl || !write(chunk.data(), fl)) return;
			rom->off += n;
			if (rom->off >= rom->data.size()) pop_rom();
		}
	}
	void pop_rom()
	{
		std::lock_guard<std::mutex> l(m);
		roms.pop_front();
	}
	bool write(const void *d, size_t n)
	{
		if (gbahp_net_send(sock, d, n) == 0) return true;
		gbahp_net_shutdown(sock);   // wakes the reader: the session ends
		return false;
	}
	void close()
	{
		{
			std::lock_guard<std::mutex> l(m);
			closed = true;
		}
		cv.notify_all();
		gbahp_net_shutdown(sock);   // a send blocked on a stalled peer returns
		if (th.joinable()) th.join();
	}
};

// ---------------------------------------------------------------- Server

namespace {
const int kIdleTimeoutMs = 15000;   // no frame at all (the phone pings every ~5 s)
const int kHelloTimeoutMs = 5000;
const size_t kMaxDroppableQueue = 8u << 20;
const uint64_t kMaxRomSize = 32u << 20;

std::string name_of(const gbahp_hello &h)
{
	// The name comes from the network: keep it printable and short for the title.
	std::string s;
	for (size_t i = 0; i < h.name.n && s.size() < 64; i++) {
		unsigned char ch = (unsigned char)h.name.p[i];
		if (ch >= 0x20 && ch != 0x7F) s += (char)ch;
	}
	return s.empty() ? std::string("aparelho") : s;
}
} // namespace

bool Server::start(const ServerOptions &opt)
{
	opt_ = opt;
	gbahp_net_init();
	listener_ = gbahp_net_listen_loopback(opt_.port);
	if (listener_ == GBAHP_BAD_SOCK) {
		// "PORT_BUSY" is grepped by the integration test.
		SDL_Log("link: PORT_BUSY could not listen on 127.0.0.1:%u (port busy? another instance running?)", opt_.port);
		return false;
	}
	stop_ = false;
	th_ = std::thread([this] { thread_main(); });
	SDL_Log("link: listening on 127.0.0.1:%u", opt_.port);
	return true;
}

void Server::stop()
{
	stop_ = true;
	if (th_.joinable()) th_.join();
	if (listener_ != GBAHP_BAD_SOCK) { gbahp_net_close(listener_); listener_ = GBAHP_BAD_SOCK; }
}

void Server::thread_main()
{
	intptr_t next = GBAHP_BAD_SOCK;
	while (!stop_) {
		intptr_t s = next;
		next = GBAHP_BAD_SOCK;
		if (s == GBAHP_BAD_SOCK) {
			int timed_out = 0;
			s = gbahp_net_accept(listener_, 200, &timed_out);
			if (s == GBAHP_BAD_SOCK) {
				if (!timed_out) SDL_Delay(200); // listener error: do not spin
				continue;
			}
		}
		next = session(s);
	}
	if (next != GBAHP_BAD_SOCK) gbahp_net_close(next);
}

void Server::post(Event &&e)
{
	std::lock_guard<std::mutex> l(ev_m_);
	ev_.push_back(std::move(e));
}

bool Server::poll(Event &e)
{
	std::lock_guard<std::mutex> l(ev_m_);
	if (ev_.empty()) return false;
	e = std::move(ev_.front());
	ev_.pop_front();
	return true;
}

bool Server::send(std::vector<uint8_t> &&frame, bool droppable)
{
	if (frame.empty()) return false;
	std::shared_ptr<Outbox> o;
	{
		std::lock_guard<std::mutex> l(out_m_);
		o = out_;
	}
	if (!o) return false;
	{
		std::lock_guard<std::mutex> l(o->m);
		if (o->closed || (droppable && o->queued > kMaxDroppableQueue)) return false;
		o->queued += frame.size();
		o->msgs.push_back(std::move(frame));
	}
	o->cv.notify_one();
	return true;
}

void Server::flush(int timeout_ms)
{
	std::shared_ptr<Outbox> o;
	{
		std::lock_guard<std::mutex> l(out_m_);
		o = out_;
	}
	if (!o) return;
	for (int t = 0; t < timeout_ms; t += 10) {
		{
			std::lock_guard<std::mutex> l(o->m);
			if (o->closed || (o->msgs.empty() && !o->sending)) return;
		}
		SDL_Delay(10);
	}
}

bool Server::send_rom(const uint8_t sha1[20], const std::string &path)
{
	std::shared_ptr<Outbox> o;
	{
		std::lock_guard<std::mutex> l(out_m_);
		o = out_;
	}
	if (!o) return false;
	{
		std::lock_guard<std::mutex> l(o->m);
		if (o->closed) return false;
		Outbox::Rom r;
		r.path = path;
		memcpy(r.sha1, sha1, 20);
		o->roms.push_back(std::move(r));
	}
	o->cv.notify_one();
	return true;
}

void Server::handle_rom_chunk(const gbahp_rom_push &r)
{
	if (r.offset == 0) {
		rx_.active = false;
		if (r.total_size == 0 || r.total_size > kMaxRomSize) { SDL_Log("link: ROM_PUSH with absurd size %llu, ignored", (unsigned long long)r.total_size); return; }
		memcpy(rx_.sha1, r.sha1, 20);
		rx_.total = r.total_size;
		rx_.buf.clear();
		rx_.buf.reserve((size_t)r.total_size);
		rx_.active = true;
	}
	if (!rx_.active || memcmp(rx_.sha1, r.sha1, 20) != 0 || r.total_size != rx_.total || r.offset != rx_.buf.size() ||
	    r.offset + r.data_len > rx_.total) {
		SDL_Log("link: out-of-sequence ROM_PUSH chunk, transfer discarded");
		rx_.active = false;
		return;
	}
	rx_.buf.insert(rx_.buf.end(), r.data, r.data + r.data_len);
	if (rx_.buf.size() < rx_.total) return;

	rx_.active = false;
	Event ev;
	memcpy(ev.sha1, rx_.sha1, 20);
	uint8_t got[20];
	gbah_sha1(rx_.buf.data(), rx_.buf.size(), got);
	if (memcmp(got, rx_.sha1, 20) != 0) {
		SDL_Log("link: received ROM does not match its SHA-1, rejected");
		ev.type = Event::RomRejected;
	} else {
		char hex[41];
		gbah_sha1_hex(rx_.sha1, hex);
		std::string path = opt_.roms_dir + "/" + hex + ".gba";
		if (gbah_fs_exists(path.c_str()) || gbah_fs_write_atomic(path.c_str(), rx_.buf.data(), rx_.buf.size()) == 0) {
			SDL_Log("link: ROM %s received (%zu bytes)", hex, rx_.buf.size());
			ev.type = Event::RomReceived;
		} else {
			SDL_Log("link: could not store the received ROM");
			ev.type = Event::RomRejected;
		}
	}
	rx_.buf.clear();
	rx_.buf.shrink_to_fit();
	post(std::move(ev));
}

// Runs one connection. If it ends because a newer connection was already waiting (the phone
// app restarted and this socket is stale), returns that socket so the caller serves it next.
intptr_t Server::session(intptr_t sock)
{
	gbahp_conn c;
	gbahp_conn_init(&c, sock);
	gbahp_frame f;
	uint8_t out[1024];
	intptr_t pending = GBAHP_BAD_SOCK;

	gbahp_hello mine;
	memset(&mine, 0, sizeof mine);
	mine.proto_version = GBAHP_VERSION;
	memcpy(mine.device_id, opt_.device_id, 16);
	mine.core_version.p = opt_.core_version.data();
	mine.core_version.n = opt_.core_version.size();
	mine.name.p = opt_.device_name.data();
	mine.name.n = opt_.device_name.size();

	// 1. HELLO (bounded wait)
	int r = 0;
	for (int waited = 0; waited < kHelloTimeoutMs && !stop_; waited += 200) {
		r = gbahp_conn_recv(&c, &f, 200);
		if (r != 0) break;
	}
	gbahp_hello theirs;
	if (r != 1 || f.type != GBAHP_MSG_HELLO || gbahp_decode_hello(f.payload, f.len, &theirs) != 0) {
		SDL_Log("link: connection without a valid HELLO, dropped");
		gbahp_conn_close(&c);
		ended_++;
		return GBAHP_BAD_SOCK;
	}
	std::string name = name_of(theirs);   // copy: the view dies with the next recv

	// 2. HELLO_ACK; refuse incompatible peers (a state only loads on the same mGBA version)
	gbahp_hello_ack ack;
	ack.hello = mine;
	ack.status = (uint8_t)gbahp_hello_compat(&mine, &theirs);
	size_t n = gbahp_encode_hello_ack(&ack, out, sizeof out);
	if (!n || gbahp_conn_send(&c, out, n) != 0) { gbahp_conn_close(&c); ended_++; return GBAHP_BAD_SOCK; }
	if (ack.status != GBAHP_ACK_OK) {
		SDL_Log("link: refused '%s' (status %d: %s)", name.c_str(), ack.status,
		        ack.status == GBAHP_ACK_CORE_MISMATCH ? "different mGBA version" : "different protocol version");
		gbahp_net_shutdown(c.s);
		gbahp_conn_close(&c);
		ended_++;
		return GBAHP_BAD_SOCK;
	}

	// 3. from here on every send goes through the writer thread
	auto ob = std::make_shared<Outbox>();
	ob->sock = c.s;
	ob->th = std::thread([ob] { ob->run(); });
	{
		std::lock_guard<std::mutex> l(out_m_);
		out_ = ob;
	}
	rx_.active = false;
	st_.set_connected(name);
	SDL_Log("link: conectado: %s", name.c_str());
	{
		Event e;
		e.type = Event::Connected;
		e.name = name;
		post(std::move(e));   // the main thread answers with our LIBRARY
	}

	// 4. serve until BYE / error / idle timeout / stop / superseded
	Uint64 last_rx = SDL_GetTicks();
	while (!stop_) {
		r = gbahp_conn_recv(&c, &f, 200);
		if (r == 0) {
			if ((int)(SDL_GetTicks() - last_rx) > kIdleTimeoutMs) { SDL_Log("link: idle timeout"); break; }
			// a pending connection means the phone reconnected: drop the stale one
			pending = gbahp_net_accept(listener_, 0, nullptr);
			if (pending != GBAHP_BAD_SOCK) {
				SDL_Log("link: new connection while one is active: dropping the old one");
				break;
			}
			continue;
		}
		if (r < 0) {
			if (r == -2) SDL_Log("link: protocol error %d, dropping", c.proto_error);
			break;
		}
		last_rx = SDL_GetTicks();
		bool done = false;
		switch (f.type) {
		case GBAHP_MSG_PING: {
			gbahp_ping p;
			if (gbahp_decode_ping(f.payload, f.len, &p) == 0) {
				n = gbahp_encode_pong(&p, out, sizeof out);
				if (n) send(std::vector<uint8_t>(out, out + n));
			}
			break;
		}
		case GBAHP_MSG_INPUT: {
			gbahp_input in;
			if (gbahp_decode_input(f.payload, f.len, &in) == 0) keys_ = in.keys;
			break;
		}
		case GBAHP_MSG_LIBRARY: {
			gbahp_library l;
			if (gbahp_decode_library(f.payload, f.len, &l) == 0) {
				Event e;
				e.type = Event::PhoneLibrary;
				for (uint32_t i = 0; i < l.count; i++) {
					gbahp_lib_entry le;
					gbahp_library_get(&l, i, &le);
					e.lib.push_back(le);
				}
				post(std::move(e));
			}
			break;
		}
		case GBAHP_MSG_ROM_REQUEST: {
			gbahp_rom_request rq;
			if (gbahp_decode_rom_request(f.payload, f.len, &rq) == 0) {
				Event e;
				e.type = Event::RomRequest;
				memcpy(e.sha1, rq.sha1, 20);
				post(std::move(e));
			}
			break;
		}
		case GBAHP_MSG_ROM_PUSH: {
			gbahp_rom_push rp;
			if (gbahp_decode_rom_push(f.payload, f.len, &rp) == 0) handle_rom_chunk(rp);
			break;
		}
		case GBAHP_MSG_STATE_PUSH: {
			gbahp_state_push sp;
			if (gbahp_decode_state_push(f.payload, f.len, &sp) == 0) {
				auto pd = std::make_shared<PushData>();
				memcpy(pd->sha1, sp.sha1_rom, 20);
				memcpy(pd->version.dev, sp.origin_device, 16);
				pd->version.counter = sp.version;
				memcpy(pd->base.dev, sp.base_device, 16);
				pd->base.counter = sp.base;
				pd->flags = sp.flags;
				pd->timestamp_ms = sp.timestamp_ms;
				pd->frame = sp.frame;
				pd->state.assign(sp.state, sp.state + sp.state_len);
				pd->sav.assign(sp.sav, sp.sav + sp.sav_len);
				pd->thumb.assign(sp.thumb, sp.thumb + sp.thumb_len);
				Event e;
				e.type = Event::Push;
				e.push = std::move(pd);
				post(std::move(e));
			}
			break;
		}
		case GBAHP_MSG_STATE_ACK: {
			Event e;
			e.type = Event::Ack;
			if (gbahp_decode_state_ack(f.payload, f.len, &e.ack) == 0) post(std::move(e));
			break;
		}
		case GBAHP_MSG_CONTROL: {
			Event e;
			e.type = Event::Control;
			if (gbahp_decode_control(f.payload, f.len, &e.ctl) == 0) post(std::move(e));
			break;
		}
		case GBAHP_MSG_BYE: done = true; break;
		default:
			SDL_Log("link: ignoring message type %u", f.type);
			break;
		}
		if (done) break;
	}

	keys_ = 0;
	{
		std::lock_guard<std::mutex> l(out_m_);
		out_.reset();
	}
	ob->close();   // wakes + joins the writer; also shuts the socket down
	st_.set_connected("");
	{
		Event e;
		e.type = Event::Disconnected;
		post(std::move(e));
	}
	SDL_Log("link: desconectado");
	gbahp_conn_close(&c);
	ended_++;
	return pending;
}

} // namespace cable
