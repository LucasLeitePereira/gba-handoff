#!/usr/bin/env bash
# Downloads Google's official Android platform-tools for Windows and copies ONLY
# adb.exe + AdbWinApi.dll + AdbWinUsbApi.dll (+ NOTICE.txt, the license text that
# must travel with them) into <dest>/adb/, i.e. next to gbahandoff.exe.
#
#   tools/fetch-adb-windows.sh [dest-dir]      default: build-win/desktop
#
# The binaries are NOT versioned (see .gitignore). Redistributing adb is allowed
# by its license (Apache-2.0 for adb itself; see NOTICE.txt) as long as the notice ships along.
# Optional: ADB_ZIP_SHA256=<hex> makes the script refuse a zip with a different hash.
set -euo pipefail

URL="https://dl.google.com/android/repository/platform-tools-latest-windows.zip"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$ROOT/build-win/desktop}/adb"
FILES=(adb.exe AdbWinApi.dll AdbWinUsbApi.dll NOTICE.txt)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL"
if command -v curl >/dev/null; then curl -fL --retry 3 -o "$TMP/pt.zip" "$URL"
else wget -O "$TMP/pt.zip" "$URL"; fi

SUM="$(sha256sum "$TMP/pt.zip" | cut -d' ' -f1)"
echo "sha256 of the zip: $SUM"
if [ -n "${ADB_ZIP_SHA256:-}" ] && [ "$ADB_ZIP_SHA256" != "$SUM" ]; then
  echo "ERROR: sha256 differs from ADB_ZIP_SHA256" >&2; exit 1
fi

mkdir -p "$DEST" "$TMP/x"
if command -v unzip >/dev/null; then
  unzip -q -j "$TMP/pt.zip" $(printf 'platform-tools/%s ' "${FILES[@]}") -d "$TMP/x"
else
  python3 -I - "$TMP/pt.zip" "$TMP/x" "${FILES[@]}" <<'PY'
import sys, zipfile
z, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
with zipfile.ZipFile(z) as f:
    for n in names:
        open(f"{out}/{n}", "wb").write(f.read(f"platform-tools/{n}"))
PY
fi

for f in "${FILES[@]}"; do
  [ -s "$TMP/x/$f" ] || { echo "ERROR: $f missing in the zip" >&2; exit 1; }
  cp "$TMP/x/$f" "$DEST/$f"
done
echo "Installed into $DEST:"
ls -l "$DEST"
