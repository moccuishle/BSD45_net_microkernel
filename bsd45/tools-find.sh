#!/usr/bin/env bash
# tools-find.sh - locate the toolchain the Bash build uses.
#
#	Sourced by every build_*.sh:   . "$ws/tools-find.sh"
#
# There are two ways to produce the same artifacts, and both are supported:
#
#   native   gcc -m32 emits ELF32 directly; linked with boot/i386/kernel.ld.
#            This is the path LINUX-BUILD.md documents; it needs gcc-multilib.
#
#   mingw    gcc -m32 emits COFF; ld -m i386pe produces a PE image which
#            objcopy rewrites as elf32-i386.  This is exactly what the *.ps1
#            scripts do, so the same command lines work on Windows/MSYS2 and
#            on Linux with gcc-mingw-w64-i686 installed.
#
# Selection:
#   BSD45_TOOLCHAIN=native|mingw   force one
#   BSD45_MINGW_PREFIX=i686-w64-mingw32-   tool prefix (MINGW_PREFIX is used
#                                          by MSYS2 itself, hence the name)
#   default: native when `gcc -m32` really emits ELF32 for i386, else mingw
#
# Defines:
#   TOOLCHAIN  native|mingw
#   CC LD OBJCOPY OBJDUMP   target toolchain (command names, no paths)
#   HOSTCC                  compiler for generator programs (genassym, mknodes)
#   HOSTEXE                 "" or ".exe" - suffix host binaries get here
#   LEADUS[]                -fleading-underscore on ELF, empty on PE (the PE
#                           assembler already expects the "_" prefix)
#   CROSSBIN[]              -B <dir> pinning gcc to its own as/ld (Windows
#                           only: keeps a stale SysWOW64 zlib1.dll away from
#                           the 32-bit assembler)
set -u

have() { command -v "$1" >/dev/null 2>&1; }

case "$(uname -s 2>/dev/null)" in
	MSYS*|MINGW*|CYGWIN*) BSD45_OS=windows ;;
	*)                    BSD45_OS=unix ;;
esac
if [ "$BSD45_OS" = windows ]; then HOSTEXE=".exe"; else HOSTEXE=""; fi

HOSTCC="${HOSTCC:-$(command -v cc 2>/dev/null || command -v gcc 2>/dev/null || true)}"

# Does the local gcc/ld pair produce real ELF32 for i386?  On Windows/MSYS2 the
# answer is no: -m32 gives PE-i386 and ld has no elf_i386 emulation.
probe_native() {
	local tmp rc=1
	have gcc && have ld && have objdump || return 1
	tmp="$(mktemp -d "${TMPDIR:-/tmp}/bsd45probe.XXXXXX" 2>/dev/null)" || return 1
	printf 'int main(void){return 0;}\n' > "$tmp/t.c"
	if gcc -m32 -fno-pie -c -o "$tmp/t.o" "$tmp/t.c" 2>/dev/null &&
	   objdump -f "$tmp/t.o" 2>/dev/null | grep -qi 'elf32-i386' &&
	   gcc -m32 -fno-pie -no-pie -o "$tmp/t" "$tmp/t.o" 2>/dev/null; then
		rc=0
	fi
	rm -rf "$tmp"
	return $rc
}

setup_native() {
	TOOLCHAIN=native
	CC="${CC:-gcc}"
	LD="${LD:-ld}"
	OBJCOPY="${OBJCOPY:-objcopy}"
	OBJDUMP="${OBJDUMP:-objdump}"
	LEADUS=(-fleading-underscore)
	CROSSBIN=()
}

# Resolve one tool of the cross toolchain.  A Linux cross install ships the
# prefixed names (i686-w64-mingw32-ld); MSYS2's mingw32 package ships the
# unprefixed ones (ld.exe) next to i686-w64-mingw32-gcc.exe, so on Windows the
# bare name is the right answer - elsewhere it would be the host linker.
mingw_tool() {
	local pref="$1" bare="$2"
	if have "${p}${pref}"; then echo "${p}${pref}"; return 0; fi
	if [ "$BSD45_OS" = windows ] && have "$bare"; then echo "$bare"; return 0; fi
	return 1
}

