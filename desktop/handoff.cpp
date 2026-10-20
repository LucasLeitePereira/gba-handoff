#include "handoff.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include "gbahp.h"
}

namespace cable {

namespace {

uint32_t now_ms() { return (uint32_t)SDL_GetTicks(); }

uint64_t fnv(const void *d, size_t n)
{
	const uint8_t *p = (const uint8_t *)d;
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
	return h;
}

gbah_meta meta_of(const PushData &p)
{
	gbah_meta m{};
	memcpy(m.sha1, p.sha1, 20);
	m.has_data = 1;
	m.version = p.version;
	m.base = p.base;
	m.timestamp_ms = p.timestamp_ms;
	m.frame = p.frame;
	return m;
}

std::vector<uint8_t> encode_push(const gbah_meta &m, uint8_t flags, const void *state, size_t state_len,
                                 const void *sav, size_t sav_len, const void *thumb, size_t thumb_len)
{
	gbahp_state_push p{};
	memcpy(p.sha1_rom, m.sha1, 20);
	p.version = m.version.counter;
	memcpy(p.origin_device, m.version.dev, 16);
	memcpy(p.base_device, m.base.dev, 16);
	p.base = m.base.counter;
	p.flags = flags;
	p.timestamp_ms = m.timestamp_ms;
	p.frame = m.frame;
	p.state = (const uint8_t *)state; p.state_len = (uint32_t)state_len;
	p.sav = (const uint8_t *)sav;     p.sav_len = (uint32_t)sav_len;
	p.thumb = (const uint8_t *)thumb; p.thumb_len = (uint32_t)thumb_len;
	return wire::state_push(p);
}

} // namespace

Hub::Hub(gbah_core *core, Library &lib, DiskQueue &disk, Server &server, Status &st, const HubOptions &opt)
    : core_(core), lib_(lib), disk_(disk), server_(server), st_(st), opt_(opt)
{
	settings_ = lib_.load_settings();
}

bool Hub::osd_active() const { return !osd_.empty() && (int32_t)(osd_until_ - now_ms()) > 0; }

void Hub::say(const std::string &msg)
{
	osd_ = msg;
	osd_until_ = now_ms() + 4000;
	SDL_Log("hub: %s", msg.c_str());
}

std::string Hub::mode_text() const
{
	switch (mode_) {
	case Mode::Menu: return "";
	case Mode::Playing: return "";
	case Mode::Conflict: return "Conflito: resolva no celular";
	case Mode::Returning: return "Enviando para o celular...";
	case Mode::WaitPhone: return "Jogando no celular";
	case Mode::AwaitPlace: return connected_ ? "Escolha no celular" : "Pausado (sem cabo)";
	}
	return "";
}

std::string Hub::pending_text() const
{
	return settings_pending_ && connected_ ? "Configuracoes diferentes: escolha no celular" : "";
}

void Hub::set_mode(Mode m, const char *why)
{
	static const char *const names[] = { "Menu", "Playing", "Conflict", "Returning", "WaitPhone", "AwaitPlace" };
	if (m != mode_) SDL_Log("hub: mode %s -> %s (%s)", names[(int)mode_], names[(int)m], why);
	// Single owner: whatever the phone was holding down must not outlive the time the PC plays.
	if (mode_ == Mode::Playing && m != Mode::Playing) server_.clear_keys();
	mode_ = m;
}

// ---------------------------------------------------------------- snapshots & pushes

Hub::SnapPtr Hub::take_snapshot()
{
	if (!cur_) return nullptr;
	auto s = std::make_shared<Snap>();
	void *st = nullptr;
	size_t n = gbah_state_save(core_, &st);
	if (!n) return nullptr;
	s->state.assign((uint8_t *)st, (uint8_t *)st + n);
	gbah_free(st);
	void *sv = nullptr;
	size_t vn = gbah_sram_export(core_, &sv);
	if (vn) s->sav.assign((uint8_t *)sv, (uint8_t *)sv + vn);
	gbah_free(sv);
	const uint8_t *fb = (const uint8_t *)gbah_framebuffer(core_);
	s->thumb.assign(fb, fb + GBAHP_THUMB_BYTES);
	s->frame = gbah_frame_counter(core_);

	if (dirty_ || !cur_->meta.has_data) {
		gbah_sync_advance(&cur_->meta, lib_.device_id(), gbah_now_ms(), s->frame);
		dirty_ = false;
		s->meta = cur_->meta;
		disk_.post([dir = lib_.games_dir(), s] {
			if (gbah_store_commit(dir.c_str(), &s->meta, s->state.data(), s->state.size(), s->sav.data(), s->sav.size(),
			                      s->thumb.data(), (uint32_t)s->thumb.size()) != 0)
				SDL_Log("hub: COMMIT FAILED for a snapshot");
		});
	} else {
		s->meta = cur_->meta;
	}
	sram_hash_ = fnv(s->sav.data(), s->sav.size());
	last_snap_ms_ = now_ms();
	return s;
}

void Hub::checkpoint_if_dirty()
{
	if (cur_ && dirty_ && (mode_ == Mode::Playing)) take_snapshot();
}

void Hub::add_inflight(const uint8_t sha1[20], const gbah_ver &v, Kind k, const SnapPtr &s, uint8_t flags)
{
	Inflight f;
	memcpy(f.sha1, sha1, 20);
	f.ver = v;
	f.kind = k;
	f.snap = s;
	f.flags = flags;
	f.t_ms = now_ms();
	inflight_.push_back(std::move(f));
}

Hub::Inflight *Hub::find_inflight(const uint8_t sha1[20], const gbah_ver &v)
{
	for (Inflight &f : inflight_)
		if (!memcmp(f.sha1, sha1, 20) && gbah_ver_eq(&f.ver, &v)) return &f;
	return nullptr;
}

bool Hub::push_snapshot(const SnapPtr &s, uint8_t flags, Kind kind)
{
	if (!s || !connected_) return false;
	std::vector<uint8_t> fr = encode_push(s->meta, flags, s->state.data(), s->state.size(), s->sav.data(), s->sav.size(),
	                                      s->thumb.data(), s->thumb.size());
	if (!server_.send(std::move(fr), flags == GBAHP_PUSH_MIRROR)) return false;
	add_inflight(s->meta.sha1, s->meta.version, kind, s, flags);
	return true;
}

void Hub::push_stored(GameInfo *g, uint8_t flags, Kind kind)
{
	if (!connected_ || !g || !g->meta.has_data) return;
	gbah_meta m = g->meta;
	add_inflight(m.sha1, m.version, kind, nullptr, flags);
	disk_.post([dir = lib_.games_dir(), m, flags, dev = std::vector<uint8_t>(lib_.device_id(), lib_.device_id() + 16), srv = &server_] {
		void *st = nullptr, *sv = nullptr;
		size_t sn = 0, vn = 0;
		if (gbah_store_read(dir.c_str(), m.sha1, &st, &sn, &sv, &vn) != 0) {
			SDL_Log("hub: cannot read the stored state to send it");
			return;
		}
		gbah_meta tmp;
		uint8_t *th = nullptr;
		uint32_t tl = 0;
		gbah_store_meta(dir.c_str(), m.sha1, dev.data(), &tmp, &th, &tl);
		if (tl != GBAHP_THUMB_BYTES) tl = 0;
		srv->send(encode_push(m, flags, st, sn, sv, vn, th, tl));
		gbah_fs_free(st); gbah_fs_free(sv); free(th);
	});
}

void Hub::sync_push(GameInfo *g)
{
	if (!g) return;
	if (cur_ == g && mode_ == Mode::Playing) mirror_now();
	else push_stored(g, 0, Kind::Sync);
}

void Hub::mirror_now()
{
	if (!cur_) return;
	if (!connected_ || mode_ != Mode::Playing) { checkpoint_if_dirty(); return; }
	for (const Inflight &f : inflight_)       // one at a time: wait for the ACK (or 30 s)
		if (f.kind == Kind::Mirror && now_ms() - f.t_ms < 30000) { checkpoint_if_dirty(); return; }
	inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(), [](const Inflight &f) { return f.kind == Kind::Mirror; }),
	                inflight_.end());
	push_snapshot(take_snapshot(), GBAHP_PUSH_MIRROR, Kind::Mirror);
}

