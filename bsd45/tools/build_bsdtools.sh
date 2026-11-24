#!/usr/bin/env bash
# build_bsdtools.sh - build 4.4BSD-Lite2's yacc (byacc) and contrib flex 2.5.2
# as host tools under tools/hostbin.
#
#	bash tools/build_bsdtools.sh
#	bash tools/build_bsdtools.sh -t /path/4.4BSD-Lite2.tar.gz --force
#
# Why: build_sh.* generates the 4.5BSD /bin/sh parser from arith.y and
# arith_lex.l.  GNU bison needs the grammar rewritten ("= {" is byacc's
# action-sharing notation, and bison scopes yyerrok/yyclearin to yyparse),
# while byacc consumes the period-correct source untouched.  Bash counterpart
# of tools/build_bsdtools.ps1; the same tools are installed to the same place,
# so both builds share one hostbin.
#
# These are HOST programs: they are compiled with $HOSTCC for this machine and
# only emit C, so nothing about the i386 target applies here.
set -uo pipefail
tools="$(cd "$(dirname "$0")" && pwd)"
ws="$(dirname "$tools")"
. "$ws/tools-find.sh"

tarball="${BSD45_SRC_TARBALL:-}"
force=0
while [ $# -gt 0 ]; do
	case "$1" in
		-t|--tarball) tarball="${2:-}"; shift 2 ;;
		-f|--force)   force=1; shift ;;
		*) echo "usage: $0 [-t 4.4BSD-Lite2.tar.gz] [--force]" >&2; exit 2 ;;
	esac
done
if [ -z "$tarball" ]; then
	for c in "$HOME/Desktop/4.4BSD-Lite2.tar.gz" "$HOME/4.4BSD-Lite2.tar.gz" \
	         "$ws/../4.4BSD-Lite2.tar.gz"; do
		[ -f "$c" ] && { tarball="$c"; break; }
	done
fi

src="$tools/bsdsrc"
dst="$tools/hostbin"
yac="$src/4.4BSD-Lite2/usr/src/usr.bin/yacc"
flx="$src/4.4BSD-Lite2/usr/src/contrib/flex-2.5.2"

if [ "$force" -eq 1 ] || [ ! -d "$yac" ] || [ ! -d "$flx" ]; then
	[ -n "$tarball" ] && [ -f "$tarball" ] || {
		echo "4.4BSD-Lite2 tarball not found: ${tarball:-<none>} (pass -t <path>)" >&2
		exit 1; }
	echo "== extracting yacc + flex from $tarball"
	mkdir -p "$src"
	( cd "$src" && tar xzf "$tarball" \
		"4.4BSD-Lite2/usr/src/usr.bin/yacc" \
		"4.4BSD-Lite2/usr/src/contrib/flex-2.5.2" ) || {
		echo "extract failed" >&2; exit 1; }
fi

mkdir -p "$dst"

# byacc: self-contained, no generator needed (skeleton.c is checked in).
echo "== building byacc"
# -std=gnu89: K&R-era sources; -fcommon: gcc >= 10 defaults to -fno-common
( cd "$yac" && "$HOSTCC" -O2 -std=gnu89 -fcommon -w -o "yacc$HOSTEXE" \
	closure.c error.c lalr.c lr0.c main.c mkpar.c output.c reader.c \
	skeleton.c symtab.c verbose.c warshall.c ) || { echo "byacc build failed" >&2; exit 1; }

# flex 2.5.2: the distribution ships the bootstrapped scanner (scan.c) and, in
# MISC/, the yacc-generated parser - copy both to the top level.
echo "== building flex 2.5.2"
cp -f "$flx/MISC/parse.c" "$flx/parse.c"
cp -f "$flx/MISC/parse.h" "$flx/parse.h"
( cd "$flx" && "$HOSTCC" -O2 -std=gnu89 -fcommon -w -DHAVE_CONFIG_H -I. \
	-o "flex$HOSTEXE" ccl.c dfa.c ecs.c gen.c main.c misc.c nfa.c parse.c \
	scan.c skel.c sym.c tblcmp.c yylex.c ) || { echo "flex build failed" >&2; exit 1; }

cp -f "$yac/yacc$HOSTEXE" "$dst/yacc$HOSTEXE"
cp -f "$flx/flex$HOSTEXE" "$dst/flex$HOSTEXE"

echo
echo "built:"
for f in "$dst/yacc$HOSTEXE" "$dst/flex$HOSTEXE"; do
	echo "  $(basename "$f")  $(stat -c%s "$f" 2>/dev/null || wc -c < "$f") bytes"
done
echo
echo "byacc accepts 4.5BSD arith.y as-is; flex 2.5.2 needs the output name"
echo "attached to -o (-oarith_lex.c, not -o arith_lex.c)."
