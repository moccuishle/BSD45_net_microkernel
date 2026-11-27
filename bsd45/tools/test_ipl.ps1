#
# Copyright (c) 2026 jialiang.  All rights reserved.
#

# test_ipl.ps1 - os.md mechanism 4 acceptance: 16-level IPL, unified
# softirq/hardware pending bitmap, strict > dispatch order, and the
# thread base_ipl/priority-inheritance state machine.
#
# The ipltest kernel self-test checks semantics; check_ipl_trace.ps1 then
# replays the recorded transitions independently and proves the monotonic
# and nesting invariants from the log alone.
param([string]$Iso = "")

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD45-ELF-x86.iso" }

$script = Join-Path $ws "tools\run_qemu_test.ps1"
$checker = Join-Path $ws "tools\check_ipl_trace.ps1"
$fails = 0

function Check([string]$name, [string]$log, [string[]]$needles) {
    foreach ($n in $needles) {
        if ($log.Contains($n)) {
            Write-Host ("  ok   {0}: found {1}" -f $name, $n) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL {0}: missing {1}" -f $name, $n) -ForegroundColor Red
            $script:fails++
        }
    }
}

Write-Host "os.md M4 acceptance: IPL 0..15 / unified pending / inheritance"
$log = (& $script -Iso $Iso -Keys @(
    "ipl",
    "ipltest",
    "ipltrace",
    "ipl") -WaitFor "ipltest: PASS" -TimeoutSec 20) -join "`n"

Check "levels"       $log @("ipl: 16 levels", "clock: 100 Hz", "IRQ0 -> IPL 5")
Check "vector map"   $log @("ipltest: vector map ok")
Check "raise/restore" $log @("ipltest: raise/restore ok")
Check "strict order" $log @("ipltest: order 12>7>3 ok")
Check "inheritance"  $log @("ipltest: thread ipl/inheritance ok")
Check "verdict"      $log @("ipltest: PASS")
Check "trace"        $log @("itrace:")

if ($log -match 'kernel TRAP') {
    Write-Host "  FAIL kernel trap" -ForegroundColor Red
    $fails++
}

# independent log checker
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ipl-" + [guid]::NewGuid().ToString() + ".log")
Set-Content -Path $tmp -Value $log -Encoding UTF8
& powershell -NoProfile -ExecutionPolicy Bypass -File $checker -Log $tmp
if ($LASTEXITCODE -ne 0) { $fails++ }
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

if ($fails) {
    Write-Host "`nos.md M4 IPL: FAILED ($fails)" -ForegroundColor Red
    exit 1
}
Write-Host "`nos.md M4 IPL: PASSED" -ForegroundColor Green
exit 0
