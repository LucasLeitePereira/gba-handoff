#include "library.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstring>
#include <random>

extern "C" {
#include "gbah_core.h"
}

namespace cable {

std::string hex_of(const uint8_t sha1[20])
{
	char h[41];
	gbah_sha1_hex(sha1, h);
	return h;
}

// ---------------------------------------------------------------- DiskQueue

DiskQueue::DiskQueue() : th_([this] { run(); }) {}

void DiskQueue::post(std::function<void()> fn)
{
	{
		std::lock_guard<std::mutex> l(m_);
		q_.push_back(std::move(fn));
	}
	cv_.notify_one();
}

size_t DiskQueue::pending() const
{
	std::lock_guard<std::mutex> l(m_);
	return q_.size() + (busy_ ? 1 : 0);
}

void DiskQueue::wait_idle()
{
	std::unique_lock<std::mutex> l(m_);
	idle_cv_.wait(l, [this] { return q_.empty() && !busy_; });
}

void DiskQueue::drain_and_stop()
{
	{
		std::lock_guard<std::mutex> l(m_);
		stop_ = true;
	}
	cv_.notify_all();
	if (th_.joinable()) th_.join();
}

void DiskQueue::run()
{
	for (;;) {
		std::function<void()> fn;
		{
			std::unique_lock<std::mutex> l(m_);
			cv_.wait(l, [this] { return stop_ || !q_.empty(); });
			if (q_.empty()) return;   // stop_ and nothing left
			fn = std::move(q_.front());
			q_.pop_front();
			busy_ = true;
		}
		fn();
		{
			std::lock_guard<std::mutex> l(m_);
			busy_ = false;
		}
		idle_cv_.notify_all();
	}
}

// ---------------------------------------------------------------- Library

namespace {

std::string default_root()
{
#ifdef _WIN32
	const char *app = "GBAHandoff";   // %APPDATA%\GBAHandoff
#else
	const char *app = "gbahandoff";   // $XDG_DATA_HOME/gbahandoff
#endif
	std::string r;
	if (char *p = SDL_GetPrefPath("", app)) {
		r = p;
		SDL_free(p);
	}
	return r;
}

void load_device_id(const std::string &root, uint8_t id[16])
{
	const std::string path = root + "/device_id";
	void *buf = nullptr;
	size_t n = 0;
	if (gbah_fs_read_all(path.c_str(), &buf, &n) == 0 && n == 16) {
		memcpy(id, buf, 16);
		gbah_fs_free(buf);
		return;
	}
	gbah_fs_free(buf);
	// Phase 3 kept the id under <pref>/gbahandoff/gbahandoff/: keep the identity if it exists.
	if (char *old = SDL_GetPrefPath("gbahandoff", "gbahandoff")) {
		std::string op = std::string(old) + "device_id";
		SDL_free(old);
		if (gbah_fs_read_all(op.c_str(), &buf, &n) == 0 && n == 16) {
			memcpy(id, buf, 16);
			gbah_fs_free(buf);
			gbah_fs_write_atomic(path.c_str(), id, 16);
			return;
		}
		gbah_fs_free(buf);
	}
	std::random_device rd;
	uint64_t mix = SDL_GetPerformanceCounter() ^ gbah_now_ms();   // random_device is weak on old mingw
	for (int i = 0; i < 16; i++) id[i] = (uint8_t)(rd() ^ (mix >> ((i % 8) * 8)) ^ (i * 0x9E));
	gbah_fs_write_atomic(path.c_str(), id, 16);
}

struct ScanCtx { std::vector<std::string> names; };
void scan_cb(const char *name, void *u)
{
	size_t n = strlen(name);
	if (n == 44 && !strcmp(name + 40, ".gba")) static_cast<ScanCtx *>(u)->names.push_back(name);
}

std::string title_of(const std::string &rom_path)
{
	uint8_t head[0xC0] = {};
	size_t got = 0;
	std::string t;
	if (gbah_fs_read_prefix(rom_path.c_str(), head, sizeof head, &got) == 0 && got >= 0xAC)
		for (int i = 0; i < 12; i++) {
			uint8_t c = head[0xA0 + i];
			if (c >= 0x20 && c < 0x7F) t += (char)c;
		}
	while (!t.empty() && t.back() == ' ') t.pop_back();
	return t;
}

} // namespace

bool Library::open(const std::string &root)
{
	root_ = root.empty() ? default_root() : root;
	while (root_.size() > 1 && (root_.back() == '/' || root_.back() == '\\')) root_.pop_back();
	if (root_.empty()) return false;
	roms_ = root_ + "/roms";
	games_ = root_ + "/games";
	if (gbah_fs_mkdirs(roms_.c_str()) != 0 || gbah_fs_mkdirs(games_.c_str()) != 0) {
		SDL_Log("library: cannot use '%s'", root_.c_str());
		return false;
	}
	load_device_id(root_, dev_);

	ScanCtx ctx;
	gbah_fs_list(roms_.c_str(), scan_cb, &ctx);
	std::sort(ctx.names.begin(), ctx.names.end());
	for (const std::string &n : ctx.names) {
		uint8_t sha[20];
		if (gbah_sha1_from_hex(n.c_str(), sha) == 0) refresh_one(sha);
	}
	SDL_Log("library: %s (%zu game(s))", root_.c_str(), games_list_.size());
	return true;
}

GameInfo *Library::find(const uint8_t sha1[20])
{
	for (GameInfo &g : games_list_)
		if (!memcmp(g.sha1, sha1, 20)) return &g;
	return nullptr;
}

std::string Library::rom_path(const uint8_t sha1[20]) const { return roms_ + "/" + hex_of(sha1) + ".gba"; }

bool Library::has_rom(const uint8_t sha1[20]) const { return gbah_fs_exists(rom_path(sha1).c_str()); }

GameInfo *Library::refresh_one(const uint8_t sha1[20])
{
	GameInfo *g = find(sha1);
	const bool fresh = g == nullptr;
	if (fresh) {
		games_list_.emplace_back();
		g = &games_list_.back();
		memcpy(g->sha1, sha1, 20);
	}
	const std::string rp = rom_path(sha1);
	uint64_t sz = 0;
	gbah_fs_size(rp.c_str(), &sz);
	g->rom_size = sz;
	g->title = title_of(rp);
	if (g->title.empty()) g->title = hex_of(sha1).substr(0, 8);
	// The in-memory meta is the truth while the process lives (disk writes are queued behind it).
	if (fresh) gbah_store_meta(games_.c_str(), sha1, dev_, &g->meta, nullptr, nullptr);
	return g;
}

GameInfo *Library::add_rom(const uint8_t sha1[20]) { return refresh_one(sha1); }

void Library::remove_rom(const uint8_t sha1[20])
{
	gbah_fs_remove(rom_path(sha1).c_str());
	games_list_.erase(std::remove_if(games_list_.begin(), games_list_.end(),
	                                 [&](const GameInfo &g) { return !memcmp(g.sha1, sha1, 20); }),
	                  games_list_.end());
}

int Library::import_rom_file(const std::string &path, uint8_t sha1[20], bool *added)
{
	void *buf = nullptr;
	size_t n = 0;
	if (added) *added = false;
	if (gbah_fs_read_all(path.c_str(), &buf, &n) != 0) return -1;
	if (n < 0xC0 || n > (32u << 20) || ((const uint8_t *)buf)[0xB2] != 0x96) { gbah_fs_free(buf); return -2; }   // 0x96 = GBA header fixed value
	gbah_sha1(buf, n, sha1);
	int rc = 0;
	if (!has_rom(sha1)) {
		rc = gbah_fs_write_atomic(rom_path(sha1).c_str(), buf, n) == 0 ? 0 : -3;
		if (rc == 0 && added) *added = true;
	}
	gbah_fs_free(buf);
	if (rc == 0) refresh_one(sha1);
	return rc;
}

} // namespace cable
