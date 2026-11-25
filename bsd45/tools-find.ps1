# tools-find.ps1 - locate MSYS2 minw32 / mkisofs / QEMU on this machine.
# Dot-source it from the build scripts:  . (Join-Path $ws "tools-find.ps1")

function Find-Msys2Root {
    $cands = @()
    if ($env:MSYS2_ROOT) { $cands += $env:MSYS2_ROOT }
    $cands += @(
        "C:\msys64",
        "C:\msys2",
        (Join-Path $env:ProgramFiles "msys64"),
        (Join-Path ${env:ProgramFiles(x86)} "msys64")
    )
    foreach ($r in $cands) {
        if (-not $r) { continue }
        foreach ($sub in @("mingw32", "ucrt64", "mingw64")) {
            if (Test-Path (Join-Path $r "$sub\bin\gcc.exe")) { return (Resolve-Path $r).Path }
        }
    }
    $g = Get-Command "i686-w64-mingw32-gcc.exe" -ErrorAction SilentlyContinue
    if (-not $g) { $g = Get-Command "gcc.exe" -ErrorAction SilentlyContinue }
    if ($g -and $g.Source -match "^(.*)\\(mingw32|ucrt64|mingw64)\\bin\\[^\\]+$") { return $matches[1] }
    return $null
}

# The build only needs a gcc that can emit 32-bit COFF (-m32).  MSYS2's
# mingw32 (i686) toolchain is the classic choice, but any ucrt64/mingw64
# (x86_64) gcc also works: gas/ld/objcopy are multi-target and the driver
# accepts -m32.  Prefer an i686 toolchain when it exists.
function Find-CcBin {
    $msys = Find-Msys2Root
    $cands = @()
    if ($msys) {
        $cands += @(
            (Join-Path $msys "mingw32\bin"),
            (Join-Path $msys "ucrt64\bin"),
            (Join-Path $msys "mingw64\bin")
        )
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c "gcc.exe"))) { return $c }
    }
    $g = Get-Command "gcc.exe" -ErrorAction SilentlyContinue
    if ($g) { return (Split-Path -Parent $g.Source) }
    return $null
}

# value for gcc's -B switch: tell the driver where its as/ld live.
function Get-CcBinArg([string]$bin) {
    if (!$bin) { $bin = Get-MingwBin }
    return (($bin -replace "\\", "/") + "/")
}

function Get-MingwBin {
    $bin = Find-CcBin
    if ($bin) {
        $msys = Find-Msys2Root
        $env:PATH = "$bin;" + (Join-Path $msys "usr\bin") + ";" + $env:PATH
        return $bin
    }
    throw @"
No gcc found (looked for mingw32, ucrt64 and mingw64 under C:\msys64).
Install an MSYS2 toolchain:
  pacman -S mingw-w64-i686-gcc          # classic, or
  pacman -S mingw-w64-ucrt-x86_64-gcc   # works too (-m32)
or set the MSYS2_ROOT environment variable to the MSYS2 install directory.
"@
}

function Get-GccInclude([string]$bin) {
    $gcc = Join-Path $bin "gcc.exe"
    $inc = (& $gcc -print-file-name=include 2>$null | Select-Object -First 1)
    if ($inc -and (Test-Path $inc)) { return ($inc -replace "\\", "/") }
    $base = Join-Path $bin "..\lib\gcc\i686-w64-mingw32"
    $d = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | Select-Object -First 1
    if ($d) { return ((Join-Path $d.FullName "include") -replace "\\", "/") }
    throw "cannot locate the GCC include directory under $bin"
}

# Link a PE image through an ld response file.  Passing 100+ object files on
# the Windows command line makes binutils 2.47 ld die with 0xC0000409 before
# printing anything (seen when linking /bin/sh, 140 objects).  An @file with
# forward slashes is the portable fix; backslashes would be eaten as escapes.
function Invoke-LdRsp {
    param(
        [string]$Ld,
        [string[]]$LdFlags,
        [string]$Out,
        [string[]]$Objects,
        [string[]]$Tail = @(),
        [string]$RspPath
    )
    if (!$RspPath) {
        $RspPath = Join-Path ([IO.Path]::GetTempPath()) ("bsd45-ld-" + [guid]::NewGuid().ToString("N") + ".rsp")
    }
    ($Objects | ForEach-Object { $_ -replace '\\', '/' }) | Set-Content -Encoding ASCII $RspPath
    $log = & $Ld @LdFlags -o $Out "@$RspPath" @Tail 2>&1
    $code = $LASTEXITCODE
    $log | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
    return $code
}

