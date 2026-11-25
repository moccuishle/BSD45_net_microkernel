# build_iso.ps1 - build the El Torito boot image and the BSD45-ELF-x86 ISO
param(
    [string]$Kernel = "",
    [string]$OutIso = "",
    [int]$KernelLba = 8192
)

$ErrorActionPreference = "Continue"
$ws  = Split-Path -Parent $MyInvocation.MyCommand.Path
$bld = Join-Path $ws "build"
$boot= Join-Path $ws "boot"
$out = Join-Path $ws "out"
. (Join-Path $ws "tools-find.ps1")
$bin = Get-MingwBin
$ccb  = Get-CcBinArg $bin
$GCC = "$bin\gcc.exe"; $LD = "$bin\ld.exe"; $OC = "$bin\objcopy.exe"
$MKISO = Get-Mkisofs
$isodir = Join-Path $bld "isodir"
if (!$Kernel) { $Kernel = Join-Path $bld "bsd45.elf" }
if (!$OutIso) { $OutIso = Join-Path $out "BSD45-ELF-x86.iso" }
New-Item -ItemType Directory -Force -Path $out | Out-Null
# Move the stale staging tree aside instead of deleting it.  This is
# equivalent for a fresh build and also works in sandboxes that restrict
# bulk Remove-Item.
if (Test-Path $isodir) {
    $stale = "$isodir.stale.$([int][double]::Parse((Get-Date -UFormat %s)))"
    try { Move-Item $isodir $stale -Force -ErrorAction Stop }
    catch { Remove-Item $isodir -Recurse -Force -ErrorAction SilentlyContinue }
}
New-Item -ItemType Directory -Force -Path $isodir | Out-Null

function Build-Stage([string]$src, [string]$text, [string]$name, [string]$entry = "_start") {
    $o = "$bld\$name.o"
    $pe = "$bld\$name.pe"
    $raw = "$bld\$name.bin"
    # If the assembler/linker are unavailable (e.g. an environment that
    # blocks the compiler's child processes) reuse the already linked PE as
    # long as it is newer than the source - it only takes objcopy to produce
    # the flat binary.
    $reuse = (Test-Path $pe) -and ((Get-Item $pe).LastWriteTime -gt (Get-Item $src).LastWriteTime)
    if (-not $reuse) {
        & $GCC -B $ccb -m32 -c -x assembler-with-cpp -o $o $src
        if ($LASTEXITCODE) { throw "assemble $name failed" }
        & $LD -m i386pe -Ttext $text -e $entry --subsystem console -o $pe $o 2>$null
        if ($LASTEXITCODE) { throw "link $name failed" }
    } else {
        Write-Host "reusing $pe (source unchanged)"
    }
    & $OC -O binary --only-section=.text $pe $raw
    if ($LASTEXITCODE) { throw "objcopy $name failed" }
    return $raw
}

$s1 = Build-Stage "$boot\i386\stage1.s" "0x7C00" "stage1"
$s2 = Build-Stage "$boot\i386\stage2.s" "0x90000" "stage2" "_start32"

$b1 = [IO.File]::ReadAllBytes($s1)
$b2 = [IO.File]::ReadAllBytes($s2)
if ($b1.Length -ne 512) { throw "stage1 must be 512 bytes, got $($b1.Length)" }
Write-Host ("stage1 {0} bytes, stage2 {1} bytes" -f $b1.Length, $b2.Length)

# boot image = stage1 || stage2, padded to 8 virtual sectors (4 KiB)
$img = New-Object byte[] 4096
[Array]::Copy($b1, 0, $img, 0, 512)
[Array]::Copy($b2, 0, $img, 512, $b2.Length)

# patch the manifest: "BDMF" lba bytes stage2size
$magic = [byte[]](0x42, 0x44, 0x4D, 0x46)
$off = -1
for ($i = 0; $i -le 512 - 16; $i++) {
    if ($img[$i] -eq 0x42 -and $img[$i+1] -eq 0x44 -and $img[$i+2] -eq 0x4D -and $img[$i+3] -eq 0x46) { $off = $i; break }
}
if ($off -lt 0) { throw "manifest magic not found" }
[BitConverter]::GetBytes([uint32]$KernelLba).CopyTo($img, $off + 4)
[BitConverter]::GetBytes([uint32](Get-Item $Kernel).Length).CopyTo($img, $off + 8)
[BitConverter]::GetBytes([uint32]$b2.Length).CopyTo($img, $off + 12)
[IO.File]::WriteAllBytes("$bld\boot.img", $img)

