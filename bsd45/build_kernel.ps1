# build_kernel.ps1 - cross-build the 4.5BSD-Lite2 i386 kernel as UNIX ELF
# using the MSYS2 mingw32 (i686-w64-mingw32) gcc.
param(
    [switch]$NoLink,
    [string]$Only = "",
    [switch]$Unicode,
    [string]$Arch = "i386",
    [string]$Define = ""
)

$ErrorActionPreference = "Continue"
$ws   = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $ws "kernel"
$bld  = Join-Path $ws "build"
$obj  = Join-Path $bld "obj"
$inc  = Join-Path $bld "inc"
$gen  = Join-Path $bld "gen"
. (Join-Path $ws "tools-find.ps1")
$bin  = Get-MingwBin
$ccb  = Get-CcBinArg $bin
$GCC  = "$bin\gcc.exe"
$AS   = "$bin\as.exe"
$LD   = "$bin\ld.exe"
$OC   = "$bin\objcopy.exe"
$OD   = "$bin\objdump.exe"
$gccinc = Get-GccInclude $bin

New-Item -ItemType Directory -Force -Path $bld, $obj, $gen | Out-Null

# ---------------------------------------------------------------- sources
# overlay of the files that were adjusted for modern GCC / PE linking.


# ---------------------------------------------------------------- headers
function Copy-Headers([string]$from, [string]$to) {
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Get-ChildItem $from -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            Copy-Item $_.FullName (Join-Path $to $_.Name) -Force -ErrorAction Stop
        } catch {
            Write-Host "  skip header $($_.Name) (broken link)"
        }
    }
}
New-Item -ItemType Directory -Force -Path "$inc\sys", "$inc\vm", "$inc\machine", "$inc\i386" | Out-Null
Copy-Headers "$src\include\sys" "$inc\sys"
Copy-Headers "$src\vm" "$inc\vm"
Copy-Headers "$src\include\machine" "$inc\machine"
Copy-Headers "$src\include\machine" "$inc\i386"
Copy-Item "$src\include\machine\ansi.h"  "$inc\machine\ansi.h"  -Force
Copy-Item "$src\include\machine\limits.h" "$inc\machine\limits.h" -Force
Copy-Item "$src\include\machine\float.h"  "$inc\machine\float.h"  -Force
New-Item -ItemType Directory -Force -Path "$inc\libkern" | Out-Null
Copy-Item "$src\lib\libkern.h" "$inc\libkern\libkern.h" -Force
if (!(Select-String -Path "$inc\machine\types.h" -Pattern 'typedef int register_t;' -Quiet)) {
    Add-Content "$inc\machine\types.h" "`ntypedef int register_t;`ntypedef unsigned int u_register_t;" -Encoding ascii
}
@'
/* Minimal freestanding <string.h> for the kernel build. */
#ifndef _STRING_H_
#define _STRING_H_
#include <sys/types.h>
unsigned int	strlen(const char *);
int		strcmp(const char *, const char *);
int		strncmp(const char *, const char *, unsigned int);
int		bcmp(const void *, const void *, unsigned int);
void		bcopy(const void *, void *, unsigned int);
void		bzero(void *, unsigned int);
void *		memcpy(void *, const void *, unsigned int);
void *		memmove(void *, const void *, unsigned int);
void *		memset(void *, int, unsigned int);
#endif
'@ | Set-Content -Encoding ascii "$inc\string.h"
@'
#ifndef _MACHINE_ENDIAN_H_
#define _MACHINE_ENDIAN_H_
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
#define _PDP_ENDIAN 3412
#define _BYTE_ORDER _LITTLE_ENDIAN
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#define PDP_ENDIAN _PDP_ENDIAN
#define BYTE_ORDER _BYTE_ORDER
#endif
'@ | Set-Content -Encoding ascii "$inc\machine\endian.h"
@'
#ifndef _MACHINE_STDARG_H_
#define _MACHINE_STDARG_H_
#include <stdarg.h>
#endif
'@ | Set-Content -Encoding ascii "$inc\machine\stdarg.h"
'#define NISA 0'              | Set-Content -Encoding ascii "$gen\isa.h"
'#define NCOM 0'              | Set-Content -Encoding ascii "$gen\com.h"
'#define NPX 0'               | Set-Content -Encoding ascii "$gen\npx.h"
'#define NWD 0'               | Set-Content -Encoding ascii "$gen\wd.h"
'#define NFD 0'               | Set-Content -Encoding ascii "$gen\fd.h"

