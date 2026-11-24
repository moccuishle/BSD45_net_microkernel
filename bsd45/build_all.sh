#!/usr/bin/env bash
# build_all.sh - userland -> /bin/sh -> /test -> kernel -> ISO.
#
# Mirrors build_all.ps1.  Compile-time knobs come from the environment (the
# PowerShell equivalents are -Unicode / -Define):
#   KERNEL_UNICODE=1      build kernel strings as UTF-16   (PS: -Unicode)
#   KERNEL_DEFINE=A,B     comma-separated -D macros        (PS: -Define A,B)
#   BSD45_TOOLCHAIN=native|mingw                           (no PS equivalent)
#
#	bash build_all.sh                 # everything
#	bash build_all.sh --no-sh         # keep the tiny libexec/sh.c stub
#	bash build_all.sh -D S10_AUTOTEST -u
set -euo pipefail
ws="$(cd "$(dirname "$0")" && pwd)"

build_sh=1
while [ $# -gt 0 ]; do
	case "$1" in
		-D|--define)
			if [ $# -lt 2 ]; then echo "build_all.sh: $1 needs a value" >&2; exit 2; fi
			KERNEL_DEFINE="$2"; export KERNEL_DEFINE; shift 2 ;;
		-u|--unicode) KERNEL_UNICODE=1; export KERNEL_UNICODE; shift ;;
		--no-sh) build_sh=0; shift ;;
		*) echo "build_all.sh: unknown option $1" >&2; shift ;;
	esac
done

bash "$ws/build_userland.sh"

if [ "$build_sh" -eq 1 ]; then
	# S3: the real 4.5BSD /bin/sh (overwrites out/userland/sh.elf), plus the
	# small tools (/bin/echo, /bin/ls, ...) linked against the same libc.
	bash "$ws/build_sh.sh"
	[ -f "$ws/out/userland/sh.elf" ] || { echo "sh build failed" >&2; exit 1; }
	# /test: the process/tty/tmpfs/proc self test the monitor runs.
	bash "$ws/build_sh.sh" -t test
fi

bash "$ws/build_kernel.sh"
bash "$ws/build_iso.sh"

echo
echo "Artifacts:"
ls -l "$ws/out" | sed 's/^/  /'
