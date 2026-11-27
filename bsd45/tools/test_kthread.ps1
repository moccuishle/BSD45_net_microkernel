#
# Copyright (c) 2026 jialiang.  All rights reserved.
#

# test_kthread.ps1 - S21 acceptance: kernel threads and softirq carriers.
#
# kthread      - list kernel threads/stacks
# softirq      - list deferred-work handlers
# kthreadtest  - spawn a kernel thread, let it block/wake 3 times, exit
# softirqtest  - schedule a handler and verify the softirq thread ran it
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

Write-Host "S21 acceptance: kernel threads + softirq"
$log = (& $script -Iso $Iso -Keys @(
    "kthread",
    "softirq",
    "kthreadtest",
    "softirqtest",
    "kthread") -WaitFor "softirqtest: PASS" -TimeoutSec 20) -join "`n"

Check "stacks"      $log @("thread slots at fc000000", "16 KB stacks")
Check "soft thread" $log @("kthread: spawn softirq")
Check "thread test" $log @("kthreadtest: counter 3 PASS")
Check "softirq test" $log @("softirqtest: count 1 PASS")
Check "switches"    $log @("kthread: switches")
Check "reclaim handler" $log @("softirq: 0 runs")

if ($log -match 'TRAP') {
    Write-Host "  FAIL kernel trap" -ForegroundColor Red
    $fails++
}

if ($fails) {
    Write-Host "`nS21 kthread: FAILED ($fails)" -ForegroundColor Red
    exit 1
}
Write-Host "`nS21 kthread: PASSED" -ForegroundColor Green
exit 0
