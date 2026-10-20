#include "link.h"
#include "proc.h"
#include "version.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <iterator>
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
	if (inst_th_.joinable()) inst_th_.join();
}

std::string AdbWatcher::apk_path() const
{
	if (!apk_override_.empty()) return apk_override_;
	const char *base = SDL_GetBasePath();
	return std::string(base ? base : "") + "GBAHandoff.apk";
}

PhoneInfo AdbWatcher::phone() const
{
	std::lock_guard<std::mutex> l(pm_);
	return info_;
}

bool AdbWatcher::can_install() const
{
	std::lock_guard<std::mutex> l(pm_);
	return !installing_ && info_.apk_found && (info_.state == PhoneState::NoApp || info_.state == PhoneState::Outdated);
}

void AdbWatcher::set_state(PhoneState s, const std::string &serial)
{
	std::lock_guard<std::mutex> l(pm_);
	if (info_.state != s) SDL_Log("adb: phone state %d -> %d (%s)", (int)info_.state, (int)s, serial.c_str());
	info_.state = s;
	info_.serial = serial;
	if (s == PhoneState::Ready || s == PhoneState::None || s == PhoneState::Unauthorized) info_.message.clear();
}

void AdbWatcher::set_message(const std::string &m)
{
	std::lock_guard<std::mutex> l(pm_);
	info_.message = m;
}

void AdbWatcher::install()
{
	std::string serial;
	{
		std::lock_guard<std::mutex> l(pm_);
		if (installing_ || (info_.state != PhoneState::NoApp && info_.state != PhoneState::Outdated)) return;
		SDL_PathInfo pi;
		info_.apk_found = SDL_GetPathInfo(apk_path().c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE;
		if (!info_.apk_found) return;
		serial = info_.serial;
		info_.message.clear();
		info_.state = PhoneState::Installing;
		installing_ = true;
	}
	if (inst_th_.joinable()) inst_th_.join();   // the previous run has finished (installing_ was false)
	inst_th_ = std::thread([this, serial] { install_main(serial); });
}

// NEVER uninstalls: that would wipe the game saves kept in the app's private storage.
void AdbWatcher::install_main(std::string serial)
{
	std::string out;
	int rc = proc::run({adb_, "-s", serial, "install", "-r", apk_path()}, 180000, out);
	InstallResult r = parse_install_result(rc, out);
	SDL_Log("adb: install on %s rc=%d ok=%d: %s", serial.c_str(), rc, (int)r.ok, out.substr(0, 300).c_str());
	if (r.ok) {
		std::string o2;
		proc::run({adb_, "-s", serial, "shell", "monkey", "-p", "io.gbahandoff", "-c",
		           "android.intent.category.LAUNCHER", "1"}, kCmdTimeoutMs, o2);
		set_message("");
	} else {
		set_message(r.message);
	}
	{
		std::lock_guard<std::mutex> l(pm_);
		// Optimistic until the re-check (next poll) confirms; on failure back to what it was.
		info_.state = r.ok ? PhoneState::Ready : PhoneState::Outdated;
	}
	recheck_ = true;
	installing_ = false;
}

void AdbWatcher::thread_main()
{
	adb_ = find_adb();
	const std::string adb = adb_;
	SDL_Log("adb: using '%s'", adb.c_str());
	const std::string spec = "tcp:" + std::to_string(port_);
	std::map<std::string, Uint64> reversed;   // serial -> time of the last successful reverse
	std::map<std::string, int> checked;       // serial -> installed versionCode (kVersionAbsent = none); asked once
	bool warned_missing = false;

	while (!stop_) {
		{
			SDL_PathInfo pi;
			bool f = SDL_GetPathInfo(apk_path().c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE;
			std::lock_guard<std::mutex> l(pm_);
			info_.apk_found = f;
		}
		if (recheck_.exchange(false)) checked.clear();
		if (st_.connected()) {   // light polling: nothing to do while a phone is connected
			reversed.clear();    // a replug after this session must redo the reverse
			checked.clear();     // and the app may have been changed meanwhile
			if (!installing_) set_state(PhoneState::Ready, "");
			sleep_ms(kPollMs);
			continue;
		}
		std::string out;
		int rc = proc::run({adb, "devices"}, kCmdTimeoutMs, out);
		if (rc == -1) {
			st_.set_adb_note("adb não encontrado (pasta adb/ ao lado do .exe, ou no PATH)");
			if (!warned_missing) { SDL_Log("adb: could not run '%s'", adb.c_str()); warned_missing = true; }
			if (!installing_) set_state(PhoneState::None, "");
			sleep_ms(10000);
			continue;
		}
		warned_missing = false;
		std::vector<Dev> devs = parse_devices(out);
		bool any_ready = false, any_unauth = false;
		std::string target;   // first authorised device: the one the card talks about
		std::map<std::string, Uint64> still;
		for (const Dev &d : devs) {
			if (d.state == "unauthorized") any_unauth = true;
			if (d.state != "device") continue;
			if (target.empty()) target = d.serial;
			if (!checked.count(d.serial) && !installing_) {
				std::string o3;
				int r3 = proc::run({adb, "-s", d.serial, "shell", "dumpsys", "package", "io.gbahandoff"}, kCmdTimeoutMs, o3);
				int v = r3 == 0 ? parse_version_code(o3) : kVersionError;
				if (v != kVersionError) {
					checked[d.serial] = v;
					SDL_Log("adb: %s app versionCode=%d (bundled %d)", d.serial.c_str(), v, GBAH_APP_VERSION_CODE);
				} else {
					SDL_Log("adb: could not read the app version of %s (rc=%d), will retry", d.serial.c_str(), r3);
				}
			}
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
		for (auto it = checked.begin(); it != checked.end();) {   // a replugged phone is asked again
			bool present = false;
			for (const Dev &d : devs) if (d.serial == it->first && d.state == "device") present = true;
			it = present ? std::next(it) : checked.erase(it);
		}
		if (!installing_) {
			if (!target.empty()) {
				auto c = checked.find(target);
				if (c != checked.end()) set_state(classify_app(c->second, GBAH_APP_VERSION_CODE), target);
				// else: version not read yet; keep the previous state
			} else {
				set_state(any_unauth ? PhoneState::Unauthorized : PhoneState::None, "");
			}
		}
		if (any_ready) st_.set_adb_note("aparelho detectado, abra o app no celular");
		else if (any_unauth) st_.set_adb_note("autorize a depuração USB no aparelho");
		else st_.set_adb_note("aguardando aparelho (cabo USB + depuração USB)");
		sleep_ms(kPollMs);
	}
}

} // namespace cable