void Hub::periodic()
{
	next_periodic_ = now_ms() + opt_.mirror_ms;
	sram_frames_ = 0;
	if (connected_) mirror_now();
	else checkpoint_if_dirty();
}

void Hub::send_thumb(GameInfo *g)
{
	if (!connected_ || !g || !g->meta.has_data) return;
	disk_.post([dir = lib_.games_dir(), sha = std::vector<uint8_t>(g->sha1, g->sha1 + 20),
	            dev = std::vector<uint8_t>(lib_.device_id(), lib_.device_id() + 16), srv = &server_] {
		gbah_meta m;
		uint8_t *th = nullptr;
		uint32_t tl = 0;
		gbah_store_meta(dir.c_str(), sha.data(), dev.data(), &m, &th, &tl);
		if (th && tl == GBAHP_THUMB_BYTES) srv->send(wire::thumb(sha.data(), th, tl));
		free(th);
	});
}

void Hub::send_library()
{
	std::vector<gbahp_lib_entry> v;
	for (const GameInfo &g : lib_.games()) {
		gbahp_lib_entry e{};
		memcpy(e.sha1, g.sha1, 20);
		e.size = g.rom_size;
		e.has_data = g.meta.has_data ? 1 : 0;
		if (e.has_data) {
			memcpy(e.version_dev, g.meta.version.dev, 16); e.version = g.meta.version.counter;
			memcpy(e.base_dev, g.meta.base.dev, 16);       e.base = g.meta.base.counter;
			e.timestamp_ms = g.meta.timestamp_ms;
			e.frame = g.meta.frame;
		}
		v.push_back(e);
	}
	server_.send(wire::library(v));
}

