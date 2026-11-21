#!/usr/bin/env python3
"""A failed check takes the guest's state once, bounded, and never waits
on a guest that stopped answering -- against a fake console and QMP.

    python3 tools/hostchecks/failure_bundle_hostcheck.py
    python3 tools/hostchecks/failure_bundle_hostcheck.py --positive-control   # must FAIL

What it holds `tools/lib/failure_bundle.py` to: every file is written and
the index names the check; a second failure in the same process takes
nothing (the FIRST is the evidence); a guest command that comes back cut
short ends the ring-3 part while QMP still runs; every command carries a
deadline; `SAUNAOS_FAILURE_BUNDLE=off` is off.

`--positive-control` sends the guest commands with no deadline -- what
`send()` did before the bundle bounded it -- so the fake guest refuses
them and the run must fail.
"""

RUNNER = {
    "covers": "a failed check takes the guest's state once, bounded, without waiting on a stalled guest",
    "gate": True,
}

import argparse  # noqa: E402
import os  # noqa: E402
import shutil  # noqa: E402
import sys  # noqa: E402
import tempfile  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import failure_bundle as fb  # noqa: E402


class FakeConsole:
    def __init__(self, stall_at=None):
        self._all_log, self.log_lines, self._stale = ["klog one", "klog two"], [], False
        self._log_thread, self.sent, self.stall_at = object(), [], stall_at

    def send(self, cmd, timeout=None):
        if timeout is None:
            raise RuntimeError("unbounded guest command")
        self.sent.append(cmd)
        if cmd == self.stall_at:
            self._stale = True
            return ""
        return f"answer to {cmd}"


class FakeQMP:
    def hmp(self, cmd):
        return "RIP=ffffffff80001234"

    def screenshot(self, path, stable=True, settle=0.3):
        if stable:
            raise RuntimeError("waited for a settled frame from a guest that may be wedged")
        with open(path, "wb") as f:
            f.write(b"png")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    if args.positive_control:
        fb.CMD_TIMEOUT_S = None

    fails = []

    def ok(name, cond):
        print(f"  {'PASS' if cond else 'FAIL'}  {name}")
        if not cond:
            fails.append(name)

    def read(d, name):
        p = os.path.join(d or "", name)
        return open(p).read() if d and os.path.exists(p) else ""

    tmp = tempfile.mkdtemp(prefix="bundle-hostcheck-")
    os.environ.pop(fb.ENV_OFF, None)
    try:
        os.environ[fb.ENV_DIR] = os.path.join(tmp, "a")
        c, q = FakeConsole(), FakeQMP()
        fb.register_console(c)
        fb.register_qmp(q)
        d = fb.collect("the window opens", "win=None", tool="hostcheck")
        want = sorted(["index.txt", "klog.txt", "cpu-regs.txt", "screen.png"]
                      + [n for n, _ in fb.GUEST_COMMANDS])
        ok("a failure writes every file", d and sorted(os.listdir(d)) == want)
        ok("the index names the check and its detail",
           "the window opens" in read(d, "index.txt") and "win=None" in read(d, "index.txt"))
        ok("the klog tail is the console's log", "klog two" in read(d, "klog.txt"))
        ok("ps is the guest's answer", "answer to sh ps" in read(d, "ps.txt"))

        os.environ[fb.ENV_DIR] = os.path.join(tmp, "b")
        ok("a second failure takes nothing",
           fb.collect("again") is None and not os.path.exists(os.path.join(tmp, "b")))

        fb._taken = False
        fb._consoles.discard(c)
        stalled = FakeConsole(stall_at="sh cpus")
        fb.register_console(stalled)
        os.environ[fb.ENV_DIR] = os.path.join(tmp, "c")
        d = fb.collect("stalled", tool="hostcheck")
        ok("a stalled guest ends the ring-3 part", stalled.sent == ["sh dmesg -n 400", "sh ps", "sh cpus"])
        ok("QMP still runs after a stall", os.path.exists(os.path.join(d or "", "cpu-regs.txt")))

        fb._taken = False
        os.environ[fb.ENV_OFF] = "off"
        ok("SAUNAOS_FAILURE_BUNDLE=off takes nothing", fb.collect("off") is None)
    finally:
        os.environ.pop(fb.ENV_OFF, None)
        os.environ.pop(fb.ENV_DIR, None)
        shutil.rmtree(tmp, ignore_errors=True)
    total = 8
    print(f"\nfailure_bundle_hostcheck: {total - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