# ---------------------------------------------------------------- assym.s
# Build and run the genuine 4.5BSD genassym on the host to get the same
# struct offsets / constants that locore.s expects.
#
# genassym has to be a 32-bit program (the offsets depend on the i386 ABI)
# but there is no 32-bit libgcc/crt in an x86_64 MSYS2 install.  So link it
# with ld directly, entry point = main, against a 32-bit msvcrt import
# library we generate with dlltool: genassym only needs printf()/exit().
$ga = "$bld\genassym.exe"
$imp = "$bld\host32\libmsvcrt32.a"
$def = Join-Path $ws "tools\host32\msvcrt.def"
New-Item -ItemType Directory -Force -Path "$bld\host32" | Out-Null
if (!(Test-Path $imp) -and (Test-Path $def)) {
    # -f "--32": dlltool's default assembler is the 64-bit one, force i386
    & "$bin\dlltool.exe" -m i386 -f "--32" -d $def -l $imp 2>&1 | Out-Null
}
& $GCC -B $ccb -m32 -O1 -std=gnu89 -fno-strict-aliasing `
    -fno-stack-protector -ffreestanding `
    -mno-ms-bitfields -I $inc -DKERNEL -c -o "$bld\genassym.o" "$src\arch\i386\i386\genassym.c" 2>$null
if ($LASTEXITCODE -ne 0) { throw "genassym compile failed" }
& $GCC -B $ccb -m32 -O1 -ffreestanding -fno-stack-protector `
    -c -o "$bld\hostcrt.o" (Join-Path $ws "tools\host32\hostcrt.c") 2>$null
if ($LASTEXITCODE -ne 0) { throw "hostcrt compile failed" }
& $LD -m i386pe -e _main --subsystem console -o $ga "$bld\genassym.o" "$bld\hostcrt.o" $imp 2>&1 |
    Where-Object { $_ -match 'undefined|error' } | Select-Object -First 10
if (!(Test-Path $ga)) { throw "genassym link failed" }
# M1: capture genassym output explicitly and write ASCII (no UTF-16 BOM).
# Start-Process -RedirectStandardOutput yields an empty file under some hosts,
# while PowerShell 5.1's ">" emits UTF-16LE which gcc rejects.
# M1: 显式捕获输出并以 ASCII 写出（无 UTF-16 BOM）。Start-Process 的重定向在
# 某些宿主下产出空文件，而 PowerShell 5.1 的 ">" 会写 UTF-16LE，gcc 不接受。
# The freshly linked 32-bit helper can be locked/scanned briefly by Windows;
# launching it immediately sometimes yields exit 0 with empty output.  Retry
# with a short delay instead of silently writing a 0-byte assym.s.
$assymText = ""
$gaExit = 0
for ($try = 1; $try -le 4; $try++) {
    if ($try -gt 1) { Start-Sleep -Milliseconds 300 }
    $assymText = (& $ga 2>&1) -join "`n"
    $gaExit = $LASTEXITCODE
    if ($assymText.Length -ge 100 -or $gaExit -ne 0) { break }
}
if ($gaExit -ne 0) { throw "genassym run failed (exit $gaExit)" }
[System.IO.File]::WriteAllText("$bld\assym.s", $assymText, [System.Text.Encoding]::ASCII)
if (!(Test-Path "$bld\assym.s")) { throw "genassym run failed" }
if ((Get-Item "$bld\assym.s").Length -lt 100) { throw "genassym produced no output" }
Write-Host ("genassym: {0} bytes of assym.s" -f (Get-Item "$bld\assym.s").Length)
@'
/*
 * vector.s - device interrupt stubs.
 * Normally generated by config(8); this kernel attaches no ISA drivers,
 * so the table is empty.  8259 stubs come from i386/isa/icu.s.
 */
'@ | Set-Content -Encoding ascii "$bld\vector.s"

