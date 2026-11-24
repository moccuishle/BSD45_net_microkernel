#!/usr/bin/env bash
# build_iso.sh - build the El Torito boot image and the BSD45-ELF-x86 ISO.
#
#	bash build_iso.sh [kernel.elf] [out.iso] [kernel_lba]
#
# The ISO is an ISO9660 volume with a 4 KiB no-emulation boot image
# (stage1 || stage2), and the kernel ELF appended verbatim at LBA 8192 - stage 2
# reads it from there, so the kernel is not part of the filesystem tree.
set -uo pipefail
ws="$(cd "$(dirname "$0")" && pwd)"
. "$ws/tools-find.sh"

kernel="${1:-$ws/build/bsd45.elf}"
out_iso="${2:-$ws/out/BSD45-ELF-x86.iso}"
# Keep in sync with build_iso.ps1 default (8192).
kernel_lba="${3:-8192}"
bld="$ws/build"
boot="$ws/boot"
out="$ws/out"
isodir="$bld/isodir"
toolchain_banner

mkiso="$(find_mkisofs)"
mkdir -p "$out"
rm -rf "$isodir"; mkdir -p "$isodir"

fsize() { stat -c%s "$1" 2>/dev/null || wc -c < "$1"; }

# build_stage <src> <text> <name> <entry> -> prints the flat binary path.
# Runs inside $(...) so it must `return`, never `exit`.
build_stage() {
	local src="$1" text="$2" name="$3" entry="$4"
	"$CC" "${TARGET32[@]}" "${CROSSBIN[@]}" -c -x assembler-with-cpp \
		-o "$bld/$name.o" "$src" || return 1
	if [ "$TOOLCHAIN" = mingw ]; then
		"$LD" -m i386pe -Ttext "$text" -e "$entry" --subsystem console \
			--disable-reloc-section -o "$bld/$name.img" "$bld/$name.o" 2>/dev/null || return 1
	else
		"$LD" -m elf_i386 -Ttext "$text" -e "$entry" -o "$bld/$name.img" \
			"$bld/$name.o" || return 1
	fi
	"$OBJCOPY" -O binary --only-section=.text "$bld/$name.img" "$bld/$name.bin" || return 1
	echo "$bld/$name.bin"
}

s1="$(build_stage "$boot/i386/stage1.s" 0x7C00  stage1 _start)" || { echo "stage1 build failed" >&2; exit 1; }
s2="$(build_stage "$boot/i386/stage2.s" 0x90000 stage2 _start32)" || { echo "stage2 build failed" >&2; exit 1; }
n1=$(fsize "$s1"); n2=$(fsize "$s2")
[ "$n1" -eq 512 ] || { echo "stage1 must be 512 bytes, got $n1" >&2; exit 1; }
echo "stage1 $n1 bytes, stage2 $n2 bytes"

# boot image = stage1 || stage2, padded to 4 KiB
img="$bld/boot.img"
dd if=/dev/zero of="$img" bs=4096 count=1 status=none
dd if="$s1" of="$img" bs=1 seek=0 conv=notrunc status=none
dd if="$s2" of="$img" bs=1 seek=512 conv=notrunc status=none

# patch the manifest: "BDMF" lba bytes stage2size
off=$(LC_ALL=C grep -abo -m1 'BDMF' "$img" 2>/dev/null | cut -d: -f1 || true)
[ -n "$off" ] || { echo "manifest magic not found" >&2; exit 1; }
# Little-endian dword writer.  The escapes are built as *text* first so no NUL
# byte ever has to survive a command substitution (8192 begins with 0x00).
put32() {  # put32 <value> <offset>
	local v=$(( $1 & 0xffffffff ))
	printf "\\x$(printf %02x $((v & 0xff)))\\x$(printf %02x $(((v >> 8) & 0xff)))\\x$(printf %02x $(((v >> 16) & 0xff)))\\x$(printf %02x $(((v >> 24) & 0xff)))" |
		dd of="$img" bs=1 seek="$2" conv=notrunc status=none
}
put32 "$kernel_lba" $((off + 4))
put32 "$(fsize "$kernel")" $((off + 8))
put32 "$n2" $((off + 12))

