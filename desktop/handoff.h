// Handoff logic of the PC side. MAIN THREAD ONLY: it owns the core, the library and every
// protocol decision. Network (link.cpp) and disk (DiskQueue) run on their own threads and talk to
// this class only through the event inbox / queued closures, so render and audio never wait on
// them. See sync/gbah_sync.h for the version rules.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "library.h"
#include "link.h"

extern "C" {
#include "gbah_core.h"
}

namespace cable {

struct HubOptions {
	uint32_t mirror_ms = 60000;        // period of MIRROR pushes (connected) / autosave (alone)
	uint32_t sav_min_ms = 10000;       // minimum gap between "the .sav changed" snapshots
};

class Hub {
public:
	enum class Mode {
		Menu,        // no game
		Playing,     // emulating
		Conflict,    // paused: this game diverges from the phone, the user decides on the phone
		Returning,   // paused: state sent to the phone, waiting for its STATE_ACK
		WaitPhone,   // game handed to the phone ("Jogando no celular")
		AwaitPlace   // paused: a conflict was resolved (or the cable dropped while paused); NOBODY plays until the
		             // phone says where to continue (CONTROL PLACE_PC / PLACE_PHONE) or, without a cable, the user
		             // presses Enter. This is what keeps "exactly one side plays" true after a resolution.
	};

	Hub(gbah_core *core, Library &lib, DiskQueue &disk, Server &server, Status &st, const HubOptions &opt);

	Mode mode() const { return mode_; }
	bool emulating() const { return mode_ == Mode::Playing; }
	const GameInfo *current() const { return cur_; }          // game in the core (or waiting for the phone)
	bool connected() const { return connected_; }
	const std::string &osd() const { return osd_; }           // transient message, "" = none
	bool osd_active() const;
	// Set once per takeover/load: the UI should drop queued audio.
	bool take_audio_flush() { bool f = audio_flush_; audio_flush_ = false; return f; }

	// ---- user actions
	bool open_game(const uint8_t sha1[20]);   // from the menu: resume the stored progress
	void close_game();                        // checkpoint, back to the menu
	void request_return();                    // F2: hand the game to the phone
	void resume_local();                      // WaitPhone / AwaitPlace without a cable -> play the PC's own version
	// Settings (menu): the change is saved and announced to the phone, which asks the user if the two differ.
	const Settings &settings() const { return settings_; }
	void set_prefer_newest(bool on);
	// PC-local settings (keymap, volume, mute): saved, never sent to the phone.
	void set_local_settings(const Settings &s);
	// Persistent line for the screen ("" = none): settings differ and the phone has to pick the default.
	std::string pending_text() const;
	void quick_save();                        // F5
	void quick_load();                        // F8
	// Imports a ROM file into the library (command line / drag and drop). Returns the game or null.
	GameInfo *import_rom(const std::string &path);

	// ---- called by the main loop
	void tick();                              // events from the link + timers
	void on_frames(int n);                    // after running n frames
	void shutdown();                          // final checkpoint (+ best-effort mirror)

	std::string mode_text() const;

private:
	struct Snap {
		std::vector<uint8_t> state, sav, thumb;
		uint32_t frame = 0;
		gbah_meta meta{};
	};
	typedef std::shared_ptr<Snap> SnapPtr;
	enum class Kind { Mirror, Return, Fetch, Sync };
	struct Inflight {
		uint8_t sha1[20];
		gbah_ver ver;
		Kind kind;
		SnapPtr snap;
		uint8_t flags;
		bool retried = false;
		uint32_t t_ms;
	};

	void dispatch(Event &e);
	void on_connected(const std::string &name);
	void on_disconnected();
	void on_phone_library(const std::vector<gbahp_lib_entry> &lib);
	void on_push(const std::shared_ptr<PushData> &p);
	void on_ack(const gbahp_state_ack &a);
	void on_control(const gbahp_control &c);
	void set_mode(Mode m, const char *why);    // the ONLY place that changes mode_ (logs it, releases remote keys)
	void send_settings();
	void on_phone_settings(uint32_t bits, bool chosen);
	void place_pc(GameInfo *g);
	void place_phone(GameInfo *g);
	void on_rom_received(const uint8_t sha1[20]);

	SnapPtr take_snapshot();                   // advances + queues the commit if there is new progress
	void checkpoint_if_dirty();
	void mirror_now();
	bool push_snapshot(const SnapPtr &s, uint8_t flags, Kind kind);
	void push_stored(GameInfo *g, uint8_t flags, Kind kind);
	void sync_push(GameInfo *g);               // our version should reach the phone
	void send_thumb(GameInfo *g);
	void send_library();
	void send_pc_status();
	void send_ack(const uint8_t sha1[20], const gbah_ver &v, uint8_t status);
	void mark_conflict(GameInfo *g);
	bool load_rom(GameInfo *g);
	bool load_into_core(GameInfo *g, const PushData &p);
	void enter_playing(GameInfo *g);
	void add_inflight(const uint8_t sha1[20], const gbah_ver &v, Kind k, const SnapPtr &s, uint8_t flags);
	Inflight *find_inflight(const uint8_t sha1[20], const gbah_ver &v);
	void say(const std::string &msg);
	void periodic();

	gbah_core *core_;
	Library &lib_;
	DiskQueue &disk_;
	Server &server_;
	Status &st_;
	HubOptions opt_;

	Mode mode_ = Mode::Menu;
	GameInfo *cur_ = nullptr;
	bool rom_in_core_ = false;
	uint8_t core_sha_[20] = {};
	bool dirty_ = false;                       // frames ran since the last snapshot
	bool connected_ = false;
	bool audio_flush_ = false;
	std::string osd_;
	uint32_t osd_until_ = 0;
	uint32_t next_periodic_ = 0, last_snap_ms_ = 0;
	uint32_t sram_frames_ = 0;
	uint64_t sram_hash_ = 0;

	Settings settings_;
	bool phone_settings_known_ = false;        // the phone announced its settings in this session
	bool phone_prefer_newest_ = false;
	bool settings_pending_ = false;            // they differ: waiting for the phone's SETTINGS_SET

	std::map<std::string, gbahp_lib_entry> phone_lib_;   // by sha1 hex
	std::map<std::string, std::shared_ptr<PushData>> held_;   // pushes waiting for their ROM
	std::map<std::string, bool> rom_asked_, rom_sent_;
	std::vector<Inflight> inflight_;
};

} // namespace cable