# ---------------------------------------------------------------- userland
# Embed the userland ELF images as C arrays (ramfs contents) and the two
# disk boot stages so the `install` command can write them to the ATA disk.
function Append-ByteArray($sb, [string]$cname, [byte[]]$bytes) {
    [void]$sb.AppendLine("const unsigned char ${cname}[] = {")
    for ($i = 0; $i -lt $bytes.Length; $i += 16) {
        $end = [Math]::Min($i + 15, $bytes.Length - 1)
        $line = ($bytes[$i..$end] | ForEach-Object { "0x{0:x2}" -f $_ }) -join ","
        [void]$sb.AppendLine("$line,")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("const unsigned int ${cname}_len = $($bytes.Length);")
}

function Build-RawStage([string]$src, [string]$text, [string]$name, [string]$entry) {
    $o = "$bld\$name.embed.o"
    $pe = "$bld\$name.embed.pe"
    $raw = "$bld\$name.embed.bin"
    & $GCC -B $ccb -m32 -c -x assembler-with-cpp -o $o $src
    if ($LASTEXITCODE) { throw "assemble $name failed" }
    & $LD -m i386pe -Ttext $text -e $entry --subsystem console -o $pe $o 2>$null
    if ($LASTEXITCODE) { throw "link $name failed" }
    & $OC -O binary --only-section=.text $pe $raw
    if ($LASTEXITCODE) { throw "objcopy $name failed" }
    return [IO.File]::ReadAllBytes($raw)
}

$s1h = Build-RawStage "$ws\boot\i386\stage1_hdd.s" "0x7C00" "stage1_hdd" "_start"
$s2  = Build-RawStage "$ws\boot\i386\stage2.s" "0x90000" "stage2" "_start32"
# S412: AP trampoline is a raw real-mode blob linked at physical 0x7000.
$apt = Build-RawStage "$ws\kernel\arch\$Arch\ap_tramp.s" "0x7000" "ap_tramp" "_ap_tramp_start"
if ($s1h.Length -ne 512) { throw "stage1_hdd must be 512 bytes, got $($s1h.Length)" }
Write-Host ("embedded boot stages: stage1_hdd {0} bytes, stage2 {1} bytes, ap_tramp {2} bytes" -f $s1h.Length, $s2.Length, $apt.Length)
if ($apt.Length -lt 0x318) { throw "ap_tramp too small: $($apt.Length)" }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* generated by build_kernel.ps1 */")
foreach ($name in "hello", "init") {
    $path = Join-Path $ws "out\userland\$name.elf"
    $ub = @()
    if (Test-Path $path) { $ub = [IO.File]::ReadAllBytes($path) }
    Append-ByteArray $sb "${name}_elf" $ub
}
Append-ByteArray $sb "stage1_hdd_bin" $s1h
Append-ByteArray $sb "stage2_bin" $s2
Append-ByteArray $sb "ap_tramp_bin" $apt
[IO.File]::WriteAllText("$bld\embed.c", $sb.ToString())

# ---------------------------------------------------------------- headers
$cflags = @(
    "-m32", "-std=gnu89", "-O2", "-ffreestanding",
    "-fno-pie", "-fno-pic", "-fcommon", "-fno-builtin",
    "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-fno-tree-loop-distribute-patterns",
    "-march=i386", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
    "-mno-ms-bitfields", "-mpreferred-stack-boundary=2",
    "-ffunction-sections", "-fdata-sections",
    "-nostdinc", "-I", $inc, "-I", $gen, "-I", $src,
    "-I", (Join-Path $src "arch"), "-I", (Join-Path $src "fs"),
    "-I", (Join-Path $src "driver"), "-I", (Join-Path $src "net"),
    "-isystem", $gccinc, "-DKERNEL", "-DARCH_$Arch", "-B", $ccb
)
if ($Unicode) { $cflags += "-DKERNEL_UNICODE=1" }
if ($Define) { foreach ($d in ($Define -split ',')) { $cflags += "-D$d" } }

function Compile-C([string]$file, [string]$out) {
    $args = $cflags + @("-c", "-o", $out, $file)
    $log = & $GCC $args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $file" -ForegroundColor Red
        $log | Select-Object -First 40 | ForEach-Object { Write-Host "   $_" }
        return $false
    }
    return $true
}

function Assemble([string]$file, [string]$out) {
    $args = @(
        "-m32", "-c", "-x", "assembler-with-cpp",
        "-nostdinc", "-I", $bld, "-I", $inc, "-I", $gen, "-I", $src,
        "-I", (Join-Path $src "arch"), "-I", (Join-Path $src "fs"),
        "-I", (Join-Path $src "driver"), "-I", (Join-Path $src "net"),
        "-I", (Join-Path $inc "sys"), "-DKERNEL", "-DLOCORE",
        "-B", $ccb,
        "-o", $out, $file
    )
    $log = & $GCC $args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $file" -ForegroundColor Red
        $log | Select-Object -First 40 | ForEach-Object { Write-Host "   $_" }
        return $false
    }
    return $true
}

