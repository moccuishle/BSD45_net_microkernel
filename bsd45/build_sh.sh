#!/usr/bin/env bash
# build_sh.sh - build the 4.5BSD /bin/sh, the small tools and the libc test as
# static i386 ELF for BSD45-ELF-x86.
#
#	bash build_sh.sh              # sh.elf + every libexec tool
#	bash build_sh.sh -t test      # test.elf (the libc self test)
#
# Sources: 4.5BSD-Lite2 usr/src/bin/sh plus a port of the libc subset it needs
# (libc/src/libc_sys.c supplies the int 0x80 syscall layer).  Mirrors
# build_sh.ps1; on the mingw path every image goes COFF -> PE -> objcopy ELF.
set -uo pipefail
ws="$(cd "$(dirname "$0")" && pwd)"
. "$ws/tools-find.sh"

target="sh"
while [ $# -gt 0 ]; do
	case "$1" in
		-t|--target) target="${2:-sh}"; shift 2 ;;
		*) echo "usage: $0 [-t sh|test]" >&2; exit 2 ;;
	esac
done

usr="$ws/libc/arch/bsd"
shsrc="$usr/bin/sh"
lc="$usr/lib/libc"
bld="$ws/build"
incu="$bld/incu"
gen="$bld/shgen"
obj="$bld/shobj"
outd="$ws/out/userland"
mkdir -p "$bld" "$incu" "$gen" "$obj" "$outd"
toolchain_banner

step() { echo "== $1"; }
fail=0

prepare_headers() {
	mkdir -p "$incu/sys" "$incu/machine" "$incu/net"
	cp -f "$ws/libc/include/stdarg.h" "$incu/stdarg.h"
	cp -f "$ws/libc/include/ctype.h"  "$incu/ctype.h"
	cp -f "$ws/libc/include/histedit.h" "$incu/histedit.h"
	cp -f "$ws/libc/include/pthread.h" "$incu/pthread.h"
	cp -f "$ws/libc/include/uring.h" "$incu/uring.h"
	cp -f "$ws/libc/include/syscall_arch.h" "$incu/syscall_arch.h"
	cp -f "$ws/libc/include/srv.h" "$incu/srv.h"
	cp -f "$ws/libc/include/dev.h" "$incu/dev.h"
	cp -f "$ws/libc/include/shmbuf.h" "$incu/shmbuf.h"
	cp -f "$ws/libc/include/net.h" "$incu/net.h"
	[ -f "$incu/errno.h" ] && return 0
	step "preparing libc header view"
	# Mirror build_sh.ps1 exactly: 4.5BSD usr/include, sys/sys (also at the
	# top level, without overwriting), sys/i386/include -> machine/, sys/net,
	# and the hp300 fallbacks for ansi/limits/float.
	cp -f "$usr/include/"*.h "$incu/" 2>/dev/null
	for h in "$usr/sys/sys/"*.h; do
		dst="$incu/$(basename "$h")"
		[ -e "$dst" ] || cp -f "$h" "$dst"
	done
	cp -f "$usr/sys/sys/"*.h "$incu/sys/" 2>/dev/null
	cp -f "$usr/sys/i386/include/"*.h "$incu/machine/" 2>/dev/null
	cp -f "$usr/sys/net/"*.h "$incu/net/" 2>/dev/null
	cp -f "$usr/sys/hp300/include/ansi.h"   "$incu/machine/ansi.h"
	cp -f "$usr/sys/hp300/include/limits.h" "$incu/machine/limits.h"
	cp -f "$usr/sys/hp300/include/float.h"  "$incu/machine/float.h"
	cp -f "$usr/sys/sys/termios.h" "$incu/termios.h"
	# The 4.5BSD usr/include copies above overwrite these; put the project's
	# versions back on top, exactly like build_sh.ps1 does.
	cp -f "$ws/libc/include/stdarg.h" "$incu/stdarg.h"
	cp -f "$ws/libc/include/ctype.h"  "$incu/ctype.h"
	cp -f "$ws/libc/include/histedit.h" "$incu/histedit.h"
	cp -f "$ws/libc/include/pthread.h" "$incu/pthread.h"
	cp -f "$ws/libc/include/uring.h" "$incu/uring.h"
	cp -f "$ws/libc/include/syscall_arch.h" "$incu/syscall_arch.h"
}

prepare_headers

