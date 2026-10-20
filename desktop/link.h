// PC side of the cable link: TCP server on 127.0.0.1 (reached by the phone through
// `adb reverse`) and the adb watcher that sets that reverse up. Both run on their own
// threads; the render loop only polls Status::generation() to refresh the window title.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adbstate.h"

extern "C" {
#include "gbah_sync.h"
#include "gbahp.h"
}

namespace cable {

// What the window title shows. Written by the link/adb threads, read by the UI thread.
class Status {
public:
	void set_connected(const std::string &device_name); // empty name = disconnected
	void set_adb_note(const std::string &note);
	void set_port_busy(uint16_t port);                   // another instance owns the port
	bool port_busy() const { return port_busy_.load(); }
	std::string device() const;                          // connected phone name, "" = none
	std::string note() const;                            // adb note
	// Why the last phone was refused at HELLO ("" = none); cleared by the next accepted connection.
	void set_refusal(const std::string &why);
	std::string refusal() const;
	bool connected() const { return connected_.load(); }
	unsigned generation() const { return gen_.load(); }
	std::string title() const;

private:
	mutable std::mutex m_;
	std::string device_, note_, refusal_;
	std::atomic<bool> connected_{false};
	std::atomic<bool> port_busy_{false};
	std::atomic<unsigned> gen_{1};
};

struct ServerOptions {
	uint16_t port = GBAHP_DEFAULT_PORT;
	uint8_t device_id[16] = {};
	std::string device_name;
	std::string core_version;
	std::string roms_dir;                 // received ROMs are written here as <sha1>.gba
};

// A STATE_PUSH, owned (the wire views die with the next recv).
struct PushData {
	uint8_t sha1[20] = {};
	gbah_ver version{}, base{};
	uint8_t flags = 0;
	uint64_t timestamp_ms = 0;
	uint32_t frame = 0;
	std::vector<uint8_t> state, sav, thumb;
};

// What the link thread tells the main thread. ALL protocol decisions are made on the main
// thread (it owns the core and the library); the link thread is transport + ROM reception.
struct Event {
	enum Type { Connected, Disconnected, PhoneLibrary, Push, Ack, Control, RomRequest, RomReceived, RomRejected } type;
	std::string name;                     // Connected
	std::vector<gbahp_lib_entry> lib;     // PhoneLibrary
	std::shared_ptr<PushData> push;       // Push
	gbahp_state_ack ack{};                // Ack
	gbahp_control ctl{};                  // Control
	uint8_t sha1[20] = {};                // RomRequest / RomReceived / RomRejected
};

// Frame builders (main thread). Each returns one complete frame, empty on error.
namespace wire {
std::vector<uint8_t> library(const std::vector<gbahp_lib_entry> &e);
std::vector<uint8_t> state_push(const gbahp_state_push &p);
std::vector<uint8_t> state_ack(const uint8_t sha1[20], const gbah_ver &v, uint8_t status);
std::vector<uint8_t> control(uint8_t cmd, const uint8_t sha1[20], uint32_t arg);
std::vector<uint8_t> thumb(const uint8_t sha1[20], const void *data, uint32_t len);
std::vector<uint8_t> rom_request(const uint8_t sha1[20]);
} // namespace wire

struct Outbox;

class Server {
public:
	explicit Server(Status &st) : st_(st) {}
	~Server() { stop(); }
	bool start(const ServerOptions &opt); // false if the port cannot be bound
	void stop();
	unsigned sessions_ended() const { return ended_.load(); }
	uint32_t remote_keys() const { return keys_.load(); } // latest INPUT from the phone (control mode)
	void clear_keys() { keys_ = 0; }                      // the game left Playing: no key may stay held

	bool poll(Event &e);                  // main thread: next event, false if none
	// Queues one frame for the phone (never blocks). droppable frames are discarded when more
	// than 8 MiB are already waiting (a stalled link must not eat the PC's memory). false = not
	// connected / dropped.
	bool send(std::vector<uint8_t> &&frame, bool droppable = false);
	// Streams roms/<sha1>.gba as ROM_PUSH chunks, interleaved with (and below) normal messages.
	bool send_rom(const uint8_t sha1[20], const std::string &path);
	// Waits (bounded) until the queued messages have been written to the socket.
	void flush(int timeout_ms);

private:
	void thread_main();
	intptr_t session(intptr_t sock);
	void post(Event &&e);
	void handle_rom_chunk(const gbahp_rom_push &r);

	Status &st_;
	ServerOptions opt_;
	intptr_t listener_ = -1;
	std::thread th_;
	std::atomic<bool> stop_{false};
	std::atomic<unsigned> ended_{0};
	std::atomic<uint32_t> keys_{0};

	std::mutex ev_m_;
	std::deque<Event> ev_;
	std::mutex out_m_;
	std::shared_ptr<Outbox> out_;         // current session's writer (null = not connected)

	struct RomRx { uint8_t sha1[20] = {}; uint64_t total = 0; std::vector<uint8_t> buf; bool active = false; } rx_;
};

// What the menu's status card shows about the attached phone (written by the adb threads).
struct PhoneInfo {
	PhoneState state = PhoneState::None;
	std::string serial;
	std::string message;        // last install failure ("" = none); already says "do not uninstall"
	bool apk_found = true;      // GBAHandoff.apk exists next to the exe
};

// Polls `adb devices` (only while no phone is connected), runs
// `adb -s SERIAL reverse tcp:PORT tcp:PORT` for every authorised device, and checks (once per
// appearance of the device, and after an install) whether the app is installed and up to date.
class AdbWatcher {
public:
	AdbWatcher(Status &st, uint16_t port, std::string adb_override)
	    : st_(st), port_(port), adb_override_(std::move(adb_override)) {}
	~AdbWatcher() { stop(); }
	void start();
	void stop();

	PhoneInfo phone() const;
	bool can_install() const;            // state NoApp/Outdated, APK present, nothing running
	void install();                      // UI thread: `adb install -r` + launch, on its own thread
	void set_apk_path(const std::string &p) { apk_override_ = p; }   // test hook (--apk)

private:
	void thread_main();
	void install_main(std::string serial);
	std::string find_adb() const;
	std::string apk_path() const;
	void sleep_ms(int ms);
	void set_state(PhoneState s, const std::string &serial);
	void set_message(const std::string &m);

	Status &st_;
	uint16_t port_;
	std::string adb_override_, apk_override_, adb_;
	std::thread th_, inst_th_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> installing_{false};
	std::atomic<bool> recheck_{false};
	mutable std::mutex pm_;
	PhoneInfo info_;
};

} // namespace cable
