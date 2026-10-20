#!/bin/sh
# Stand-in for adb in the desktop integration test.
case "$1" in
  devices)
    printf 'List of devices attached\nFAKE123\tdevice\nUNAUTH9\tunauthorized\n\n'
    ;;
  -s)
    shift; serial="$1"; shift
    echo "$serial $*" >> "$ADB_FAKE_LOG"
    case "$*" in
      "shell dumpsys package"*)   # FAKE_PKG_VERSION unset = package not installed
        [ -n "$FAKE_PKG_VERSION" ] && printf 'Packages:\n  Package [io.gbahandoff] (abc):\n    versionCode=%s minSdk=26 targetSdk=36\n' "$FAKE_PKG_VERSION" ;;
      install*) echo "${FAKE_INSTALL:-Success}" ;;
    esac
    ;;
esac
exit 0
