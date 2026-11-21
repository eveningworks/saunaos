#!/usr/bin/env python3
"""A recorded session replays instruction for instruction: `vm.py --record`
then `vm.py --replay` give a byte-identical kernel log.

    python3 tools/tests/sweep/replay_test.py
    python3 tools/tests/sweep/replay_test.py --positive-control   # must FAIL

Records a boot plus two debug-console commands on a copy of disk.img,
stops it, replays the recording to its end, and compares COM1 as the
recording's relay kept it (FILE.com1.log) with what the replay wrote
(FILE.replay.com1.log). Also checks the replay ENDS by itself -- a
recording stopped with a power-down did not, because QMP's
system_powerdown is not a recorded event.

`--positive-control` records with the COM1 relay off
(SAUNAOS_RR_NO_RELAY), the shape that failed first: a replayed serial
write emits only what the recorded write delivered, and a socket nobody
read delivered nothing, so the replayed log is empty and the run fails.
"""

RUNNER = {"covers": "a vm.py --record session replays to a byte-identical kernel log and ends by itself"}

import argparse  # noqa: E402
import os  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, scratch_dir  # noqa: E402

VM = toolpath.tool("vm.py")


def vm(inst, *args, env=None, timeout=400):
    r = subprocess.run([sys.executable, VM, "--instance", str(inst), *args], cwd=toolpath.REPO,
                       capture_output=True, text=True, timeout=timeout, env=env)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=9)
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    res = Results()
    tmp = scratch_dir("replay_test")
    rec = os.path.join(tmp, "rec")
    env = dict(os.environ)
    if args.positive_control:
        env["SAUNAOS_RR_NO_RELAY"] = "1"
    n = args.instance

    rc, out = vm(n, "--record", rec, "--timeout", "300", "start", env=env)
    if not res.check("the recorded guest starts", rc == 0, out.strip()[-300:]):
        return res.finish("replay_test")
    rc, out = vm(n, "exec", "ps", "dmesg -n 2")
    res.check("debug-console commands answer under record", rc == 0 and "PID" in out, out[-200:])
    vm(n, "stop")
    if not os.path.exists(rec + ".com1.log"):          # the positive control's shape: no relay
        open(rec + ".com1.log", "wb").close()

    t0 = time.time()
    rc, out = vm(n, "--replay", rec, "start")
    pid = None
    try:
        with open(os.path.join(toolpath.REPO, f".vm.{n}.pid")) as f:
            pid = int(f.read().strip())
    except (OSError, ValueError):
        pass
    ended = False
    while pid and time.time() - t0 < 300:
        try:
            os.kill(pid, 0)
        except OSError:
            ended = True
            break
        time.sleep(0.5)  # sleep-ok: polls the replay's pid, which is what the check asserts
    res.check("the replay ends by itself at the end of the recording", ended,
              f"still running after {time.time() - t0:.0f} s" if not ended else "")
    if not ended:
        vm(n, "stop", "--hard")
    want = open(rec + ".com1.log", "rb").read()
    got = open(rec + ".replay.com1.log", "rb").read() if os.path.exists(rec + ".replay.com1.log") else b""
    res.check("the recording kept a kernel log", len(want) > 2000, f"{len(want)} bytes")
    res.check("the replay's kernel log is byte-identical", want == got and len(got) > 2000,
              f"recorded {len(want)} bytes, replayed {len(got)}; first difference at "
              f"{next((i for i, (a, b) in enumerate(zip(want, got)) if a != b), min(len(want), len(got)))}")
    return res.finish("replay_test")


if __name__ == "__main__":
    sys.exit(main())
