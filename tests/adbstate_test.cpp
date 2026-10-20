// Unit test of the pure adb-output parsing behind the menu's phone card (desktop/adbstate.cpp).
#include "adbstate.h"

#include <cstdio>
#include <string>

using namespace cable;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *kInstalled2 =
	"Activity Resolver Table:\n  Non-Data Actions:\n      android.intent.action.MAIN:\n"
	"Packages:\n  Package [io.gbahandoff] (a1b2c3d):\n    userId=10234\n    pkg=Package{e4f5 io.gbahandoff}\n"
	"    codePath=/data/app/~~x==/io.gbahandoff-y==\n    versionCode=2 minSdk=26 targetSdk=36\n"
	"    versionName=0.2\n    flags=[ HAS_CODE ALLOW_CLEAR_USER_DATA ]\n";

int main()
{
	// installed, current and old
	CHECK(parse_version_code(kInstalled2) == 2);
	std::string old = kInstalled2;
	old.replace(old.find("versionCode=2"), 13, "versionCode=1");
	CHECK(parse_version_code(old) == 1);
	// another package's versionCode before ours must not win
	CHECK(parse_version_code("Package [com.other] (x):\n  versionCode=99 minSdk=1\n"
	                         "Package [io.gbahandoff] (y):\n  versionCode=7 minSdk=26\n") == 7);
	// CRLF (adb on Windows)
	CHECK(parse_version_code("Packages:\r\n  Package [io.gbahandoff] (a):\r\n    versionCode=3 minSdk=26\r\n") == 3);
	// absent: old and new Android flavours of "nothing"
	CHECK(parse_version_code("Unable to find package: io.gbahandoff\n") == kVersionAbsent);
	CHECK(parse_version_code("") == kVersionAbsent);
	CHECK(parse_version_code("Dexopt state:\nPackages:\n") == kVersionAbsent);
	// adb's own failure is not "absent" (would offer a pointless install): retry later
	CHECK(parse_version_code("error: device offline\n") == kVersionError);
	CHECK(parse_version_code("error: no devices/emulators found\n") == kVersionError);
	CHECK(parse_version_code("adb: device unauthorized.\n") == kVersionError);
	CHECK(parse_version_code("  versionCode=abc\n") == kVersionAbsent);

	CHECK(classify_app(kVersionAbsent, 2) == PhoneState::NoApp);
	CHECK(classify_app(1, 2) == PhoneState::Outdated);
	CHECK(classify_app(2, 2) == PhoneState::Ready);
	CHECK(classify_app(3, 2) == PhoneState::Ready);   // newer on the phone: never downgrade

	// install results
	InstallResult r = parse_install_result(0, "Performing Streamed Install\nSuccess\n");
	CHECK(r.ok && r.message.empty());
	r = parse_install_result(1, "Performing Streamed Install\nadb: failed to install x.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: "
	                            "Existing package io.gbahandoff signatures do not match newer version]\n");
	CHECK(!r.ok);
	CHECK(r.message.find("NAO desinstale") != std::string::npos);
	CHECK(r.message.find("assinatura") != std::string::npos);
	r = parse_install_result(1, "Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]");
	CHECK(!r.ok && r.message.find("NAO desinstale") != std::string::npos);
	r = parse_install_result(1, "Failure [INSTALL_FAILED_SOMETHING_NEW: x]");
	CHECK(!r.ok && r.message.find("INSTALL_FAILED_SOMETHING_NEW") != std::string::npos
	      && r.message.find("NAO desinstale") != std::string::npos);
	r = parse_install_result(1, "garbage");
	CHECK(!r.ok && r.message.find("NAO desinstale") != std::string::npos);
	r = parse_install_result(-1, "");
	CHECK(!r.ok && !r.message.empty());
	r = parse_install_result(-2, "");
	CHECK(!r.ok && !r.message.empty());
	r = parse_install_result(1, "Success");   // a non-zero exit never counts as success
	CHECK(!r.ok);

	if (fails) { printf("%d failure(s)\n", fails); return 1; }
	printf("adbstate_test: ok\n");
	return 0;
}