# ---------------------------------------------------------------- sources
$c_sources = [ordered]@{
    "locore.o"   = @("asm", "$src\arch\i386\isa\locore.s")
    "pmap.o"     = @("c",   "$src\arch\i386\isa\pmap.c")
    "machdep.o"  = @("c",   "$src\arch\i386\isa\machdep.c")
    "cons.o"     = @("c",   "$src\arch\i386\i386\cons.c")
    "bcmp.o"     = @("c",   "$src\lib\bcmp.c")
    "strcmp.o"   = @("c",   "$src\lib\strcmp.c")
    "strlen.o"   = @("c",   "$src\lib\strlen.c")
    "strings.o"  = @("c",   "$ws\kernel\lib\strings.c")
    "usermode.o" = @("c",   "$ws\kernel\sys\usermode.c")
    "usermode_asm.o" = @("asm", "$ws\kernel\arch\$Arch\usermode_asm.s")
    "embed.o"    = @("c",   "$bld\embed.c")
    "pccon.o"    = @("c",   "$ws\kernel\driver\pccon.c")
    "stubs.o"    = @("c",   "$ws\kernel\sys\stubs.c")
    "glue.o"     = @("c",   "$ws\kernel\sys\glue.c")
    "mon.o"      = @("c",   "$ws\kernel\sys\mon.c")
    "atapi.o"    = @("c",   "$ws\kernel\driver\atapi.c")
    "ata.o"      = @("c",   "$ws\kernel\driver\ata.c")
    "isofs.o"    = @("c",   "$ws\kernel\fs\isofs.c")
    "ffs.o"      = @("c",   "$ws\kernel\fs\ffs\ffs.c")
    "jlfs.o"     = @("c",   "$ws\kernel\fs\jlfs\jlfs.c")
    "install.o"  = @("c",   "$ws\kernel\fs\ffs\install.c")
    "tmpfs.o"    = @("c",   "$ws\kernel\fs\tmpfs.c")
    "kstr.o"     = @("c",   "$ws\kernel\lib\kstr.c")
    # subsys/ personality layer removed: syscalls dispatch straight to
    # bsd45_syscall(); struct sysregs now lives in include/sys/platform.h
    "kthread.o"  = @("c",   "$ws\kernel\thread\kthread.c")
    "sync.o"     = @("c",   "$ws\kernel\thread\sync.c")
    "kipc.o"     = @("c",   "$ws\kernel\thread\kipc.c")
    "softirq.o"  = @("c",   "$ws\kernel\thread\softirq.c")
    "kswitch.o"  = @("asm", "$ws\kernel\arch\$Arch\kswitch.s")
    "hal_arch.o" = @("asm", "$ws\kernel\arch\$Arch\x86.asm")
    "smp_ipi.o"  = @("asm", "$ws\kernel\arch\$Arch\smp_ipi.s")
    # ap_tramp.s is linked as a raw 0x7000 blob and embedded below (S412).
    # kernel/hal removed: context + timer folded into arch/i386, no IPL layer
    "smp.o"      = @("c",   "$ws\kernel\arch\$Arch\smp.c")
    "atomic.o"   = @("c",   "$ws\kernel\arch\$Arch\atomic.c")
    "ctx.o"      = @("c",   "$ws\kernel\arch\$Arch\ctx.c")
    "timer.o"    = @("c",   "$ws\kernel\arch\$Arch\timer.c")
    "zcomp.o"    = @("c",   "$ws\kernel\vm\zcomp.c")
    "mem_plan.o" = @("c",   "$ws\kernel\vm\mem_plan.c")
    "aio.o"      = @("c",   "$ws\kernel\fs\aio.c")
    "vfs_bio.o"  = @("c",   "$ws\kernel\fs\vfs_bio.c")
    "vfs_cache.o" = @("c",  "$ws\kernel\fs\vfs_cache.c")
    "pagecache.o" = @("c",  "$ws\kernel\vm\pagecache.c")
"vfs.o"      = @("c",   "$ws\kernel\fs\vfs.c")
}

$objs = @()
$fail = 0
foreach ($k in $c_sources.Keys) {
    if ($Only -and $k -ne $Only) { if (Test-Path "$obj\$k") { $objs += "$obj\$k" }; continue }
    $kind, $path = $c_sources[$k]
    $out = "$obj\$k"
    if ($kind -eq "asm") { $ok = Assemble $path $out } else { $ok = Compile-C $path $out }
    if ($ok) { $objs += $out } else { $fail++ }
}

if ($fail) { Write-Host "$fail file(s) failed" -ForegroundColor Red; exit 1 }
if ($NoLink) { Write-Host "objects built"; exit 0 }