cp -f "$kernel" "$isodir/BSD45.ELF"
mkdir -p "$isodir/bin" "$isodir/sbin"
# Keep in sync with build_iso.ps1 and with the tool list in build_sh.sh.
for pair in "sh:bin/SH.ELF" "init:sbin/INIT.ELF" "hello:HELLO.ELF" "test:TEST.ELF" \
            "ls:bin/LS.ELF" "mkdir:bin/MKDIR.ELF" "cat:bin/CAT.ELF" "echo:bin/ECHO.ELF" \
            "kill:bin/KILL.ELF" "sleep:bin/SLEEP.ELF" "rm:bin/RM.ELF" "cp:bin/CP.ELF" \
            "mv:bin/MV.ELF" "wc:bin/WC.ELF" "grep:bin/GREP.ELF" "head:bin/HEAD.ELF" \
            "tail:bin/TAIL.ELF" "df:bin/DF.ELF" "mount:bin/MOUNT.ELF" "ps:bin/PS.ELF" \
            "uname:bin/UNAME.ELF" "true:bin/TRUE.ELF" "false:bin/FALSE.ELF" \
            "date:bin/DATE.ELF" "vmstat:bin/VMSTAT.ELF" \
            "mmaptest:bin/MMAPTEST.ELF" "subtest:bin/SUBTEST.ELF" \
            "pthread:bin/PTHREAD.ELF" "schedtest:bin/SCHEDTEST.ELF" "pthpreempt:bin/PTHPRE.ELF" \
            "pthmain:bin/PTHMAIN.ELF" \
            "spin:bin/SPIN.ELF" "cowtest:bin/COWTEST.ELF" \
            "crash:bin/CRASH.ELF" "badptr:bin/BADPTR.ELF" \
            "ipctest:bin/IPCTEST.ELF" \
            "fs_server:bin/FSSRV.ELF" "srvtest:bin/SRVTEST.ELF" \
            "devfs_server:bin/DEVFS.ELF" "devtest:bin/DEVTEST.ELF" \
            "shmtest:bin/SHMTEST.ELF" "vfstest:bin/VFSTEST.ELF" "proctest:bin/PROCTEST.ELF" \
            "netsrv:bin/NETSRV.ELF" "nettest:bin/NETTEST.ELF" \
            "aiotest:bin/AIOTEST.ELF" "uring:bin/uring.ELF"; do
	name="${pair%%:*}"; dst="${pair#*:}"
	[ -f "$ws/out/userland/$name.elf" ] && cp -f "$ws/out/userland/$name.elf" "$isodir/$dst"
done

# S-install: build the native-JLFS image that the in-guest `install` command
# writes to the VMware virtual disk, and ship it on the ISO as BSD45.JLFS.
# hdd/make_install_jlfs.sh -> mkjlfs.sh is a pure-Bash port; no python.
if bash "$ws/hdd/make_install_jlfs.sh" "$ws/hdd/BSD45.JLFS"; then
	jlfs_img="$ws/hdd/BSD45.JLFS"
	if [ -f "$jlfs_img" ]; then
		cp -f "$jlfs_img" "$isodir/BSD45.JLFS"
		echo "BSD45.JLFS: $(fsize "$jlfs_img") bytes"
	fi
else
	echo "ERROR: make_install_jlfs.sh failed, BSD45.JLFS not shipped" >&2
	exit 1
fi

cp -f "$img" "$isodir/boot.img"

# rootfs_overlay/ is copied on top of the ISO root (/etc/profile, /README.TXT)
if [ -d "$ws/rootfs_overlay" ]; then
	cp -a "$ws/rootfs_overlay/." "$isodir/"
fi
find "$isodir" -name .keep -delete 2>/dev/null

rm -f "$out_iso"
case "$(basename "$mkiso")" in
	xorriso) "$mkiso" -as mkisofs -quiet -iso-level 1 -V BSD45_ELF_X86 \
	             -b boot.img -c boot.cat -no-emul-boot -boot-load-size 8 \
	             -o "$out_iso" "$isodir" ;;
	*)       "$mkiso" -quiet -iso-level 1 -V BSD45_ELF_X86 \
	             -b boot.img -c boot.cat -no-emul-boot -boot-load-size 8 \
	             -o "$out_iso" "$isodir" ;;
esac || { echo "mkisofs failed" >&2; exit 1; }

# pad to the kernel LBA, append the ELF kernel, pad to a CD sector
iso_size=$(fsize "$out_iso")
target=$((kernel_lba * 2048))
[ "$iso_size" -le "$target" ] || { echo "ISO already larger than kernel LBA" >&2; exit 1; }
# Pad in 4 KiB blocks (bs=1 over ~8 MB would take millions of writes).
pad_bytes=$((target - iso_size))
pad_blocks=$((pad_bytes / 4096))
pad_rest=$((pad_bytes % 4096))
[ "$pad_blocks" -gt 0 ] && dd if=/dev/zero bs=4096 count="$pad_blocks" status=none >> "$out_iso"
[ "$pad_rest" -gt 0 ] && dd if=/dev/zero bs=1 count="$pad_rest" status=none >> "$out_iso"
cat "$kernel" >> "$out_iso"
kbytes=$(fsize "$kernel")
pad2=$(( (2048 - (kbytes % 2048)) % 2048 ))
[ "$pad2" -gt 0 ] && dd if=/dev/zero bs="$pad2" count=1 status=none >> "$out_iso"

final=$(fsize "$out_iso")
[ "$final" -ge $((target + kbytes)) ] ||
	{ echo "kernel injection failed: $final < $((target + kbytes))" >&2; exit 1; }

echo "ISO: $out_iso ($final bytes), kernel at LBA $kernel_lba"