common=("${TARGET32[@]}" "${CROSSBIN[@]}" "${LEADUS[@]}"
	-std=gnu89 -O2 -ffreestanding -fno-builtin
	-fno-pie -fno-pic -fno-stack-protector
	-fno-asynchronous-unwind-tables -fno-unwind-tables
	-fno-strict-aliasing -fcommon
	-march=i386 -mno-sse -mno-sse2 -mno-mmx -mno-avx
	-mpreferred-stack-boundary=2
	-nostdinc -I "$incu" -I "$gen" -I "$shsrc"
	-isystem "$(find_gcc_include)" -DSHELL)

compile_c() {    # compile_c <src> <out> [extra flags...]
	local src="$1" out="$2"; shift 2
	if [ ! -f "$src" ]; then echo "  missing $src"; fail=$((fail+1)); return 1; fi
	local log
	log="$("$CC" "${common[@]}" "$@" -c -o "$out" "$src" 2>&1)"
	if [ $? -ne 0 ]; then
		echo "FAIL $(basename "$src")"
		echo "$log" | head -12 | sed 's/^/   /'
		fail=$((fail+1)); return 1
	fi
	return 0
}

compile_asm() {
	local src="$1" out="$2" log
	log="$("$CC" "${TARGET32[@]}" "${CROSSBIN[@]}" -x assembler-with-cpp -nostdinc \
		-I "$incu" -c -o "$out" "$src" 2>&1)"
	if [ $? -ne 0 ]; then
		echo "FAIL $(basename "$src")"
		echo "$log" | head -12 | sed 's/^/   /'
		fail=$((fail+1)); return 1
	fi
	return 0
}

objs=()
add() { objs+=("$1"); }

