#
# Copyright (c) 2026 jialiang.  All rights reserved.
#

# test_preempt.ps1 - os.md mechanism 2/4 acceptance: the PIT interrupt
# preempts CPU-bound user threads.  Two forked processes burn CPU without
# yielding; only the clock can switch them, so their output letters must
# interleave.  Ticks must also advance (getticks syscall 430).
param([string]$Iso = "")

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD45-ELF-x86.iso" }

$script = Join-Path $ws "tools\run_qemu_test.ps1"
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

Write-Host "os.md M2/M4 acceptance: timer preemption of user threads"
$log = (& $script -Iso $Iso -Keys @(
    "shell",
    "/bin/spin",
    "exit 0") -WaitFor "spin: PASS" -TimeoutSec 90) -join "`n"

Check "timer"   $log @("spin: timer ok")
Check "verdict" $log @("spin: PASS")

# The A/B run between the exec line and the summary is the evidence.  Count
# transitions: a non-preemptive scheduler produces exactly one (all B, then
# all A).  The threshold is 3, not 4: with 30 letters per process the guest
# lands on 3..5 transitions depending on how many letters fit in one
# quantum, so a threshold of 4 sat right on the boundary and failed ~10% of
# runs on an unchanged kernel.  3 still requires a switch away *and* back,
# which a non-preemptive scheduler cannot produce.
$m = [regex]::Match($log, '([AB]{8,})')
if ($m.Success) {
    $run = $m.Groups[1].Value
    $trans = 0
    for ($i = 1; $i -lt $run.Length; $i++) { if ($run[$i] -ne $run[$i-1]) { $trans++ } }
    Write-Host ("  info scheduler letters: {0} chars, {1} transitions" -f $run.Length, $trans)
    if ($trans -ge 3) {
        Write-Host "  ok   two processes interleaved under the timer" -ForegroundColor Green
    } else {
        Write-Host "  FAIL no interleaving: scheduler did not preempt" -ForegroundColor Red
        $fails++
    }
} else {
    Write-Host "  FAIL could not find A/B run in output" -ForegroundColor Red
    $fails++
}

if ($log -match 'kernel TRAP') {
    Write-Host "  FAIL kernel trap" -ForegroundColor Red
    $fails++
}

if ($fails) {
    Write-Host "`nos.md M2/M4 preempt: FAILED ($fails)" -ForegroundColor Red
    exit 1
}
Write-Host "`nos.md M2/M4 preempt: PASSED" -ForegroundColor Green
exit 0

