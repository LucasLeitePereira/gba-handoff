// PC game library: <data dir>/roms/<sha1>.gba and <data dir>/games/<sha1>.{state,sav,meta}.
// All members are for the MAIN thread only, except DiskQueue (its own thread) and the free
// functions. The disk is written ONLY by the DiskQueue worker (FIFO), so ordering of commits,
// backups and mark_synced is the order in which the main thread posted them.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "gbah_sync.h"
}

namespace cable {

// FIFO of closures run on one worker thread (disk I/O, ACK after a commit, ...).
class DiskQueue {
public:
	DiskQueue();
	~DiskQueue() { drain_and_stop(); }
	void post(std::function<void()> fn);
	void drain_and_stop();   // runs everything already queued, then joins
	void wait_idle();        // blocks until everything queued so far has run
	size_t pending() const;

private:
	void run();
	mutable std::mutex m_;
	std::condition_variable cv_, idle_cv_;
	std::deque<std::function<void()>> q_;
	bool stop_ = false;
	bool busy_ = false;
	std::thread th_;
};

struct GameInfo {
	uint8_t sha1[20] = {};
	std::string title;       // from the ROM header (12 chars)
	uint64_t rom_size = 0;
	gbah_meta meta{};        // PC's progress (has_data = 0: none)
	bool conflict = false;   // diverges from the phone (set while connected)
};

// User settings of the PC, one `key=value` per line in <root>/settings.ini (unknown keys and bad
// lines are ignored; a missing file means the defaults). The phone has its own copy of the same
// settings; they are compared on every connection (see GBAHP_CTL_SETTINGS).
struct Settings {
	bool prefer_newest = false;   // resolve a version conflict by itself: the newest version wins
};
std::string settings_to_text(const Settings &s);
Settings settings_from_text(const std::string &text);

class Library {
public:
	// root empty = default (%APPDATA%/GBAHandoff or $XDG_DATA_HOME/gbahandoff). false = unusable dir.
	bool open(const std::string &root);
	const std::string &root() const { return root_; }
	const std::string &games_dir() const { return games_; }
	const uint8_t *device_id() const { return dev_; }
	Settings load_settings() const;                  // defaults if settings.ini is missing / unreadable
	bool save_settings(const Settings &s) const;     // atomic (tmp + rename)

	std::deque<GameInfo> &games() { return games_list_; }
	GameInfo *find(const uint8_t sha1[20]);
	bool has_rom(const uint8_t sha1[20]) const;
	std::string rom_path(const uint8_t sha1[20]) const;

	// Copies a ROM file into roms/ (by SHA-1 of its bytes). 0 ok (sha1 filled, *added = new),
	// <0 unreadable / not worth a ROM.
	int import_rom_file(const std::string &path, uint8_t sha1[20], bool *added);
	// Registers a ROM already written to roms/ (received over the cable).
	GameInfo *add_rom(const uint8_t sha1[20]);
	void remove_rom(const uint8_t sha1[20]);   // undo of a failed import

private:
	GameInfo *refresh_one(const uint8_t sha1[20]);
	std::string root_, roms_, games_;
	uint8_t dev_[16] = {};
	std::deque<GameInfo> games_list_;
};

std::string hex_of(const uint8_t sha1[20]);

} // namespace cable
