"""Drive the BSD45 serial console in QEMU (helper for SMP tests).

The bash harness (tools/run_qemu_test.sh) talks to QEMU's TCP serial with
bash's /dev/tcp, which does not connect reliably from Git Bash on Windows
(the socket opens but nothing is delivered).  Python's socket module has no
such problem, so the SMP runs use this script instead.

    python tools/qdrive.py --iso out/BSD45-ELF-x86.iso --mem 3072 --smp 8 \
        --timeout 300 --cmd shell --cmd smpon --cmd /bin/pthread
"""
import argparse
import os
import socket
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--mem", default="3072")
    ap.add_argument("--smp", default="8")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--boot-wait", type=float, default=180.0)
    ap.add_argument("--gap", type=float, default=1.5)
    ap.add_argument("--sync", action="store_true",
                    help="send the next command as soon as the shell prompt "
                         "comes back instead of sleeping --gap seconds; "
                         "commands with an explicit +N still sleep N seconds")
    # The shell prompt is written after "\r\n" from the previous line or
    # after a bare "\r" depending on which stdio path printed last, so both
    # forms have to be accepted - matching only "\r# " made the wait miss
    # roughly one command in fifty and burn --max-wait on it.
    ap.add_argument("--sync-anchor", default="bsd45>|\r# |\n# |\r\n# ",
                    help="prompt(s) that mean 'the command finished', "
                         "separated by | (default: monitor prompt and the "
                         "root shell prompt in all its line-ending forms)")
    ap.add_argument("--max-wait", type=float, default=120.0,
                    help="ceiling for one --sync wait (a wedged command is "
                         "detected here, not by the global --timeout)")
    ap.add_argument("--tail-wait", type=float, default=20.0,
                    help="how long to wait for 'user exit' after the last "
                         "command; without this a run that ends with `exit 0` "
                         "idles away the whole --timeout")
    ap.add_argument("--verbose", action="store_true",
                    help="print how long each command took (stderr)")
    ap.add_argument("--log", default="")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--cmdfile", default="",
                    help="file with one command per line, appended to --cmd; "
                         "a soak of a few hundred loops does not fit in the "
                         "Windows 32 KB command line")
    ap.add_argument("--abort-after", type=int, default=3,
                    help="give up after N consecutive commands never bring "
                         "the prompt back (0 = never); a wedged guest then "
                         "costs one --max-wait per command instead of the "
                         "whole --timeout")
    ap.add_argument("--qemu-extra", action="append", default=[],
                    help="extra QEMU argument, repeatable (e.g. "
                         "--qemu-extra -d --qemu-extra cpu_reset)")
    args = ap.parse_args()

    if args.cmdfile:
        with open(args.cmdfile, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    args.cmd.append(line)

    qemu = None
    for cand in ("qemu-system-i386",):
        qemu = cand
    port = 6000 + (os.getpid() % 500)
    argv = [qemu, "-cdrom", args.iso, "-boot", "d", "-m", args.mem,
            "-smp", args.smp, "-display", "none", "-no-reboot",
            "-monitor", "none"] + args.qemu_extra + [
            "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % port]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    buf = bytearray()
    deadline = time.time() + args.boot_wait
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=2)
            break
        except OSError:
            time.sleep(0.2)
    if sock is None:
        print("qdrive: cannot connect to serial port", file=sys.stderr)
        proc.kill()
        return 2
    sock.setblocking(False)

    def pump(until, hard_stop, quiet=None, start=0):
        """Read into buf until `until` appears (after offset `start`), the
        deadline passes, or (when `quiet` is set) no data has arrived for that
        many seconds.  `until` may be a string or a list of strings (any of
        them matches)."""
        needles = []
        if isinstance(until, str):
            needles = [until.encode()]
        elif until:
            needles = [u.encode() if isinstance(u, str) else u for u in until]
        last = time.time()
        while time.time() < hard_stop:
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    return False
                buf.extend(chunk)
                last = time.time()
                if args.log:
                    # append as we go: if the guest wedges, the log still
                    # shows how far it got
                    with open(args.log, "ab") as f:
                        f.write(chunk)
            except BlockingIOError:
                if quiet is not None and time.time() - last > quiet:
                    return True
                time.sleep(0.05)
                continue
            except OSError:
                return False
            if needles and any(buf.find(n, start) >= 0 for n in needles):
                return True
        return False

    hard = time.time() + args.timeout
    boot_deadline = min(hard, time.time() + args.boot_wait)
    ok = pump("bsd45>", min(boot_deadline, time.time() + 20))
    while not ok and time.time() < boot_deadline:
        # The monitor prompt is printed before the serial socket is connected
        # (this kernel boots in well under a second), so waiting for it alone
        # always ran out the whole --boot-wait.  Poke the monitor instead: one
        # empty line makes it print the prompt again, which is what makes a
        # soak start in seconds instead of minutes.
        # 提示符在内核启动时就打完了，比串口 socket 连上还早，所以光等必然
        # 耗尽整个 --boot-wait。这里敲一个空行让监视器重打提示符。
        try:
            sock.sendall(b"\r")
        except OSError:
            break
        ok = pump("bsd45>", min(boot_deadline, time.time() + 15),
                  start=len(buf))
    if not ok:
        print("qdrive: boot timeout (no monitor prompt)", file=sys.stderr)
    missed = 0
    for spec in args.cmd:
        # "command+SECONDS" waits that long after the command instead of the
        # global --gap (a 1000-process stress test needs minutes, `ps` doesn't)
        cmd, wait = spec, args.gap
        fixed = False
        if "+" in spec:
            head, _, tail = spec.rpartition("+")
            if tail.isdigit():
                cmd, wait, fixed = head, float(tail), True
        mark = len(buf)
        try:
            sock.sendall(cmd.encode() + b"\r")
        except OSError:
            break
        if args.sync and not fixed:
            # The prompt coming back *is* "the command finished".  Sleeping a
            # fixed --gap instead wasted most of the wall clock in soak runs
            # and could not tell "slow" from "wedged"; this can, and it costs
            # nothing when the command is slow because the wait is a ceiling.
            # 提示符回来就代表命令跑完了。原来固定等 --gap，soak 里大半时间
            # 都耗在盲等上，还分不清"慢"和"挂死"；这里上限是 --max-wait，
            # 命令慢不会误判。
            t0 = time.time()
            ok = pump(args.sync_anchor.split("|"),
                      min(hard, time.time() + args.max_wait), start=mark)
            if args.verbose:
                print("qdrive: %-28s %6.2fs%s"
                      % (cmd, time.time() - t0, "" if ok else "  (no prompt)"),
                      file=sys.stderr)
            if ok:
                missed = 0
            else:
                print("qdrive: no prompt after %r within %.0fs"
                      % (cmd, args.max_wait), file=sys.stderr)
                missed += 1
                if args.abort_after and missed >= args.abort_after:
                    print("qdrive: %d commands in a row never returned to the "
                          "prompt - the guest is wedged, stopping here"
                          % missed, file=sys.stderr)
                    break
            continue
        # Keep draining for the whole wait.  A long stress test (proctest)
        # prints through the serial port; if nobody reads, QEMU's TCP socket
        # buffer fills up and the guest blocks inside printf forever, so the
        # test looks hung even though the kernel is healthy.  The old
        # `time.sleep(wait)` did exactly that and produced rc=124 every time.
        pump(None, min(hard, time.time() + wait))
    pump("user exit", min(hard, time.time() + args.tail_wait))
    time.sleep(1.0)
    pump(None, time.time() + 1.0)

    try:
        sock.close()
    except OSError:
        pass
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    text = bytes(buf).decode("latin-1")
    sys.stdout.write(text)
    return 0 if "user exit" in text else 1


if __name__ == "__main__":
    sys.exit(main())
