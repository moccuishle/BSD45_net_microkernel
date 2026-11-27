#!/usr/bin/env bash
# test_preempt.sh - os.md M2/M4: timer preemption of CPU-bound user threads.
set -uo pipefail
ws="$(cd "$(dirname "$0")/.." && pwd)"
iso="${1:-$ws/out/BSD45-ELF-x86.iso}"
[ -f "$iso" ] || { echo "ISO not found: $iso" >&2; exit 1; }
fails=0

check() {
    local name="$1" log="$2" n
    shift 2
    for n in "$@"; do
        if printf '%s' "$log" | grep -qF -- "$n"; then echo "  ok   $name: found $n"
        else echo "  FAIL $name: missing $n" >&2; fails=$((fails + 1)); fi
    done
}

echo "os.md M2/M4 acceptance: timer preemption of user threads"
log="$("$ws/tools/run_qemu_test.sh" -Iso "$iso" \
    -Keys "shell|/bin/spin|exit 0" -WaitFor "spin: PASS" -TimeoutSec 90)"

check "timer"   "$log" "spin: timer ok"
check "verdict" "$log" "spin: PASS"

run="$(printf '%s' "$log" | grep -oE '^[AB]+$' | head -1 || true)"
if [ -n "$run" ]; then
    trans=$(printf '%s' "$run" | awk '{n=0; for(i=2;i<=length($0);i++) if(substr($0,i,1)!=substr($0,i-1,1)) n++; print n}')
    echo "  info scheduler letters: ${#run} chars, $trans transitions"
    # 3, not 4: a non-preemptive scheduler yields exactly 1 transition, and
    # the observed distribution for 30 letters is 3..5, so 4 was a boundary
    # value.  See test_preempt.ps1 for the full rationale.
    if [ "$trans" -ge 3 ]; then echo "  ok   two processes interleaved under the timer"
    else echo "  FAIL no interleaving" >&2; fails=$((fails+1)); fi
else
    echo "  FAIL could not find A/B run" >&2; fails=$((fails+1))
fi

printf '%s' "$log" | grep -q 'kernel TRAP' && { echo "  FAIL kernel trap" >&2; fails=$((fails+1)); }

if [ "$fails" -ne 0 ]; then echo "os.md M2/M4 preempt: FAILED ($fails)" >&2; exit 1; fi
echo "os.md M2/M4 preempt: PASSED"
exit 0
