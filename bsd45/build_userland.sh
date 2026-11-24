#!/usr/bin/env bash
# build_userland.sh - build the small i386 ELF user programs (hello/init/sh).
#
#	bash build_userland.sh [outdir]
#
# Output: out/userland/{hello,init,sh}.elf.  The kernel's execve only accepts a
# static ET_EXEC ELF32 with no interpreter, hence -nostdlib and -e _start.
set -euo pipefail
ws="$(cd "$(dirname "$0")" && pwd)"
. "$ws/tools-find.sh"
out="${1:-$ws/out/userland}"
mkdir -p "$out"
toolchain_banner

CFLAGS=("${TARGET32[@]}" "${CROSSBIN[@]}" "${LEADUS[@]}"
	-O2 -ffreestanding -fno-pie -fno-pic
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
	-nostdlib -nostdinc -march=i386 -mno-sse -mno-sse2 -mno-mmx -mno-avx
	-mpreferred-stack-boundary=2)

build_user_elf() {
	local src="$1" name="$2"
	"$CC" "${CFLAGS[@]}" -c -o "$out/$name.o" "$src"
	if [ "$TOOLCHAIN" = mingw ]; then
		# COFF -> PE -> ELF, exactly like build_userland.ps1.
		"$LD" -m i386pe -Ttext 0x08048000 -e _start --subsystem console \
			--disable-reloc-section -o "$out/$name.pe" "$out/$name.o"
		"$OBJCOPY" -O elf32-i386 -R .idata -R .reloc "$out/$name.pe" "$out/$name.elf"
	else
		"$LD" -m elf_i386 -Ttext 0x08048000 -e _start \
			-o "$out/$name.elf" "$out/$name.o"
	fi
	echo "  $name.elf"
}

echo "building userland ELF32"
build_user_elf "$ws/libexec/hello.c" hello
build_user_elf "$ws/libexec/init.c"  init
build_user_elf "$ws/libexec/sh.c"    sh
echo "done -> $out"