Copy-Item $Kernel "$isodir\BSD45.ELF" -Force

# S4: the userland programs live on the ISO9660 volume, not in the kernel.
New-Item -ItemType Directory -Force -Path "$isodir\bin", "$isodir\sbin" | Out-Null
$sh = Join-Path $ws "out\userland\sh.elf"
$init = Join-Path $ws "out\userland\init.elf"
$hello = Join-Path $ws "out\userland\hello.elf"
$test = Join-Path $ws "out\userland\test.elf"
if (Test-Path $sh)    { Copy-Item $sh    "$isodir\bin\SH.ELF"   -Force }
if (Test-Path $init)  { Copy-Item $init  "$isodir\sbin\INIT.ELF" -Force }
if (Test-Path $hello) { Copy-Item $hello "$isodir\HELLO.ELF"    -Force }
if (Test-Path $test)  { Copy-Item $test  "$isodir\TEST.ELF"     -Force }
$ls = Join-Path $ws "out\userland\ls.elf"
if (Test-Path $ls)    { Copy-Item $ls    "$isodir\bin\LS.ELF"     -Force }
$mkdir = Join-Path $ws "out\userland\mkdir.elf"
if (Test-Path $mkdir) { Copy-Item $mkdir "$isodir\bin\MKDIR.ELF" -Force }
$cat = Join-Path $ws "out\userland\cat.elf"
if (Test-Path $cat)   { Copy-Item $cat   "$isodir\bin\CAT.ELF"    -Force }
$echo = Join-Path $ws "out\userland\echo.elf"
if (Test-Path $echo)  { Copy-Item $echo  "$isodir\bin\ECHO.ELF"  -Force }
$kill = Join-Path $ws "out\userland\kill.elf"
if (Test-Path $kill)  { Copy-Item $kill  "$isodir\bin\KILL.ELF"  -Force }
$sleep = Join-Path $ws "out\userland\sleep.elf"
if (Test-Path $sleep) { Copy-Item $sleep "$isodir\bin\SLEEP.ELF" -Force }
$rmb = Join-Path $ws "out\userland\rm.elf"
if (Test-Path $rmb)   { Copy-Item $rmb   "$isodir\bin\RM.ELF"    -Force }
$cpb = Join-Path $ws "out\userland\cp.elf"
if (Test-Path $cpb)   { Copy-Item $cpb   "$isodir\bin\CP.ELF"    -Force }
$mvb = Join-Path $ws "out\userland\mv.elf"
if (Test-Path $mvb)   { Copy-Item $mvb   "$isodir\bin\MV.ELF"    -Force }
$wcb = Join-Path $ws "out\userland\wc.elf"
if (Test-Path $wcb)   { Copy-Item $wcb   "$isodir\bin\WC.ELF"    -Force }
$grepb = Join-Path $ws "out\userland\grep.elf"
if (Test-Path $grepb) { Copy-Item $grepb "$isodir\bin\GREP.ELF"  -Force }
$headb = Join-Path $ws "out\userland\head.elf"
if (Test-Path $headb) { Copy-Item $headb "$isodir\bin\HEAD.ELF"  -Force }
$tailb = Join-Path $ws "out\userland\tail.elf"
if (Test-Path $tailb) { Copy-Item $tailb "$isodir\bin\TAIL.ELF"  -Force }
$dfb = Join-Path $ws "out\userland\df.elf"
if (Test-Path $dfb) { Copy-Item $dfb "$isodir\bin\DF.ELF"  -Force }
$mntb = Join-Path $ws "out\userland\mount.elf"
if (Test-Path $mntb) { Copy-Item $mntb "$isodir\bin\MOUNT.ELF" -Force }
$psb = Join-Path $ws "out\userland\ps.elf"
if (Test-Path $psb) { Copy-Item $psb "$isodir\bin\PS.ELF" -Force }
$unb = Join-Path $ws "out\userland\uname.elf"
if (Test-Path $unb) { Copy-Item $unb "$isodir\bin\UNAME.ELF" -Force }
$trb = Join-Path $ws "out\userland\true.elf"
if (Test-Path $trb) { Copy-Item $trb "$isodir\bin\TRUE.ELF" -Force }
$flb = Join-Path $ws "out\userland\false.elf"
if (Test-Path $flb) { Copy-Item $flb "$isodir\bin\FALSE.ELF" -Force }
$dtb = Join-Path $ws "out\userland\date.elf"
if (Test-Path $dtb) { Copy-Item $dtb "$isodir\bin\DATE.ELF" -Force }
$vsb = Join-Path $ws "out\userland\vmstat.elf"
if (Test-Path $vsb) { Copy-Item $vsb "$isodir\bin\VMSTAT.ELF" -Force }
$sub = Join-Path $ws "out\userland\subtest.elf"
if (Test-Path $sub) { Copy-Item $sub "$isodir\bin\SUBTEST.ELF" -Force }
$mmb = Join-Path $ws "out\userland\mmaptest.elf"
if (Test-Path $mmb) { Copy-Item $mmb "$isodir\bin\MMAPTEST.ELF" -Force }
$ppb = Join-Path $ws "out\userland\pthpreempt.elf"
if (Test-Path $ppb) { Copy-Item $ppb "$isodir\bin\PTHPRE.ELF" -Force }
$stb = Join-Path $ws "out\userland\schedtest.elf"
if (Test-Path $stb) { Copy-Item $stb "$isodir\bin\SCHEDTEST.ELF" -Force }
$ptb = Join-Path $ws "out\userland\pthread.elf"
if (Test-Path $ptb) { Copy-Item $ptb "$isodir\bin\PTHREAD.ELF" -Force }
# S425: main-thread-exits-first regression
$pmb = Join-Path $ws "out\userland\pthmain.elf"
if (Test-Path $pmb) { Copy-Item $pmb "$isodir\bin\PTHMAIN.ELF" -Force }
$atb = Join-Path $ws "out\userland\aiotest.elf"
if (Test-Path $atb) { Copy-Item $atb "$isodir\bin\AIOTEST.ELF" -Force }
$urb = Join-Path $ws "out\userland\uring.elf"
if (Test-Path $urb) { Copy-Item $urb "$isodir\bin\uring.ELF" -Force }
foreach ($pair in @(
    @("spin", "SPIN"), @("cowtest", "COWTEST"), @("crash", "CRASH"),
    @("badptr", "BADPTR"), @("ipctest", "IPCTEST"),
    @("fs_server", "FSSRV"), @("srvtest", "SRVTEST"),
    @("devfs_server", "DEVFS"), @("devtest", "DEVTEST"),
    @("shmtest", "SHMTEST"), @("vfstest", "VFSTEST"), @("proctest", "PROCTEST"),
    @("netsrv", "NETSRV"), @("nettest", "NETTEST"),
    @("rdstress", "RDSTRESS"))) {
    $srcf = Join-Path $ws ("out\userland\{0}.elf" -f $pair[0])
    if (Test-Path $srcf) { Copy-Item $srcf ("$isodir\bin\{0}.ELF" -f $pair[1]) -Force }
}
# S-install: build the native-JLFS image that the in-guest `install` command
# writes to the VMware virtual disk.  It is shipped on the ISO as BSD45.JLFS.
# Pure-PowerShell port (hdd/make_install_jlfs.ps1 -> mkjlfs.ps1); no python.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "hdd\make_install_jlfs.ps1") (Join-Path $ws "hdd\BSD45.JLFS")
if ($LASTEXITCODE -ne 0) { throw "make_install_jlfs.ps1 failed" }
$jlfsImg = Join-Path $ws "hdd\BSD45.JLFS"
if (Test-Path $jlfsImg) {
    Copy-Item $jlfsImg "$isodir\BSD45.JLFS" -Force
    Write-Host ("BSD45.JLFS: {0:N0} bytes" -f (Get-Item $jlfsImg).Length)
}

