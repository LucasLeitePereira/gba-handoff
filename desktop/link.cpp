#include "link.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <cstring>

extern "C" {
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

std::string Status::title() const
{
	std::lock_guard<std::mutex> l(m_);
	if (!device_.empty()) return "GBA Handoff - conectado: " + device_;
	if (!note_.empty()) return "GBA Handoff - " + note_;
	return "GBA Handoff";
}

// ---------------------------------------------------------------- Server

namespace {
const int kIdleTimeoutMs = 15000;   // no frame at all (the phone pings every ~5 s)
const int kHelloTimeoutMs = 5000;

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
		SDL_Log("link: could not listen on 127.0.0.1:%u (port busy? another instance running?)", opt_.port);
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

	st_.set_connected(name);
	SDL_Log("link: conectado: %s", name.c_str());

	// 3. our LIBRARY (the phone sends its own; both directions are informational for now)
	n = gbahp_encode_library(opt_.library.data(), (uint32_t)opt_.library.size(), out, sizeof out);
	if (n) gbahp_conn_send(&c, out, n);

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
				if (!n || gbahp_conn_send(&c, out, n) != 0) done = true;
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
			if (gbahp_decode_library(f.payload, f.len, &l) == 0) SDL_Log("link: phone library: %u ROM(s)", l.count);
			break;
		}
		case GBAHP_MSG_BYE: done = true; break;
		default: // ROM/STATE messages arrive with the handoff (next phase)
			SDL_Log("link: ignoring message type %u", f.type);
			break;
		}
		if (done) break;
	}

	keys_ = 0;
	st_.set_connected("");
	SDL_Log("link: desconectado");
	gbahp_net_shutdown(c.s);
	gbahp_conn_close(&c);
	ended_++;
	return pending;
}

} // namespace cable
