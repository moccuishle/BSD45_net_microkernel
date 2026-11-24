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
    ap.add_argument("--log", default="")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--qemu-extra", action="append", default=[],
                    help="extra QEMU argument, repeatable (e.g. "
                         "--qemu-extra -d --qemu-extra cpu_reset)")
    args = ap.parse_args()

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

    def pump(until, hard_stop, quiet=None):
        """Read into buf until `until` appears, the deadline passes, or (when
        `quiet` is set) no data has arrived for that many seconds."""
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
            if until and buf.find(until.encode()) >= 0:
                return True
        return False

    hard = time.time() + args.timeout
    if not pump("bsd45>", min(hard, time.time() + args.boot_wait)):
        print("qdrive: boot timeout (no monitor prompt)", file=sys.stderr)
    for spec in args.cmd:
        # "command+SECONDS" waits that long after the command instead of the
        # global --gap (a 1000-process stress test needs minutes, `ps` doesn't)
        cmd, wait = spec, args.gap
        if "+" in spec:
            head, _, tail = spec.rpartition("+")
            if tail.isdigit():
                cmd, wait = head, float(tail)
        try:
            sock.sendall(cmd.encode() + b"\r")
        except OSError:
            break
        # Keep draining for the whole wait.  A long stress test (proctest)
        # prints through the serial port; if nobody reads, QEMU's TCP socket
        # buffer fills up and the guest blocks inside printf forever, so the
        # test looks hung even though the kernel is healthy.  The old
        # `time.sleep(wait)` did exactly that and produced rc=124 every time.
        pump(None, min(hard, time.time() + wait))
    pump("user exit", hard)
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
