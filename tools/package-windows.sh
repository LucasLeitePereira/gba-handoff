#!/usr/bin/env bash
# Builds dist/GBAHandoff-windows.zip: a GBAHandoff/ folder with gbahandoff.exe, adb/, GBAHandoff.apk and LEIA-ME.txt.
#
#   tools/package-windows.sh            (needs build-win/desktop/gbahandoff.exe: cmake --build --preset build-win)
#
# adb comes from tools/fetch-adb-windows.sh (downloaded if not there yet). The APK is copied from the Gradle
# debug build if it exists; this script never runs Gradle. Without the APK the zip still works, but the
# exe cannot install the phone app ("GBAHandoff.apk nao encontrado").
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$ROOT/build-win/desktop/gbahandoff.exe"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
DIST="${GBAH_DIST:-$ROOT/dist}"   # override for tests
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
# Staged outside dist/: dist/GBAHandoff-windows/ is where the user unzips and runs the package
# (its adb.exe may be running and locked), so it must never be deleted here.
PKG="$STAGE/GBAHandoff"
ZIP="$DIST/GBAHandoff-windows.zip"

[ -f "$EXE" ] || { echo "ERROR: $EXE not found. Run: cmake --preset build-win && cmake --build --preset build-win" >&2; exit 1; }

if [ ! -f "$ROOT/build-win/desktop/adb/adb.exe" ]; then
  "$ROOT/tools/fetch-adb-windows.sh" "$ROOT/build-win/desktop"
fi

mkdir -p "$DIST"; rm -f "$ZIP"
mkdir -p "$PKG/adb"
cp "$EXE" "$PKG/gbahandoff.exe"
cp "$ROOT"/build-win/desktop/adb/* "$PKG/adb/"
if [ -f "$APK" ]; then
  cp "$APK" "$PKG/GBAHandoff.apk"
else
  echo "WARNING: $APK not found: the zip will have NO GBAHandoff.apk (build the Android app first)." >&2
fi

# CRLF so Notepad shows the lines properly.
printf '%s\r\n' \
  "1. Abra o gbahandoff.exe (nao precisa instalar nada no PC)." \
  "2. No celular: Configuracoes > Opcoes do desenvolvedor > ative a Depuracao USB." \
  "3. Plugue o celular no PC pelo cabo USB e aceite a depuracao quando o celular perguntar." \
  "4. No gbahandoff, clique em Instalar (ou Atualizar) para colocar o app no celular." \
  "5. Jogue: arraste uma ROM .gba para a janela e Esc abre o menu do jogo." \
  > "$PKG/LEIA-ME.txt"

( cd "$STAGE" && if command -v zip >/dev/null; then zip -qr "$ZIP" GBAHandoff
  else python3 -I - "$ZIP" <<'PY'
import os, sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, fs in os.walk("GBAHandoff"):
        for f in fs:
            z.write(os.path.join(d, f))
PY
  fi )
echo "Created $ZIP"
ls -l "$ZIP"
