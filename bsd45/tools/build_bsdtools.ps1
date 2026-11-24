# build_bsdtools.ps1 - build 4.4BSD-Lite2's yacc (byacc) and contrib flex
# 2.5.2 as Windows host tools under tools\hostbin.
#
# Why: build_sh.ps1 generates the 4.5BSD /bin/sh parser from arith.y and
# arith_lex.l.  With GNU bison the grammar has to be rewritten ("= {" is
# byacc's action-sharing notation, and bison scopes yyerrok/yyclearin to
# yyparse).  byacc accepts the period-correct source untouched, so the
# shell builds from unmodified 4.5BSD files.
#
#	.\tools\build_bsdtools.ps1
#	.\tools\build_bsdtools.ps1 -Tarball D:\4.4BSD-Lite2.tar.gz -Force
param(
    [string]$Tarball,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$ws    = Split-Path -Parent $tools
. (Join-Path $ws "tools-find.ps1")

if (-not $Tarball) {
    $Tarball = Join-Path ([Environment]::GetFolderPath("Desktop")) "4.4BSD-Lite2.tar.gz"
}
$src = Join-Path $tools "bsdsrc"
$dst = Join-Path $tools "hostbin"
$yac = Join-Path $src "4.4BSD-Lite2\usr\src\usr.bin\yacc"
$flx = Join-Path $src "4.4BSD-Lite2\usr\src\contrib\flex-2.5.2"

$bin = Get-MingwBin
$GCC = "$bin\gcc.exe"
$ccb = Get-CcBinArg $bin
if (!(Test-Path $GCC)) { throw "no mingw gcc under $bin" }

if ($Force -or !(Test-Path $yac) -or !(Test-Path $flx)) {
    if (!(Test-Path $Tarball)) {
        throw "4.4BSD-Lite2 tarball not found: $Tarball (pass -Tarball <path>)"
    }
    Write-Host "== extracting yacc + flex from $Tarball"
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    Push-Location $src
    # bsdtar (Windows tar.exe) and GNU tar both accept directory prefixes
    & tar xzf $Tarball "4.4BSD-Lite2/usr/src/usr.bin/yacc" "4.4BSD-Lite2/usr/src/contrib/flex-2.5.2"
    Pop-Location
}

New-Item -ItemType Directory -Force -Path $dst | Out-Null

# byacc: self-contained, no generator needed (skeleton.c is checked in).
Write-Host "== building byacc"
$yaccSrc = @("closure.c", "error.c", "lalr.c", "lr0.c", "main.c", "mkpar.c",
             "output.c", "reader.c", "skeleton.c", "symtab.c", "verbose.c",
             "warshall.c")
Push-Location $yac
# -std=gnu89: K&R-era sources; -fcommon: gcc >= 10 defaults to -fno-common
& $GCC -O2 -std=gnu89 -fcommon -w "-B" $ccb -o "yacc.exe" @yaccSrc
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "byacc build failed" }
Pop-Location

# flex 2.5.2: the distribution ships the bootstrapped scanner (scan.c) and,
# in MISC/, the yacc-generated parser - copy both to the top level.
Write-Host "== building flex 2.5.2"
Copy-Item (Join-Path $flx "MISC\parse.c") $flx -Force
Copy-Item (Join-Path $flx "MISC\parse.h") $flx -Force
$flexSrc = @("ccl.c", "dfa.c", "ecs.c", "gen.c", "main.c", "misc.c", "nfa.c",
             "parse.c", "scan.c", "skel.c", "sym.c", "tblcmp.c", "yylex.c")
Push-Location $flx
& $GCC -O2 -std=gnu89 -fcommon -w -DHAVE_CONFIG_H -I. "-B" $ccb -o "flex.exe" @flexSrc
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "flex build failed" }
Pop-Location

Copy-Item (Join-Path $yac "yacc.exe") $dst -Force
Copy-Item (Join-Path $flx "flex.exe") $dst -Force

Write-Host ""
Write-Host "built:"
Get-ChildItem $dst -Filter *.exe | ForEach-Object { "  {0}  {1:N0} bytes" -f $_.Name, $_.Length }
Write-Host ""
Write-Host "byacc accepts 4.5BSD arith.y as-is; flex 2.5.2 needs the output"
Write-Host "name attached to -o  (-oarith_lex.c, not -o arith_lex.c)."