setup_mingw() {
	local p="${BSD45_MINGW_PREFIX:-i686-w64-mingw32-}" missing=""
	if ! have "${p}gcc"; then
		if [ -n "${MINGW_BIN:-}" ] && [ -x "$MINGW_BIN/${p}gcc${HOSTEXE}" ]; then
			PATH="$MINGW_BIN:$PATH"
			export PATH
		else
			cat >&2 <<-EOF
			No i686-w64-mingw32 toolchain found (looked for ${p}gcc).
			Install one:
			  Ubuntu/Debian: sudo apt install gcc-mingw-w64-i686 binutils-mingw-w64-i686
			  MSYS2:         pacman -S mingw-w64-i686-gcc
			or set MINGW_BIN to the directory holding ${p}gcc.
			EOF
			exit 1
		fi
	fi
	TOOLCHAIN=mingw
	CC="${p}gcc"
	LD="$(mingw_tool ld ld)"         || missing="$missing ld"
	OBJCOPY="$(mingw_tool objcopy objcopy)" || missing="$missing objcopy"
	OBJDUMP="$(mingw_tool objdump objdump)" || missing="$missing objdump"
	if [ -n "$missing" ]; then
		echo "the mingw toolchain has no:$missing (install binutils-mingw-w64-i686)" >&2
		exit 1
	fi
	LEADUS=()
	CROSSBIN=()
	# The 32-bit assembler on 64-bit Windows picks up a SysWOW64 zlib1.dll and
	# dies quietly; -B at the toolchain's own bin/ avoids it.  build_kernel.ps1
	# passes the same thing (Get-CcBinArg).
	if [ "$BSD45_OS" = windows ]; then
		local dir
		dir="$(dirname "$(command -v "${p}gcc")")"
		dir="$(cd "$dir" && pwd)"
		CROSSBIN=(-B "$dir/")
	fi
}

case "${BSD45_TOOLCHAIN:-auto}" in
	native) setup_native ;;
	mingw)  setup_mingw ;;
	auto)
		if probe_native; then setup_native; else setup_mingw; fi
		;;
	*) echo "BSD45_TOOLCHAIN must be native, mingw or auto" >&2; exit 2 ;;
esac

# -m32 for the target on both paths.
TARGET32=(-m32)

find_gcc_include() {
	"$CC" -print-file-name=include 2>/dev/null | head -1
}

find_python() {
	local c
	if [ -n "${PYTHON:-}" ] && [ -x "$PYTHON" ]; then echo "$PYTHON"; return 0; fi
	for c in python3 python; do
		if have "$c" && [ "$("$c" -c 'print(1)' 2>/dev/null)" = "1" ]; then
			command -v "$c"; return 0
		fi
	done
	return 1
}

find_mkisofs() {
	local c
	if [ -n "${MKISOFS:-}" ] && [ -x "$MKISOFS" ]; then echo "$MKISOFS"; return 0; fi
	for c in mkisofs genisoimage xorriso; do
		if have "$c"; then command -v "$c"; return 0; fi
	done
	echo "mkisofs/genisoimage/xorriso not found; install genisoimage or xorriso" >&2
	return 1
}

find_qemu() {
	local c
	if [ -n "${QEMU:-}" ] && [ -x "$QEMU" ]; then echo "$QEMU"; return 0; fi
	for c in qemu-system-i386 qemu-system-i386.exe; do
		if have "$c"; then command -v "$c"; return 0; fi
	done
	return 1
}

require_tools() {
	local t
	for t in "$@"; do
		have "$t" || { echo "missing tool: $t" >&2; exit 1; }
	done
}

# One line at the top of every log, so an artifact can always be traced back to
# the toolchain that produced it.
toolchain_banner() { echo "toolchain: $TOOLCHAIN ($CC -> $LD)"; }

require_tools "$CC" "$LD" "$OBJCOPY" "$OBJDUMP"
[ -n "$HOSTCC" ] || { echo "no host C compiler found (set HOSTCC)" >&2; exit 1; }