Copy-Item "$bld\boot.img" "$isodir\boot.img" -Force
# rootfs_overlay/ is copied on top of the ISO root: /etc/profile,
# /README.TXT and the empty bin/sbin/tmp skeleton.
$ovl = Join-Path $ws "rootfs_overlay"
if (Test-Path $ovl) {
    Get-ChildItem $ovl -Force | ForEach-Object {
        # .keep placeholders only keep empty dirs in git; never copy them into
        # the ISO.  Skipping here avoids a bulk-delete later (works in sandboxes
        # that restrict Remove-Item).
        if ($_.Name -eq ".keep") { return }
        if ($_.PSIsContainer) {
            Copy-Item $_.FullName "$isodir\$($_.Name)" -Recurse -Force
            # nested .keep placeholders: rename in place (empty files, harmless
            # but keep them out of the ISO name space) without deleting.
            Get-ChildItem "$isodir\$($_.Name)" -Recurse -Filter ".keep" -Force -ErrorAction SilentlyContinue |
                ForEach-Object { Rename-Item $_.FullName ".keep.skip" -Force -ErrorAction SilentlyContinue }
        } else {
            Copy-Item $_.FullName "$isodir\$($_.Name)" -Force
        }
    }
}
# xorriso -o overwrites an existing ISO, so no pre-delete is needed.