# ------------------------------------------------------ shrink BSS objects
# i686-w64-mingw32-gcc is a PE/COFF target.  With -fdata-sections every
# *static* zero-initialised object is emitted as ".data$<name>" carrying its
# bytes, so `static char heap[256KB]` really puts 256 KB into the object and
# into the linked image.  A Linux ELF toolchain puts the same object in .bss,
# which costs no file space - that is the whole reason the Windows kernel came
# out ~572 KB while the Ubuntu one is ~123 KB.
# Rewriting those sections as bss is a pure representation change (an
# all-zero payload with no relocations *is* uninitialised data), so the
# kernel behaves identically while the image shrinks by ~400 KB.
$fixer = Join-Path $ws "tools\pe_bss_fix.py"
# A bare `Get-Command python` may resolve to the Microsoft Store stub, which
# runs nothing and makes this step silently a no-op -> the image keeps every
# zero-initialised static array in .data and grows past what the boot loader
# can stage (see os_vmware_debug.md).  Always resolve a working interpreter.
$py = Get-PythonExe
if ($py -and (Test-Path $fixer)) {
    & $py $fixer --recurse $obj | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "pe_bss_fix.py failed" }
} else {
    Write-Host "warning: no working python found - zero-initialised static arrays stay in .data" -ForegroundColor Red
    Write-Host "         the kernel image will be ~450 KB larger than it should be" -ForegroundColor Red
}

# ---------------------------------------------------------------- link
$ldflags = @(
    # The kernel now lives at KERNBASE + 1 MB (phys 0x101000) so .data/.bss
    # may extend past 0xA0000 - see boot/i386/kernel.ld.  stage2 copies each
    # PT_LOAD segment to p_vaddr - KERNBASE, so only the file still has to
    # fit in the 576 KB staging area below 0x90000.
    "-m", "i386pe", "--image-base", "0xFE000000", "-Ttext", "0xFE101000",
    "--subsystem", "console", "-e", "start", "--gc-sections",
    # the kernel is loaded at a fixed address by stage2; a PE base-reloc
    # table is useless here and binutils >= 2.4x refuses to emit one for
    # images that contain 8-bit fixups ("8-bit reloc in dll").
    "--disable-reloc-section",
    "--file-alignment", "0x1000", "--section-alignment", "0x1000"
)

# pass 1: discover section layout with a harmless _end
$rc = Invoke-LdRsp -Ld $LD -LdFlags $ldflags -Out "$bld\kernel.pe" -Objects $objs `
    -Tail @("--defsym", "_end=0xFE180000", "--defsym", "_edata=0xFE140000", "--defsym", "_etext=0xFE140000") `
    -RspPath "$bld\link_kernel.rsp"
if ($rc -ne 0) { Write-Host "link pass1 failed" -ForegroundColor Red; exit 1 }

$sect = & $OD -h "$bld\kernel.pe" 2>&1
$maxEnd = [long]0
$textEnd = [long]0
$dataEnd = [long]0
foreach ($l in $sect) {
    if ($l -match '^\s*(\d+)\s+(\.\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})') {
        $name = $matches[2]
        $size = [long][Convert]::ToUInt32($matches[3], 16)
        $vma  = [long][Convert]::ToUInt32($matches[4], 16)
        if ($name -eq ".text") { $textEnd = $vma + $size }
        if ($name -eq ".bss")  { $dataEnd = $vma }
        if ($name -ne ".reloc" -and ($vma + $size) -gt $maxEnd) { $maxEnd = $vma + $size }
    }
}
$endAddr = [long](([math]::Floor(($maxEnd + 0xFFF) / 0x1000)) * 0x1000)
if ($textEnd -eq 0) { $textEnd = [long]0xFE101000 }
if ($dataEnd -eq 0) { $dataEnd = $maxEnd }
Write-Host ("sections end {0:x}, _end {1:x}, _edata {2:x}, _etext {3:x}" -f $maxEnd, $endAddr, $dataEnd, $textEnd)

$endSym  = "_end=0x{0:x}"   -f $endAddr
$dataSym = "_edata=0x{0:x}" -f $dataEnd
$textSym = "_etext=0x{0:x}" -f $textEnd
$rc = Invoke-LdRsp -Ld $LD -LdFlags $ldflags -Out "$bld\kernel.pe" -Objects $objs `
    -Tail @("--defsym", $endSym, "--defsym", $dataSym, "--defsym", $textSym) `
    -RspPath "$bld\link_kernel.rsp"
if ($rc -ne 0) { Write-Host "link pass2 failed" -ForegroundColor Red; exit 1 }

& $OC -O elf32-i386 -R .idata -R .reloc "$bld\kernel.pe" "$bld\bsd45.elf"
if ($LASTEXITCODE -ne 0) { Write-Host "objcopy failed" -ForegroundColor Red; exit 1 }

Copy-Item "$bld\bsd45.elf" (Join-Path $ws "out\BSD45.ELF") -Force -ErrorAction SilentlyContinue
Write-Host "built $bld\bsd45.elf"
& $OD -h "$bld\bsd45.elf" | Select-Object -First 12













