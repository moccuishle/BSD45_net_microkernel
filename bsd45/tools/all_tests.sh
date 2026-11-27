#!/usr/bin/env bash
# tools/all_tests.sh - run every acceptance suite, print one verdict per suite.
#
# Use this after a fresh clone:  bash tools/all_tests.sh
# It kills stray QEMU processes first, because a QEMU still holding
# out/BSD45-ELF-x86.iso makes build_iso.ps1 write a truncated ISO and then
# every boot test reports NO VERDICT.
set -uo pipefail
ws="$(cd "$(dirname "$0")/.." && pwd)"
iso="${1:-$ws/out/BSD45-ELF-x86.iso}"
cd "$ws" || exit 1

# 0) asmble.md pre-flight: no assembly may live outside kernel/arch/.
#    This is a source-tree rule, not a runtime one, so it fails before any
#    QEMU is started - a violation would mean the booted kernel is not the
#    one this tree describes.
if [ -x tools/check-asm.sh ] || [ -f tools/check-asm.sh ]; then
	if ! bash tools/check-asm.sh >/tmp/check-asm.out 2>&1; then
		cat /tmp/check-asm.out
		echo "all_tests: aborting, asmble.md violation (see above)" >&2
		exit 1
	fi
fi

# 1) stray QEMU (Windows) / qemu (Linux) that may hold the ISO
if command -v powershell.exe >/dev/null 2>&1; then
	powershell.exe -NoProfile -Command \
		"Get-Process qemu-system-i386 -ErrorAction SilentlyContinue | Stop-Process -Force" 2>/dev/null
else
	pkill -f qemu-system-i386 2>/dev/null
fi
sleep 1

if [ ! -f "$iso" ]; then
	echo "ISO missing: $iso" >&2
	echo "run: powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1" >&2
	exit 1
fi
size=$(wc -c <"$iso")
if [ "$size" -lt 4000000 ]; then
	echo "ISO looks truncated ($size bytes, expected >4 MB): rebuild it" >&2
	exit 1
fi

# 2) run the suites
if command -v powershell.exe >/dev/null 2>&1; then
	run() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "tools/$1.ps1" 2>&1; }
else
	run() { bash "tools/$1.sh" 2>&1; }
fi

fail=0
for t in test_s3 test_s4 test_proc test_tty test_s7 test_s8 test_s9 test_s10 \
         test_sig test_s10_3 test_s10_4 test_dev test_tools test_mount test_ps \
         test_cache test_mmap test_subsys test_kthread \
         test_pthread test_wpath \
         test_ipl test_preempt test_cow test_fault test_badptr; do
	out=$(run "$t")
	v=$(printf '%s' "$out" | grep -E 'PASSED|FAILED' | tail -1)
	printf '%-12s %s\n' "$t" "${v:-NO VERDICT}"
	case "$v" in *PASSED*) ;; *) fail=$((fail + 1));; esac
done
echo "---- suites failed: $fail"
[ "$fail" -ne 0 ] && exit 1
echo "ALL SUITES PASSED"
exit 0
