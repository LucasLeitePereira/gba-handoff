#include "link.h"
#include "proc.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <map>
#include <sstream>

namespace cable {

namespace {
const int kPollMs = 2000;          // `adb devices` period while not connected
const int kCmdTimeoutMs = 8000;    // first call may start the adb server
const Uint64 kRereverseMs = 10000; // re-issue reverse while still not connected (adb server restarts lose it)

struct Dev { std::string serial, state; };

std::vector<Dev> parse_devices(const std::string &text)
{
	std::vector<Dev> v;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if (line.empty() || line[0] == '*' || line.rfind("List of", 0) == 0) continue;
		size_t tab = line.find_first_of(" \t");
		if (tab == std::string::npos) continue;
		size_t st = line.find_first_not_of(" \t", tab);
		if (st == std::string::npos) continue;
		size_t en = line.find_first_of(" \t", st);
		v.push_back({line.substr(0, tab), line.substr(st, en == std::string::npos ? en : en - st)});
	}
	return v;
}
} // namespace

std::string AdbWatcher::find_adb() const
{
	if (!adb_override_.empty()) return adb_override_;
#ifdef _WIN32
	const char *rel = "adb/adb.exe";
#else
	const char *rel = "adb/adb";
#endif
	if (const char *base = SDL_GetBasePath()) {
		std::string p = std::string(base) + rel;   // next to the executable, as shipped
		SDL_PathInfo info;
		if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) return p;
	}
	return "adb";   // PATH
}

void AdbWatcher::sleep_ms(int ms)
{
	for (int t = 0; t < ms && !stop_; t += 50) SDL_Delay(50);
}

void AdbWatcher::start()
{
	stop_ = false;
	th_ = std::thread([this] { thread_main(); });
}

void AdbWatcher::stop()
{
	stop_ = true;
	if (th_.joinable()) th_.join();
}

void AdbWatcher::thread_main()
{
	const std::string adb = find_adb();
	SDL_Log("adb: using '%s'", adb.c_str());
	const std::string spec = "tcp:" + std::to_string(port_);
	std::map<std::string, Uint64> reversed;   // serial -> time of the last successful reverse
	bool warned_missing = false;

	while (!stop_) {
		if (st_.connected()) {   // light polling: nothing to do while a phone is connected
			reversed.clear();    // a replug after this session must redo the reverse
			sleep_ms(kPollMs);
			continue;
		}
		std::string out;
		int rc = proc::run({adb, "devices"}, kCmdTimeoutMs, out);
		if (rc == -1) {
			st_.set_adb_note("adb não encontrado (pasta adb/ ao lado do .exe, ou no PATH)");
			if (!warned_missing) { SDL_Log("adb: could not run '%s'", adb.c_str()); warned_missing = true; }
			sleep_ms(10000);
			continue;
		}
		warned_missing = false;
		std::vector<Dev> devs = parse_devices(out);
		bool any_ready = false, any_unauth = false;
		std::map<std::string, Uint64> still;
		for (const Dev &d : devs) {
			if (d.state == "unauthorized") any_unauth = true;
			if (d.state != "device") continue;
			Uint64 now = SDL_GetTicks();
			auto it = reversed.find(d.serial);
			if (it == reversed.end() || now - it->second >= kRereverseMs) {
				std::string o2;
				int r2 = proc::run({adb, "-s", d.serial, "reverse", spec, spec}, kCmdTimeoutMs, o2);
				if (r2 == 0) {
					if (it == reversed.end()) SDL_Log("adb: %s reverse %s ok", d.serial.c_str(), spec.c_str());
					still[d.serial] = now;
				} else {
					SDL_Log("adb: reverse failed for %s (rc=%d): %s", d.serial.c_str(), r2, o2.c_str());
					continue;
				}
			} else {
				still[d.serial] = it->second;
			}
			any_ready = true;
		}
		reversed.swap(still);   // devices that disappeared are forgotten: replug redoes the reverse
		if (any_ready) st_.set_adb_note("aparelho detectado, abra o app no celular");
		else if (any_unauth) st_.set_adb_note("autorize a depuração USB no aparelho");
		else st_.set_adb_note("aguardando aparelho (cabo USB + depuração USB)");
		sleep_ms(kPollMs);
	}
}

} // namespace cable
