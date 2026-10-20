#!/bin/sh
# Stand-in for adb in the desktop integration test.
case "$1" in
  devices)
    printf 'List of devices attached\nFAKE123\tdevice\nUNAUTH9\tunauthorized\n\n'
    ;;
  -s)
    shift; serial="$1"; shift
    echo "$serial $*" >> "$ADB_FAKE_LOG"
    ;;
esac
exit 0
