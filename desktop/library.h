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

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

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
// lines are ignored; a missing file means the defaults). `prefer_newest` is compared with the phone
// on every connection (see GBAHP_CTL_SETTINGS); the keymap and the volume are LOCAL to this PC and
// never travel to the phone.
enum { kNumPcButtons = 10 };   // GBA buttons in this order: A B L R Start Select Up Down Left Right
struct Settings {
	bool prefer_newest = false;   // resolve a version conflict by itself: the newest version wins
	SDL_Scancode keys[kNumPcButtons];   // keyboard map (ini: key_a=Z ... via SDL_GetScancodeName)
	int volume = 100;             // 0..100, steps of 10
	bool muted = false;
	Settings();                   // defaults
};
const char *button_label(int i);                 // "A", "B", "L", "R", "Start", "Select", "Cima", ...
SDL_Scancode default_key(int i);
// Keys that cannot be bound to a button: they are fixed shortcuts (Esc, F2, F5, F8, F11) or the
// volume/mute keys (M, -, +, keypad) or UNKNOWN.
bool key_reserved(SDL_Scancode k);
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
