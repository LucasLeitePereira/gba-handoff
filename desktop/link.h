// PC side of the cable link: TCP server on 127.0.0.1 (reached by the phone through
// `adb reverse`) and the adb watcher that sets that reverse up. Both run on their own
// threads; the render loop only polls Status::generation() to refresh the window title.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "gbahp.h"
}

namespace cable {

// What the window title shows. Written by the link/adb threads, read by the UI thread.
class Status {
public:
	void set_connected(const std::string &device_name); // empty name = disconnected
	void set_adb_note(const std::string &note);
	bool connected() const { return connected_.load(); }
	unsigned generation() const { return gen_.load(); }
	std::string title() const;

private:
	mutable std::mutex m_;
	std::string device_, note_;
	std::atomic<bool> connected_{false};
	std::atomic<unsigned> gen_{1};
};

struct ServerOptions {
	uint16_t port = GBAHP_DEFAULT_PORT;
	uint8_t device_id[16] = {};
	std::string device_name;
	std::string core_version;
	std::vector<gbahp_lib_entry> library; // ROMs this PC has
};

class Server {
public:
	explicit Server(Status &st) : st_(st) {}
	~Server() { stop(); }
	bool start(const ServerOptions &opt); // false if the port cannot be bound
	void stop();
	unsigned sessions_ended() const { return ended_.load(); }
	uint32_t remote_keys() const { return keys_.load(); } // latest INPUT from the phone

private:
	void thread_main();
	intptr_t session(intptr_t sock);

	Status &st_;
	ServerOptions opt_;
	intptr_t listener_ = -1;
	std::thread th_;
	std::atomic<bool> stop_{false};
	std::atomic<unsigned> ended_{0};
	std::atomic<uint32_t> keys_{0};
};

// Polls `adb devices` (only while no phone is connected) and runs
// `adb -s SERIAL reverse tcp:PORT tcp:PORT` for every authorised device.
class AdbWatcher {
public:
	AdbWatcher(Status &st, uint16_t port, std::string adb_override)
	    : st_(st), port_(port), adb_override_(std::move(adb_override)) {}
	~AdbWatcher() { stop(); }
	void start();
	void stop();

private:
	void thread_main();
	std::string find_adb() const;
	void sleep_ms(int ms);

	Status &st_;
	uint16_t port_;
	std::string adb_override_;
	std::thread th_;
	std::atomic<bool> stop_{false};
};

} // namespace cable
