#!/bin/sh
# Rebuilds gbah_test.gba from gbah_test.s. Needs binutils-arm-none-eabi
# (apt-get download + dpkg -x works without root; put its bin/ on PATH).
set -e
cd "$(dirname "$0")"
# $1 = output name, rest = extra `as` args (e.g. --defsym LATE=1)
build() {
	out=$1; shift
	arm-none-eabi-as -mcpu=arm7tdmi "$@" -o "$out.o" gbah_test.s
	arm-none-eabi-ld -Ttext=0x08000000 --entry=_start -o "$out.elf" "$out.o"
	arm-none-eabi-objcopy -O binary "$out.elf" "$out.gba"
	python3 -I - "$out.gba" <<'PY'
import sys
d = bytearray(open(sys.argv[1], "rb").read())
d += b"\0" * (-len(d) % 4)
d[0xBD] = (-(sum(d[0xA0:0xBD]) + 0x19)) & 0xFF   # header complement check
open(sys.argv[1], "wb").write(d)
PY
	rm -f "$out.o" "$out.elf"
	ls -l "$out.gba"
}
build gbah_test
build gbah_test_late --defsym LATE=1