void Hub::send_pc_status()
{
	if (!connected_) return;
	bool live = cur_ && (mode_ == Mode::Playing || mode_ == Mode::Conflict || mode_ == Mode::Returning);
	server_.send(wire::control(GBAHP_CTL_PC_STATUS, live ? cur_->sha1 : nullptr, live ? 1 : 0));
}

void Hub::send_ack(const uint8_t sha1[20], const gbah_ver &v, uint8_t status)
{
	server_.send(wire::state_ack(sha1, v, status));
}

// ---------------------------------------------------------------- core / game handling

bool Hub::load_rom(GameInfo *g)
{
	void *buf = nullptr;
	size_t n = 0;
	rom_in_core_ = false;
	if (gbah_fs_read_all(lib_.rom_path(g->sha1).c_str(), &buf, &n) != 0) return false;
	int rc = gbah_load_rom_buffer(core_, buf, n);
	gbah_fs_free(buf);
	if (rc != 0) return false;
	uint8_t got[20];
	if (gbah_rom_sha1(core_, got) != 0 || memcmp(got, g->sha1, 20) != 0) return false;
	memcpy(core_sha_, got, 20);
	rom_in_core_ = true;
	return true;
}

void Hub::enter_playing(GameInfo *g)
{
	cur_ = g;
	set_mode(Mode::Playing, "playing");
	server_.clear_keys();   // a late INPUT of a previous period must not press a key in the new one
	dirty_ = false;
	sram_frames_ = 0;
	void *sv = nullptr;
	size_t vn = gbah_sram_export(core_, &sv);
	sram_hash_ = fnv(sv, vn);
	gbah_free(sv);
	last_snap_ms_ = now_ms();
	next_periodic_ = now_ms() + opt_.mirror_ms;
	audio_flush_ = true;
	send_pc_status();
}

bool Hub::open_game(const uint8_t sha1[20])
{
	GameInfo *g = lib_.find(sha1);
	if (!g || !lib_.has_rom(sha1)) { say("ROM nao encontrada"); return false; }
	if (g->conflict && connected_) { say("Conflito: resolva no celular"); return false; }
	if (cur_ && mode_ == Mode::Playing) {
		if (cur_ == g) return true;
		close_game();
	}
	disk_.wait_idle();   // the files must reflect every snapshot queued so far
	if (!load_rom(g)) { say("ROM invalida"); set_mode(Mode::Menu, "invalid ROM"); cur_ = nullptr; return false; }

	void *st = nullptr, *sv = nullptr;
	size_t sn = 0, vn = 0;
	bool have_state = gbah_store_read(lib_.games_dir().c_str(), sha1, &st, &sn, &sv, &vn) == 0;
	bool restored = false;
	if (have_state) {
		restored = gbah_handoff_restore(core_, st, sn, sv, vn) == 0;
		if (!restored) {
			SDL_Log("hub: stored state could not be loaded (other mGBA version?), cold start with the .sav");
			load_rom(g);
		}
	}
	if (!restored) {
		if (!sv) {   // state missing: a bare .sav may still exist
			size_t n = 0;
			std::string p = lib_.games_dir() + "/" + hex_of(sha1) + ".sav";
			if (gbah_fs_read_all(p.c_str(), &sv, &n) == 0 && n) vn = n;
			else { gbah_fs_free(sv); sv = nullptr; vn = 0; }
		}
		if (sv && vn && gbah_load_sav_buffer(core_, sv, vn) != 0) SDL_Log("hub: could not import the .sav");
	}
	gbah_fs_free(st);
	gbah_fs_free(sv);
	enter_playing(g);
	say(restored ? "Continuando do ponto salvo" : "Novo jogo");
	return true;
}

