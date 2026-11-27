#!/usr/bin/env bash
# check-asm.sh - asmble.md guard: all assembly must live under kernel/arch/.
#
# Two classes of violation, both fatal:
#   1. an assembly source file (.s / .S / .asm) outside kernel/arch/
#   2. inline assembly in a C source or header outside kernel/arch/
#      (__asm, __asm__, or an `asm volatile` / `asm(...)` statement)
#
# Comments that merely mention assembly are not flagged: the C pattern
# requires the compiler keyword, and the file pattern requires the extension.
#
# Usage:  bash tools/check-asm.sh [kernel-dir]
# Exit status: 0 = compliant, 1 = violation, 2 = nothing to check.
set -uo pipefail
ws="$(cd "$(dirname "$0")/.." && pwd)"
kdir="${1:-$ws/kernel}"

if [ ! -d "$kdir" ]; then
	echo "check-asm: no such directory: $kdir" >&2
	exit 2
fi
cd "$kdir" || exit 1

echo "asmble.md assembly placement check"
echo "  root: $kdir"

# 1) assembly source files outside arch/
files=$(find . -path ./arch -prune -o \
	\( -name '*.s' -o -name '*.S' -o -name '*.asm' \) -print 2>/dev/null | sort)

# 2) inline assembly in C / headers outside arch/
inline=$(grep -rnE --include='*.c' --include='*.h' \
	'(__asm__|__asm[[:space:]]|asm[[:space:]]+volatile|asm[[:space:]]*\()' . 2>/dev/null | \
	grep -v '^\./arch/' | grep -v '^Binary' || true)

bad=0
if [ -n "$files" ]; then
	echo ""
	echo "FAIL assembly source outside arch/:"
	printf '%s\n' "$files" | sed 's/^/  /'
	bad=1
fi
if [ -n "$inline" ]; then
	echo ""
	echo "FAIL inline assembly outside arch/:"
	printf '%s\n' "$inline" | sed 's/^/  /'
	echo ""
	echo "  Move the instruction sequence into kernel/arch/<arch>/ as a"
	echo "  function (see arch/i386/atomic.c, arch/i386/smp.c) and leave only"
	echo "  a prototype in the generic header."
	bad=1
fi

if [ "$bad" -ne 0 ]; then
	echo ""
	echo "check-asm: FAIL"
	exit 1
fi

n=$(find ./arch -type f \( -name '*.s' -o -name '*.S' -o -name '*.asm' \) | wc -l)
echo "  asm sources under arch/: $n"
echo "  asm source outside arch/: 0"
echo "  inline asm outside arch/: 0"
echo ""
echo "check-asm: PASS"
exit 0
