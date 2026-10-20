// Pure logic of the "is the phone ready?" card: parses adb output, no SDL, no processes, unit-tested
// (tests/adbstate_test.cpp). adb.cpp does the I/O.
#pragma once
#include <string>

namespace cable {

enum class PhoneState {
	None,           // no device attached
	Unauthorized,   // attached, USB debugging not accepted on the phone yet
	NoApp,          // authorised, io.gbahandoff not installed
	Outdated,       // installed versionCode < the one bundled with this exe
	Ready,          // installed and up to date
	Installing      // `adb install` running
};

enum { kVersionAbsent = -1, kVersionError = -2 };

// `adb shell dumpsys package io.gbahandoff` -> installed versionCode (>= 0), kVersionAbsent (package not
// installed) or kVersionError (adb itself failed: offline, no device... -> retry later, do not cache).
int parse_version_code(const std::string &dumpsys);

// installed: result of parse_version_code (not kVersionError). expected: GBAH_APP_VERSION_CODE.
PhoneState classify_app(int installed, int expected);

struct InstallResult {
	bool ok = false;
	std::string message;   // short, ASCII, shown on the card when !ok
};
// rc/out as returned by proc::run for `adb install -r`. NEVER suggests uninstalling: that wipes the save.
InstallResult parse_install_result(int rc, const std::string &out);

} // namespace cable