void Hub::close_game()
{
	if (cur_ && (mode_ == Mode::Playing || mode_ == Mode::Conflict)) {
		if (connected_ && mode_ == Mode::Playing) mirror_now();
		else checkpoint_if_dirty();
	}
	set_mode(Mode::Menu, "game closed");
	cur_ = nullptr;
	send_pc_status();
}

void Hub::quick_save()
{
	if (mode_ != Mode::Playing) return;
	take_snapshot();
	say("Salvo");
}

void Hub::quick_load()
{
	if (!cur_ || (mode_ != Mode::Playing && mode_ != Mode::Conflict)) return;
	GameInfo *g = cur_;
	set_mode(Mode::Menu, "quick load");
	cur_ = nullptr;
	open_game(g->sha1);
}

// Without a cable nobody can say where to continue: the user at the PC decides (Enter). The game is
// reloaded from the store, which holds exactly what was agreed (the resolved / handed-over version).
void Hub::resume_local()
{
	if ((mode_ != Mode::WaitPhone && mode_ != Mode::AwaitPlace) || !cur_) return;
	if (connected_) { say(mode_ == Mode::WaitPhone ? "O celular esta jogando: devolva por la" : "Escolha no celular onde continuar"); return; }
	GameInfo *g = cur_;
	set_mode(Mode::Menu, "resume without a cable");
	cur_ = nullptr;
	open_game(g->sha1);
}

GameInfo *Hub::import_rom(const std::string &path)
{
	if (mode_ == Mode::Playing || mode_ == Mode::Conflict || mode_ == Mode::Returning) { say("Volte ao menu para importar"); return nullptr; }
	uint8_t sha[20];
	bool added = false;
	int rc = lib_.import_rom_file(path, sha, &added);
	if (rc != 0) { say("Arquivo nao e uma ROM de GBA"); return nullptr; }
	GameInfo *g = lib_.find(sha);
	if (g && added) {
		// A .sav next to the ROM (older versions / other emulators): adopt it once as the cartridge save.
		std::string base = path;
		size_t dot = base.find_last_of('.');
		std::string cands[2] = { path + ".sav", dot == std::string::npos ? std::string() : base.substr(0, dot) + ".sav" };
		for (const std::string &c : cands) {
			void *b = nullptr;
			size_t n = 0;
			if (!c.empty() && gbah_fs_read_all(c.c_str(), &b, &n) == 0 && n) {
				std::string dst = lib_.games_dir() + "/" + hex_of(sha) + ".sav";
				if (!gbah_fs_exists(dst.c_str())) gbah_fs_write_atomic(dst.c_str(), b, n);
				gbah_fs_free(b);
				break;
			}
			gbah_fs_free(b);
		}
	}
	return g;
}