step "startup objects"
# Mirror build_sh.ps1: read the asm-y list from libc/arch/x86/config.mk and
# compile each from libc/arch/x86/.  chkstk_ms.s is added separately below.
libc_arch="$ws/libc/arch/x86"
arch_cfg="$libc_arch/config.mk"
asm_srcs=()
if [ -f "$arch_cfg" ]; then
	while IFS= read -r line; do
		case "$line" in \#*) continue ;; esac
		case "$line" in
			*asm-y*+=*)
				a="${line#*=}"
				a="${a#"${a%%[![:space:]]*}"}"
				a="${a%"${a##*[![:space:]]}"}"
				[ -n "$a" ] && asm_srcs+=("$a")
				;;
		esac
	done < "$arch_cfg"
fi
for af in ${asm_srcs[@]+"${asm_srcs[@]}"}; do
	[ "$af" = "chkstk_ms.s" ] && continue
	base="${af%.*}"
	oname="$base.o"
	[ "$base" = "pthread" ] && oname="pthread_asm.o"
	compile_asm "$libc_arch/$af" "$obj/$oname" && add "$obj/$oname"
done

step "port glue"
compile_c "$ws/libc/src/libc_sys.c"      "$obj/libc_sys.o"      && add "$obj/libc_sys.o"
compile_c "$ws/libc/src/pthread.c"       "$obj/pthread_lib.o"   && add "$obj/pthread_lib.o"
compile_c "$ws/libc/src/dtoa.c"          "$obj/dtoa.o"          && add "$obj/dtoa.o"
compile_c "$ws/libc/src/histedit_stub.c" "$obj/histedit_stub.o" && add "$obj/histedit_stub.o"

step "4.5BSD libc subset"
libc_files=(
	string/strcat.c string/strcmp.c string/strcpy.c
	string/strcspn.c string/strdup.c string/strlen.c string/strncat.c
	string/strncmp.c string/strncpy.c string/strpbrk.c
	string/strsep.c string/strspn.c string/strstr.c string/strtok.c
	string/strcasecmp.c string/strerror.c
	string/memchr.c string/memcmp.c
	stdlib/atoi.c stdlib/atol.c stdlib/strtol.c
	stdlib/strtoul.c stdlib/malloc.c
	stdlib/calloc.c stdlib/exit.c stdlib/atexit.c stdlib/getenv.c
	stdlib/qsort.c stdlib/abs.c stdlib/div.c stdlib/ldiv.c stdlib/rand.c
	stdlib/abort.c stdlib/getopt.c gen/setmode.c gen/wait3.c gen/exec.c
	stdio/findfp.c stdio/flags.c stdio/fclose.c stdio/fopen.c
	stdio/fprintf.c stdio/fgets.c stdio/fputs.c stdio/fread.c
	stdio/fwrite.c stdio/fputc.c stdio/putc.c stdio/putchar.c
	stdio/printf.c stdio/vfprintf.c stdio/sprintf.c stdio/snprintf.c
	stdio/vsnprintf.c stdio/vsprintf.c stdio/perror.c stdio/fflush.c
	stdio/refill.c stdio/rget.c stdio/wbuf.c stdio/wsetup.c
	stdio/makebuf.c stdio/setvbuf.c stdio/fwalk.c stdio/fpurge.c
	stdio/fseek.c stdio/ftell.c stdio/rewind.c stdio/fdopen.c
	stdio/fileno.c stdio/feof.c stdio/ferror.c stdio/clrerr.c
	stdio/ungetc.c stdio/gets.c stdio/puts.c stdio/getc.c
	stdio/getchar.c stdio/fgetc.c
	gen/isatty.c gen/opendir.c gen/readdir.c gen/closedir.c
	gen/telldir.c gen/seekdir.c gen/rewinddir.c
	gen/glob.c gen/fnmatch.c
	stdio/stdio.c stdio/fvwrite.c
)
for f in "${libc_files[@]}"; do
	o="$obj/lc_$(basename "${f%.c}").o"
	extra=()
	case "$f" in stdio/*) extra=(-I "$lc/stdio") ;; esac
	compile_c "$lc/$f" "$o" ${extra[@]+"${extra[@]}"} && add "$o"
done

# files 4.5BSD compiles several times with different macros
for spec in "string/bcopy.c:" "string/bcopy.c:-DMEMCOPY:memcpy" \
            "string/bcopy.c:-DMEMMOVE:memmove" "string/memset.c:" \
            "string/memset.c:-DBZERO:bzero" "string/index.c:" \
            "string/index.c:-DSTRCHR:strchr" "string/rindex.c:" \
            "string/rindex.c:-DSTRRCHR:strrchr" "string/swab.c:"; do
	srcf="${spec%%:*}"; rest="${spec#*:}"
	def="${rest%%:*}"; name="${rest#*:}"
	[ "$name" = "$rest" ] && name="$(basename "${srcf%.c}")"
	[ -f "$lc/$srcf" ] || continue
	dflags=()
	[ -n "$def" ] && dflags=("$def")
	compile_c "$lc/$srcf" "$obj/lc_$name.o" ${dflags[@]+"${dflags[@]}"} && add "$obj/lc_$name.o"
done

# Classic (pre-rune) ctype table, from 4.3BSD-Reno.
compile_c "$ws/libc/src/errlst.c"      "$obj/lc_errlst.o"    && add "$obj/lc_errlst.o"
compile_c "$ws/libc/src/ctype_table.c" "$obj/ctype_table.o"  && add "$obj/ctype_table.o"

# 64-bit division helpers.  The two toolchains need different sources:
#   mingw  libgcc has the routines but there is no 32-bit libgcc in an x64
#          MSYS2 install, so libc/src/quad32.c supplies them (this is what
#          build_sh.ps1 links) together with __chkstk_ms.
#   native with -fleading-underscore gcc asks for ___divdi3 while libgcc only
#          defines __divdi3, so the 4.5BSD libkern quad routines are used
#          instead and the symbol names match by construction.
libgcc_extra=()
if [ "$TOOLCHAIN" = mingw ]; then
	compile_c "$ws/libc/src/quad32.c" "$obj/lc_quad32.o" && add "$obj/lc_quad32.o"
	compile_asm "$ws/libc/arch/x86/chkstk_ms.s" "$obj/chkstk_ms.o" && add "$obj/chkstk_ms.o"
	libgcc_extra=("$obj/lc_quad32.o" "$obj/chkstk_ms.o")
else
	quad_srcs=(divdi3.c moddi3.c udivdi3.c umoddi3.c qdivrem.c
	           muldi3.c adddi3.c subdi3.c anddi3.c iordi3.c xordi3.c
	           notdi2.c negdi2.c ashldi3.c ashrdi3.c lshldi3.c lshrdi3.c
	           cmpdi2.c ucmpdi2.c)
	for f in "${quad_srcs[@]}"; do
		compile_c "$ws/kernel/lib/$f" "$obj/kq_${f%.c}.o" && add "$obj/kq_${f%.c}.o"
	done
fi

if [ "$fail" -ne 0 ]; then echo "$fail file(s) failed" >&2; exit 1; fi
libobjs=("${objs[@]}")

# ---------------------------------------------------------------- yacc/lex
# S426: the 4.4BSD-Lite2 tools (byacc + contrib flex 2.5.2) consume arith.y as
# written - "= {" is byacc's action-sharing notation and yyerrok/yyclearin are
# plain global macros there.  GNU bison understands neither, so it is only the
# fallback and needs the two rewrites build_sh.ps1 documents.
pick_yacc() {
	local c
	for c in "$ws/tools/hostbin/yacc$HOSTEXE" byacc yacc; do
		[ -n "$c" ] || continue
		if [ -x "$c" ] || have "$c"; then echo "$c"; return 0; fi
	done
	return 1
}
pick_flex() {
	local c
	for c in "$ws/tools/hostbin/flex$HOSTEXE" flex; do
		[ -n "$c" ] || continue
		if [ -x "$c" ] || have "$c"; then echo "$c"; return 0; fi
	done
	return 1
}

gen_yacc() {   # run inside $gen
	local y f
	if y="$(pick_yacc)" && f="$(pick_flex)"; then
		case "$(basename "$y")" in
			bison*) ;;   # bison needs the rewritten grammar
			*)
				rm -f y.tab.c y.tab.h arith.c arith_lex.c
				"$y" -d arith.y 2>&1 | head -8
				[ -f y.tab.c ] && cp -f y.tab.c arith.c
				# flex 2.5.2 only accepts the output name attached to -o
				"$f" -oarith_lex.c arith_lex.l 2>&1 | head -8
				[ -f arith.c ] && [ -f arith_lex.c ] && return 0
				;;
		esac
	fi
	if have bison; then
		sed -e 's/= {/{/' arith.y |
			grep -v -E '^[[:space:]]*(yyerrok|yyclearin);' > arith_sane.y
		bison -y -d arith_sane.y 2>&1 | head -8
		[ -f y.tab.c ] && cp -f y.tab.c arith.c
		flex -o arith_lex.c arith_lex.l 2>&1 | head -8
		return 0
	fi
	echo "no yacc/lex implementation found (install byacc and flex," >&2
	echo "or run tools/build_bsdtools.sh to build the 4.4BSD ones)" >&2
	return 1
}

if [ "$target" = "sh" ]; then
	step "generating sh sources"
	# Copy every regular file, like build_sh.ps1: the generators need the
	# auxiliary data files too (nodetypes, nodes.c.pat, builtins.def, ...).
	for f in "$shsrc"/*; do
		[ -f "$f" ] && cp -f "$f" "$gen/"
	done
	cp -f "$ws/libc/src/"* "$gen/" 2>/dev/null

	( cd "$gen" && sh mktokens ) || true
	[ -f "$gen/token.def" ] || { echo "mktokens failed" >&2; fail=$((fail+1)); }
	( cd "$gen" && sh mkbuiltins . ) || true
	[ -f "$gen/builtins.c" ] || { echo "mkbuiltins failed" >&2; fail=$((fail+1)); }

	step "generating nodes/syntax/init"
	hostflags=(-std=gnu89 -O1 -include "$ws/libc/src/hostgen/hostshim.h" -I "$shsrc")
	"$HOSTCC" "${hostflags[@]}" -o "$gen/mknodes$HOSTEXE"  "$ws/libc/src/hostgen/mknodes.c" || fail=$((fail+1))
	"$HOSTCC" "${hostflags[@]}" -o "$gen/mksyntax$HOSTEXE" "$shsrc/mksyntax.c"              || fail=$((fail+1))
	"$HOSTCC" "${hostflags[@]}" -o "$gen/mkinit$HOSTEXE"   "$shsrc/mkinit.c" "$ws/libc/src/hostgen/hoststub.c" || fail=$((fail+1))
	if [ -x "$gen/mknodes$HOSTEXE" ] && [ -x "$gen/mksyntax$HOSTEXE" ] && [ -x "$gen/mkinit$HOSTEXE" ]; then
		( cd "$gen" && "./mknodes$HOSTEXE" nodetypes nodes.c.pat ) || true
		( cd "$gen" && "./mksyntax$HOSTEXE" ) || true
		srcs="$(cd "$shsrc" && ls *.c | grep -v -E '^(mknodes|mksyntax|mkinit)\.c$' | tr '\n' ' ')"
		# mkinit exits 1 on purpose: it cannot exec /bin/sh on the host.
		( cd "$gen" && "./mkinit$HOSTEXE" "cc -c init.c" $srcs ) >/dev/null 2>&1 || true
	fi
	for f in init.c nodes.c syntax.c; do
		[ -f "$gen/$f" ] || { echo "missing $f" >&2; fail=$((fail+1)); }
	done

	step "yacc/lex"
	( cd "$gen" && gen_yacc ) || fail=$((fail+1))
	for f in arith.c arith_lex.c; do
		[ -f "$gen/$f" ] || { echo "missing $f" >&2; fail=$((fail+1)); }
	done

	step "compiling sh"
	sh_c=(alias.c cd.c error.c eval.c exec.c expand.c input.c
	      jobs.c mail.c main.c memalloc.c miscbltin.c mystring.c
	      options.c parser.c redir.c show.c trap.c output.c var.c
	      builtins.c init.c nodes.c syntax.c arith.c arith_lex.c)
	for f in "${sh_c[@]}"; do
		compile_c "$gen/$f" "$obj/sh_${f%.c}.o" && add "$obj/sh_${f%.c}.o"
	done
	compile_c "$shsrc/bltin/echo.c" "$obj/sh_echo_builtin.o" && add "$obj/sh_echo_builtin.o"
	if [ -f "$usr/usr.bin/printf/printf.c" ]; then
		compile_c "$usr/usr.bin/printf/printf.c" "$obj/sh_printf.o" -I "$usr/usr.bin/printf" &&
			add "$obj/sh_printf.o"
	fi
else
	step "compiling libc test program"
	compile_c "$ws/libexec/t_libc.c" "$obj/t_libc.o" && add "$obj/t_libc.o"
fi

if [ "$fail" -ne 0 ]; then echo "$fail file(s) failed" >&2; exit 1; fi

# ---------------------------------------------------------------- link
step "linking"
elf="$outd/sh.elf"; pe="$bld/sh.pe"
[ "$target" = "test" ] && { elf="$outd/test.elf"; pe="$bld/test.pe"; }
libgcc="$("$CC" "${TARGET32[@]}" -print-libgcc-file-name)"
[ -f "$libgcc" ] || libgcc=""

if [ "$TOOLCHAIN" = mingw ]; then
	"$LD" -m i386pe -Ttext 0x08048000 -e _start --subsystem console \
		--disable-reloc-section -o "$pe" "${objs[@]}" ${libgcc:+"$libgcc"} 2>&1 | head -30
	[ -f "$pe" ] || { echo "link failed" >&2; exit 1; }
	"$OBJCOPY" -O elf32-i386 -R .idata -R .reloc "$pe" "$elf" ||
		{ echo "objcopy failed" >&2; exit 1; }
else
	"$LD" -m elf_i386 -Ttext 0x08048000 -e _start -o "$elf" \
		"${objs[@]}" ${libgcc:+"$libgcc"} || { echo "link failed" >&2; exit 1; }
fi
fsize() { stat -c%s "$1" 2>/dev/null || wc -c < "$1"; }
echo "built $elf ($(fsize "$elf") bytes)"

if [ "$target" = "sh" ]; then
	# The tools the ISO ships in /bin.  Keep in sync with build_sh.ps1 and
	# with the copy list in build_iso.sh/build_iso.ps1.
	tools=(echo ls cat mkdir kill sleep rm cp mv wc grep head tail df mount
	       ps uname true false date vmstat mmaptest subtest pthread schedtest
	       pthpreempt pthmain aiotest uring spin cowtest crash badptr ipctest
	       fs_server srvtest devfs_server devtest shmtest vfstest proctest
	       netsrv nettest)
	for tool in "${tools[@]}"; do
		tobj="$obj/$tool.o"
		compile_c "$ws/libexec/$tool.c" "$tobj" || continue
		if [ "$TOOLCHAIN" = mingw ]; then
			tpe="$bld/$tool.pe"
			"$LD" -m i386pe -Ttext 0x08048000 -e _start --subsystem console \
				--disable-reloc-section -o "$tpe" "${libobjs[@]}" "$tobj" \
				${libgcc:+"$libgcc"} 2>/dev/null || continue
			"$OBJCOPY" -O elf32-i386 -R .idata -R .reloc "$tpe" "$outd/$tool.elf" ||
				continue
		else
			"$LD" -m elf_i386 -Ttext 0x08048000 -e _start \
				-o "$outd/$tool.elf" "${libobjs[@]}" "$tobj" \
				${libgcc:+"$libgcc"} 2>/dev/null || continue
		fi
		echo "built $outd/$tool.elf ($(fsize "$outd/$tool.elf") bytes)"
	done
fi
