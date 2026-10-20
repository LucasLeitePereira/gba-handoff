#!/bin/bash
# Runs a sanitizer test. ASan on WSL2/new kernels sometimes dies at startup in an endless
# "AddressSanitizer:DEADLYSIGNAL" loop with no report (ASLR vs shadow map; it hung ctest).
# That signature (no "ERROR: AddressSanitizer", no "runtime error") is retried; any real
# finding or test failure is returned as is.
out=$(mktemp)
trap 'rm -f "$out"' EXIT
for try in 1 2 3 4 5 6; do
	timeout 90 "$@" 2>&1 | head -c 2000000 > "$out"
	rc=${PIPESTATUS[0]}
	if grep -q DEADLYSIGNAL "$out" && ! grep -q -e "ERROR: AddressSanitizer" -e "runtime error" "$out"; then
		echo "run_san: ASan startup glitch (try $try), retrying" >&2
		continue
	fi
	cat "$out"
	exit "$rc"
done
echo "run_san: ASan never started" >&2
exit 1