bool Hub::load_into_core(GameInfo *g, const PushData &p)
{
	if (cur_ && cur_ != g && mode_ == Mode::Playing) checkpoint_if_dirty();
	if (!(rom_in_core_ && !memcmp(core_sha_, g->sha1, 20)) && !load_rom(g)) return false;
	int rc = gbah_handoff_restore(core_, p.state.data(), p.state.size(), p.sav.empty() ? nullptr : p.sav.data(), p.sav.size());
	if (rc != 0) {
		SDL_Log("hub: the received state could not be loaded (rc=%d)", rc);
		rom_in_core_ = false;
		set_mode(Mode::Menu, "state could not be loaded");
		cur_ = nullptr;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- frames & timers

void Hub::on_frames(int n)
{
	if (n <= 0) return;
	dirty_ = true;
	sram_frames_ += (uint32_t)n;
}

void Hub::tick()
{
	Event e;
	for (int guard = 0; guard < 64 && server_.poll(e); guard++) dispatch(e);

	if (mode_ == Mode::Playing && cur_) {
		uint32_t now = now_ms();
		if ((int32_t)(now - next_periodic_) >= 0) {
			periodic();
		} else if (sram_frames_ >= 120) {   // ~2 s: did the game write its save?
			sram_frames_ = 0;
			void *sv = nullptr;
			size_t vn = gbah_sram_export(core_, &sv);
			uint64_t h = fnv(sv, vn);
			gbah_free(sv);
			if (h != sram_hash_ && now - last_snap_ms_ >= opt_.sav_min_ms) periodic();
		}
	}
}

void Hub::shutdown()
{
	if (cur_ && (mode_ == Mode::Playing || mode_ == Mode::Conflict)) {
		if (connected_ && mode_ == Mode::Playing) mirror_now();
		else checkpoint_if_dirty();
	}
	if (connected_) server_.flush(1500);
}

// ---------------------------------------------------------------- link events

void Hub::dispatch(Event &e)
{
	switch (e.type) {
	case Event::Connected: on_connected(e.name); break;
	case Event::Disconnected: on_disconnected(); break;
	case Event::PhoneLibrary: on_phone_library(e.lib); break;
	case Event::Push: on_push(e.push); break;
	case Event::Ack: on_ack(e.ack); break;
	case Event::Control: on_control(e.ctl); break;
	case Event::RomRequest:
		if (lib_.has_rom(e.sha1)) server_.send_rom(e.sha1, lib_.rom_path(e.sha1));
		else server_.send(wire::control(GBAHP_CTL_ROM_UNAVAILABLE, e.sha1, 0));
		break;
	case Event::RomReceived: on_rom_received(e.sha1); break;
	case Event::RomRejected: {
		say("ROM recebida corrompida");
		const std::string hex = hex_of(e.sha1);
		rom_asked_.erase(hex);                    // the phone may try again
		auto it = held_.find(hex);
		if (it != held_.end()) {                  // the held push cannot be applied
			send_ack(it->second->sha1, it->second->version, GBAHP_STATE_ACK_NO_ROM);
			held_.erase(it);
		}
		break;
	}
	}
}

void Hub::on_connected(const std::string &name)
{
	connected_ = true;
	phone_lib_.clear();
	held_.clear();
	rom_asked_.clear();
	rom_sent_.clear();
	inflight_.clear();
	checkpoint_if_dirty();    // the library must describe the game as it is right now
	say("Conectado: " + name);
	phone_settings_known_ = false;
	settings_pending_ = false;
	send_settings();
	send_library();
	send_pc_status();
}

void Hub::on_disconnected()
{
	connected_ = false;
	phone_lib_.clear();
	held_.clear();
	inflight_.clear();
	phone_settings_known_ = false;
	settings_pending_ = false;
	for (GameInfo &g : lib_.games()) g.conflict = false;
	// Paused states have no owner to hand the game to: the PC does NOT restart on its own (that was the
	// "two devices playing" bug). The user decides with Enter; a game that was Playing simply goes on.
	if (mode_ == Mode::Conflict || mode_ == Mode::Returning || mode_ == Mode::AwaitPlace) {
		set_mode(Mode::AwaitPlace, "cable dropped while paused");
		say("Cabo desconectado: Enter continua aqui");
	} else if (mode_ == Mode::Playing) {
		say("Cabo desconectado: continuando so no PC");
	}
}

void Hub::on_phone_library(const std::vector<gbahp_lib_entry> &lib)
{
	phone_lib_.clear();
	checkpoint_if_dirty();
	bool pushed_live = false;
	for (const gbahp_lib_entry &e : lib) {
		phone_lib_[hex_of(e.sha1)] = e;
		GameInfo *g = lib_.find(e.sha1);
		if (!g) continue;                                  // we do not have that game: the phone pushes if it wants
		gbah_meta r{};
		memcpy(r.sha1, e.sha1, 20);
		r.has_data = e.has_data;
		if (e.has_data) {
			memcpy(r.version.dev, e.version_dev, 16); r.version.counter = e.version;
			memcpy(r.base.dev, e.base_dev, 16);       r.base.counter = e.base;
			r.timestamp_ms = e.timestamp_ms;
			r.frame = e.frame;
		}
		switch (gbah_sync_decide(&g->meta, &r)) {
		case GBAH_SYNC_SEND:
			if (e.size > 0 || e.has_data) {
				if (cur_ == g && mode_ == Mode::Playing) { mirror_now(); pushed_live = true; }
				else push_stored(g, 0, Kind::Sync);
			}
			break;
		case GBAH_SYNC_CONFLICT:
			mark_conflict(g);
			break;
		default:
			g->conflict = false;
			break;
		}
	}
	// The game being played is mirrored even when the phone does not list it (its ROM follows on NO_ROM).
	if (!pushed_live && cur_ && mode_ == Mode::Playing && !phone_lib_.count(hex_of(cur_->sha1))) mirror_now();
}

void Hub::mark_conflict(GameInfo *g)
{
	g->conflict = true;
	if (cur_ == g && (mode_ == Mode::Playing || mode_ == Mode::Returning || mode_ == Mode::AwaitPlace)) {
		set_mode(Mode::Conflict, "version conflict");
		send_pc_status();
	}
	say("Conflito em " + g->title + ": resolva no celular");
	send_thumb(g);
}

void Hub::on_push(const std::shared_ptr<PushData> &p)
{
	GameInfo *g = lib_.find(p->sha1);
	if (!g || !lib_.has_rom(p->sha1)) {
		// No ROM yet: hold the push, ask for the ROM; it is applied (and ACKed) when the ROM is verified.
		std::string hex = hex_of(p->sha1);
		held_[hex] = p;
		if (!rom_asked_[hex]) {
			rom_asked_[hex] = true;
			server_.send(wire::rom_request(p->sha1));
		}
		return;
	}
	if (p->state.empty()) { send_ack(p->sha1, p->version, GBAHP_STATE_ACK_ERROR); return; }
	if (g == cur_ && mode_ == Mode::Playing) checkpoint_if_dirty();   // compare against what the game really is now

	const gbah_meta r = meta_of(*p);
	const int d = gbah_sync_decide(&g->meta, &r);
	const bool resolved = (p->flags & GBAHP_PUSH_RESOLVED) != 0;
	const bool handoff = (p->flags & GBAHP_PUSH_HANDOFF) != 0;
	bool store = false;
	if (resolved) {
		store = d != GBAH_SYNC_NOTHING;
	} else if (d == GBAH_SYNC_RECEIVE) {
		store = true;
	} else if (d == GBAH_SYNC_SEND) {
		send_ack(p->sha1, p->version, GBAHP_STATE_ACK_STALE);
		sync_push(g);                 // the phone is behind: bring it up to date
		return;
	} else if (d == GBAH_SYNC_CONFLICT) {
		send_ack(p->sha1, p->version, GBAHP_STATE_ACK_CONFLICT);
		mark_conflict(g);
		return;
	}

	// The core follows the store on a takeover (HANDOFF), or when the pushed game is the one being
	// played AND the push replaces what we hold (RECEIVE / RESOLVED). A push that changes nothing
	// (same version, e.g. a repeated mirror) must never rewind a game the PC is playing.
	const bool live = g == cur_ && (mode_ == Mode::Playing || mode_ == Mode::Conflict);
	const bool take = handoff || (live && store);
	const bool was_conflict = g == cur_ && mode_ == Mode::Conflict;
	if (take && !load_into_core(g, *p)) {
		send_ack(p->sha1, p->version, GBAHP_STATE_ACK_ERROR);
		say("Nao foi possivel carregar o estado recebido");
		return;
	}
	if (store) {
		if (resolved && g->meta.has_data) {   // the loser becomes a backup (the live game was checkpointed above)
			disk_.post([dir = lib_.games_dir(), sha = std::vector<uint8_t>(g->sha1, g->sha1 + 20)] {
				gbah_store_backup(dir.c_str(), sha.data(), gbah_now_ms(), 3);
			});
		}
		gbah_meta nm = g->meta;
		gbah_sync_adopt(&nm, &r);
		g->meta = nm;
		g->conflict = false;
		disk_.post([dir = lib_.games_dir(), p, nm, srv = &server_] {
			int rc = gbah_store_commit(dir.c_str(), &nm, p->state.data(), p->state.size(), p->sav.data(), p->sav.size(),
			                           p->thumb.data(), (uint32_t)p->thumb.size());
			srv->send(wire::state_ack(p->sha1, p->version, rc == 0 ? GBAHP_STATE_ACK_OK : GBAHP_STATE_ACK_ERROR));
		});
	} else {
		g->conflict = false;
		send_ack(p->sha1, p->version, GBAHP_STATE_ACK_OK);
	}
	if (take && resolved && !handoff) {
		// A resolution ends with BOTH sides paused: the phone asks where to continue. The PC does not
		// go back to Playing here (that was the "two devices playing" bug).
		cur_ = g;
		dirty_ = false;
		audio_flush_ = true;
		set_mode(Mode::AwaitPlace, "conflict resolved, version loaded");
		send_pc_status();
		say("Versao escolhida: escolha onde continuar no celular");
	} else if (take) {
		enter_playing(g);
		say(handoff ? "Jogo recebido do celular" : "Versao escolhida carregada");
	} else if (resolved && was_conflict) {   // the same version on both sides: nothing to load, still no owner
		g->conflict = false;
		set_mode(Mode::AwaitPlace, "conflict closed");
		send_pc_status();
	}
}

void Hub::on_rom_received(const uint8_t sha1[20])
{
	GameInfo *g = lib_.add_rom(sha1);
	if (g) say("ROM recebida: " + g->title);
	auto it = held_.find(hex_of(sha1));
	if (it != held_.end()) {
		std::shared_ptr<PushData> p = it->second;
		held_.erase(it);
		on_push(p);
	}
}

void Hub::on_ack(const gbahp_state_ack &a)
{
	gbah_ver v{};
	memcpy(v.dev, a.origin_device, 16);
	v.counter = a.version;
	Inflight *f = find_inflight(a.sha1_rom, v);
	if (!f) return;
	Inflight in = *f;
	inflight_.erase(inflight_.begin() + (f - inflight_.data()));
	GameInfo *g = lib_.find(a.sha1_rom);
	if (!g) return;

	switch (a.status) {
	case GBAHP_STATE_ACK_OK:
		if (gbah_ver_eq(&g->meta.version, &v)) {   // confirmed transfer: both sides are identical now
			gbah_sync_mark_synced(&g->meta);
			disk_.post([dir = lib_.games_dir(), sha = std::vector<uint8_t>(g->sha1, g->sha1 + 20), v] {
				gbah_store_mark_synced(dir.c_str(), sha.data(), &v);
			});
		}
		if (in.kind == Kind::Return && mode_ == Mode::Returning && cur_ == g) {
			set_mode(Mode::WaitPhone, "game handed to the phone");
			send_pc_status();
			say("Jogando no celular");
		} else if (in.kind == Kind::Fetch) {
			g->conflict = false;
			if ((mode_ == Mode::Conflict || mode_ == Mode::Playing) && cur_ == g) {
				// the phone took our version: both sides paused until it says where to continue
				dirty_ = false;
				audio_flush_ = true;
				set_mode(Mode::AwaitPlace, "our version was taken");
				send_pc_status();
				say("Versao do PC mantida: escolha onde continuar no celular");
			}
		}
		break;
	case GBAHP_STATE_ACK_CONFLICT:
		mark_conflict(g);
		break;
	case GBAHP_STATE_ACK_NO_ROM:
		if (!in.retried && !rom_sent_[hex_of(g->sha1)]) {
			rom_sent_[hex_of(g->sha1)] = true;
			server_.send_rom(g->sha1, lib_.rom_path(g->sha1));
			if (in.snap) push_snapshot(in.snap, in.flags, in.kind);
			else push_stored(g, in.flags, in.kind);
			if (Inflight *n = find_inflight(g->sha1, v)) n->retried = true;
		} else if (in.kind == Kind::Return && mode_ == Mode::Returning) {
			set_mode(Mode::Playing, "phone refused the game (no ROM)");
			say("O celular nao aceitou o jogo (sem ROM)");
		}
		break;
	case GBAHP_STATE_ACK_STALE:
		say("O celular ja tem uma versao mais nova");
		if (in.kind == Kind::Return && mode_ == Mode::Returning) set_mode(Mode::Playing, "phone already has a newer version");
		break;
	default:
		say("O celular recusou o estado");
		if (in.kind == Kind::Return && mode_ == Mode::Returning) set_mode(Mode::Playing, "phone refused the state");
		break;
	}
}

void Hub::on_control(const gbahp_control &c)
{
	GameInfo *g = lib_.find(c.sha1);
	switch (c.cmd) {
	case GBAHP_CTL_RETURN:
		if (g && g == cur_ && (mode_ == Mode::Playing || mode_ == Mode::AwaitPlace)) request_return();
		else if (g && g->meta.has_data && !(g == cur_ && mode_ == Mode::Conflict)) push_stored(g, GBAHP_PUSH_HANDOFF, Kind::Sync);
		break;
	case GBAHP_CTL_FETCH:
		if (!g || !g->meta.has_data) break;
		if (g == cur_ && (mode_ == Mode::Playing || mode_ == Mode::Conflict)) {
			if (mode_ == Mode::Playing) checkpoint_if_dirty();
			SnapPtr s = take_snapshot();
			push_snapshot(s, GBAHP_PUSH_RESOLVED, Kind::Fetch);
		} else {
			push_stored(g, GBAHP_PUSH_RESOLVED, Kind::Fetch);
		}
		break;
	case GBAHP_CTL_PLACE_PC: place_pc(g); break;
	case GBAHP_CTL_PLACE_PHONE: place_phone(g); break;
	case GBAHP_CTL_SETTINGS: on_phone_settings(c.arg, false); break;
	case GBAHP_CTL_SETTINGS_SET: on_phone_settings(c.arg, true); break;
	case GBAHP_CTL_ROM_UNAVAILABLE: {
		auto it = held_.find(hex_of(c.sha1));
		if (it != held_.end()) {
			send_ack(it->second->sha1, it->second->version, GBAHP_STATE_ACK_NO_ROM);
			held_.erase(it);
			say("O celular nao tem a ROM");
		}
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------- where to continue / settings

// The phone chose the PC after a resolution. The PC plays what the store holds (the resolved version).
void Hub::place_pc(GameInfo *g)
{
	if (!g || !connected_ || !lib_.has_rom(g->sha1)) return;
	if (g->conflict) { say("Conflito em aberto: resolva no celular"); return; }
	if (mode_ == Mode::Playing && cur_ == g) { send_pc_status(); return; }   // already here
	if ((mode_ == Mode::Playing || mode_ == Mode::Conflict || mode_ == Mode::Returning) && cur_ != g) {
		say("O PC esta ocupado com outro jogo");
		return;
	}
	cur_ = nullptr;
	set_mode(Mode::Menu, "place on the PC");
	open_game(g->sha1);   // reloads the stored version and enters Playing (announces PC_STATUS)
}

// The phone chose itself: the PC stays paused on the game and shows that it is being played there.
void Hub::place_phone(GameInfo *g)
{
	if (!g) return;
	if (cur_ == g && mode_ == Mode::AwaitPlace) {
		set_mode(Mode::WaitPhone, "continues on the phone");
		send_pc_status();
		say("Jogando no celular");
	} else {
		SDL_Log("hub: PLACE_PHONE ignored (mode %d)", (int)mode_);
	}
}

std::string settings_note(bool on) { return on ? "Sempre manter a versao mais recente: ligado" : "Perguntar em caso de conflito"; }

void Hub::send_settings()
{
	server_.send(wire::control(GBAHP_CTL_SETTINGS, nullptr, settings_.prefer_newest ? GBAHP_SET_PREFER_NEWEST : 0u));
}

// bits = the phone's settings. chosen = SETTINGS_SET: the user picked them as the default of both.
void Hub::on_phone_settings(uint32_t bits, bool chosen)
{
	const bool pn = (bits & GBAHP_SET_PREFER_NEWEST) != 0;
	if (chosen) {
		settings_.prefer_newest = pn;
		if (!lib_.save_settings(settings_)) SDL_Log("hub: could not save settings.ini");
		phone_settings_known_ = true;
		phone_prefer_newest_ = pn;
		settings_pending_ = false;
		say(settings_note(pn));
		return;
	}
	phone_settings_known_ = true;
	phone_prefer_newest_ = pn;
	settings_pending_ = pn != settings_.prefer_newest;
	if (settings_pending_) say("Configuracoes diferentes: escolha no celular");
}

void Hub::set_prefer_newest(bool on)
{
	if (settings_.prefer_newest == on) return;
	settings_.prefer_newest = on;
	if (!lib_.save_settings(settings_)) say("Nao foi possivel salvar as configuracoes");
	else say(settings_note(on));
	if (connected_) {
		send_settings();   // the phone compares and asks the user if they differ
		settings_pending_ = phone_settings_known_ && phone_prefer_newest_ != on;
	}
}

void Hub::request_return()
{
	if (!connected_) { say("Nenhum celular conectado"); return; }
	if (mode_ == Mode::Conflict) { say("Resolva o conflito no celular primeiro"); return; }
	if ((mode_ != Mode::Playing && mode_ != Mode::AwaitPlace) || !cur_) return;
	SnapPtr s = take_snapshot();
	if (!s) { say("Falha ao salvar o estado"); return; }
	auto it = phone_lib_.find(hex_of(cur_->sha1));
	if ((it == phone_lib_.end() || it->second.size == 0) && !rom_sent_[hex_of(cur_->sha1)]) {   // the phone lacks the ROM: send it first
		rom_sent_[hex_of(cur_->sha1)] = true;
		server_.send_rom(cur_->sha1, lib_.rom_path(cur_->sha1));
	}
	inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(), [](const Inflight &f) { return f.kind == Kind::Mirror; }),
	                inflight_.end());
	if (!push_snapshot(s, GBAHP_PUSH_HANDOFF, Kind::Return)) { say("Falha ao enviar para o celular"); return; }
	set_mode(Mode::Returning, "handing the game to the phone");
	send_pc_status();
	say("Enviando para o celular...");
}

} // namespace cable