function Get-Mkisofs {
    $cands = @()
    if ($env:MKISOFS) { $cands += $env:MKISOFS }
    $cands += @(
        (Join-Path ${env:ProgramFiles(x86)} "VMware\VMware Workstation\mkisofs.exe"),
        (Join-Path $env:ProgramFiles "VMware\VMware Workstation\mkisofs.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "VMware\VMware Player\mkisofs.exe"),
        "C:\msys64\usr\bin\mkisofs.exe",
        "C:\msys64\usr\bin\xorriso.exe"
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    foreach ($n in "mkisofs.exe", "xorriso.exe") {
        $cmd = Get-Command $n -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw @"
mkisofs / xorriso not found.
Install cdrtools/xorriso, or VMware Workstation (ships mkisofs.exe),
or set the MKISOFS environment variable to the executable.
"@
}

function Get-Qemu {
    $cands = @()
    if ($env:QEMU) { $cands += $env:QEMU }
    $msys = Find-Msys2Root
    if ($msys) {
        $cands += @(
            (Join-Path $msys "ucrt64\bin\qemu-system-i386.exe"),
            (Join-Path $msys "mingw64\bin\qemu-system-i386.exe")
        )
    }
    $cands += @(
        (Join-Path $env:ProgramFiles "qemu\qemu-system-i386.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "qemu\qemu-system-i386.exe")
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    $cmd = Get-Command "qemu-system-i386.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# ---------------------------------------------------------------- python
# python.exe on PATH is often the Microsoft Store *stub* in
# %LOCALAPPDATA%\Microsoft\WindowsApps: running it opens the Store and
# returns non-zero without executing anything.  build_kernel.ps1 used to
# call `Get-Command python` and then silently skip tools/pe_bss_fix.py,
# which is exactly what inflated the Windows kernel image from ~170 KB to
# 618 KB and made the BIOS staging area overrun 0xA0000 (see
# os_vmware_debug.md).  Resolve a *real* interpreter and prove it runs.
function Test-PythonExe([string]$exe) {
    if (!$exe -or !(Test-Path $exe)) { return $false }
    if ($exe -match '\\WindowsApps\\') { return $false }
    try {
        $out = & $exe -c "print(1)" 2>$null
        return ("$out".Trim() -eq "1")
    } catch { return $false }
}

function Get-PythonExe {
    $cands = @()
    if ($env:PYTHON) { $cands += $env:PYTHON }
    $msys = Find-Msys2Root
    if ($msys) {
        $cands += @(
            (Join-Path $msys "ucrt64\bin\python.exe"),
            (Join-Path $msys "mingw64\bin\python.exe"),
            (Join-Path $msys "usr\bin\python.exe")
        )
    }
    $cands += @(
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python313\python.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\python.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python311\python.exe"),
        (Join-Path $env:ProgramFiles "Python313\python.exe"),
        (Join-Path $env:ProgramFiles "Python312\python.exe"),
        (Join-Path $env:ProgramFiles "Python311\python.exe")
    )
    foreach ($n in "python3.exe", "python.exe") {
        $cmd = Get-Command $n -ErrorAction SilentlyContinue
        if ($cmd) { $cands += $cmd.Source }
    }
    # versioned installs that are not on PATH
    foreach ($glob in @(
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python*\python.exe"),
        (Join-Path $env:USERPROFILE ".workbuddy\binaries\python\versions\*\python.exe"),
        "C:\Python*\python.exe"
    )) {
        Get-ChildItem $glob -ErrorAction SilentlyContinue | ForEach-Object {
            $cands += $_.FullName
        }
    }
    foreach ($c in $cands) {
        if (Test-PythonExe $c) { return $c }
    }
    return $null
}
