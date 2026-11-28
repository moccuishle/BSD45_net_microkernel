#!/usr/bin/env bash
# soak_p1.sh - 8-core mixed-load soak for RESUME-TASK §7.1 (P1).
#
#   bash tools/soak_p1.sh [loops] [smp] [mem]
#
#     loops  mixed-load iterations              (default 300)
#     smp    vCPUs                              (default 8)
#     mem    guest RAM in MB                    (default 3072)
#
# Why it is built this way (see §7.1 "3 小时" and qdrive.py --sync):
#   * ONE QEMU for the whole run.  A boot costs minutes under TCG, so paying
#     it once is the only way to get hundreds of loops done.
#   * Every command waits for the shell prompt instead of a fixed --gap.  The
#     old soak spent ~55% of its wall clock sleeping (6 x 1.5 s per loop) and
#     could not tell "slow" from "wedged".
#   * A loop keeps the exec count low but the load high: pthread (4000 sync
#     ops + join + detach) and aiotest (the S432 aio handshake) do the real
#     work; ls/ps/date keep the BKL-free path walk and fd table exercised.
#     rdstress (fork + concurrent read) every 10 loops, pthpre/pthmain every
#     25.
#   * --abort-after stops the run when the guest stops answering, instead of
#     burning the whole --timeout on a wedged machine.
set -u
LOOPS=${1:-300}
SMP=${2:-8}
MEM=${3:-3072}

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.." || exit 1
export PATH=/c/msys64/ucrt64/bin:$PATH
export MSYS_NO_PATHCONV=1

mkdir -p /c/tmp
CMDFILE=/c/tmp/soak_p1.cmds
LOG=/c/tmp/soak_p1.log
VLOG=/c/tmp/soak_p1.verbose
rm -f "$CMDFILE" "$LOG" "$VLOG"

{
	echo smpon
	echo smpuser
	echo shell
	for ((i = 1; i <= LOOPS; i++)); do
		echo "/bin/pthread"
		echo "/bin/aiotest"
		echo "ls /bin"
		echo "ps"
		echo "date"
		if ((i % 10 == 0)); then
			echo "/bin/rdstress"
		fi
		if ((i % 25 == 0)); then
			echo "/bin/pthpre"
			echo "/bin/pthmain"
		fi
	done
	# +5: the shell dies and never prints a prompt again, so do not make the
	# sync wait burn --max-wait on the last command
	echo "exit 0+5"
} >"$CMDFILE"

echo "soak: $LOOPS loops, smp=$SMP, mem=${MEM}MB, $(wc -l <"$CMDFILE") commands"
echo "soak: log=$LOG verbose=$VLOG"

python tools/qdrive.py --iso out/BSD45-ELF-x86.iso --mem "$MEM" --smp "$SMP" \
	--boot-wait 300 --timeout $((LOOPS * 25 + 1200)) \
	--sync --max-wait 90 --tail-wait 20 --verbose --abort-after 3 \
	--cmdfile C:/tmp/soak_p1.cmds --log C:/tmp/soak_p1.log 2>"$VLOG"
RC=$?

count() { n=$(grep -ci "$2" "$1" 2>/dev/null); echo "${n:-0}"; }

echo "=== soak 结果 ==="
echo "qdrive rc       : $RC"
echo "user exit 0     : $(count "$LOG" 'user exit 0')"
echo "TRAP            : $(count "$LOG" 'TRAP')"
echo "user fault      : $(count "$LOG" 'user fault')"
echo "triple fault    : $(count "$LOG" 'triple fault')"
echo "STACK OVERFLOW  : $(count "$LOG" 'STACK OVERFLOW')"
echo "no prompt       : $(count "$VLOG" 'no prompt after')"
echo "pthread PASS    : $(count "$LOG" 'pthreadtest: PASS')"
echo "aiotest PASS    : $(count "$LOG" 'aiotest: PASS')"