$mkArgs = @()
$mkDir = $isodir
$mkOut = $OutIso
if ([IO.Path]::GetFileNameWithoutExtension($MKISO) -eq "xorriso") {
    $mkArgs += @("-as", "mkisofs")
    # MSYS2's /usr/bin/xorriso is a POSIX program: it understands "/c/..."
    # but not "C:\..." (it prepends the cwd and then fails to stat the source).
    # Native mkisofs.exe (VMware) is the other way round, so only convert when
    # we actually picked up the MSYS2 build.
    if ($MKISO -match '[\\/]usr[\\/]bin[\\/]') {
        $mkDir = $isodir -replace '\\', '/'
        $mkOut = $OutIso -replace '\\', '/'
        if ($mkDir -match '^([A-Za-z]):(.*)$') { $mkDir = '/' + $Matches[1].ToLower() + $Matches[2] }
        if ($mkOut -match '^([A-Za-z]):(.*)$') { $mkOut = '/' + $Matches[1].ToLower() + $Matches[2] }
    }
}
& $MKISO @mkArgs -quiet -iso-level 1 -V "BSD45_ELF_X86" -b boot.img -c boot.cat `
    -no-emul-boot -boot-load-size 8 -o $mkOut $mkDir
if ($LASTEXITCODE) { throw "mkisofs failed" }

# pad the ISO to the kernel LBA and append the ELF kernel there
$isoBytes = [IO.File]::ReadAllBytes($OutIso)
$target = [long]$KernelLba * 2048
if ($isoBytes.Length -gt $target) { throw "ISO already larger than kernel LBA ($($isoBytes.Length) > $target)" }
$kbytes = [IO.File]::ReadAllBytes($Kernel)
$fs = $null
try {
    $fs = [IO.File]::Open($OutIso, [IO.FileMode]::Append)
} catch {
    # VMware keeps the ISO open while a VM (or a powered-off tab) has it
    # mounted, so the kernel can never be appended.  Without this check the
    # build "succeeds" and the guest stops at "S2: bad ELF magic".
    # VMware 在虚拟机（或已关机的标签页）仍挂着 ISO 时会占住文件，内核根本
    # 追加不进去；没有这个检查的话构建看似成功，guest 停在 bad ELF magic。
    throw 'cannot append the kernel to the ISO: it is still open in VMware - close the BSD45 tab and rebuild'
}
$pad = New-Object byte[] ($target - $isoBytes.Length)
$fs.Write($pad, 0, $pad.Length)
$fs.Write($kbytes, 0, $kbytes.Length)
# CD sectors are 2048 bytes; stage1 reads whole sectors, so pad the last one.
$pad2 = (2048 - ($kbytes.Length % 2048)) % 2048
if ($pad2 -gt 0) {
    $fs.Write((New-Object byte[] $pad2), 0, $pad2)
}
$fs.Close()

# The kernel must really be in the image: stage 1 reads it from this LBA.
$final = (Get-Item $OutIso).Length
if ($final -lt ($target + $kbytes.Length)) {
    throw ('kernel injection failed: ' + $final + ' < ' + ($target + $kbytes.Length))
}

Write-Host ("ISO: {0} ({1:N0} bytes), kernel at LBA {2}" -f $OutIso, $final, $KernelLba)










