#!/usr/bin/env python3
"""`panic_store=survey` finds the ranges a restart keeps: stamped on one
boot, reported KEPT on the next.

    python3 tools/tests/sweep/panic_store_survey_test.py [--instance 3]
    python3 tools/tests/sweep/panic_store_survey_test.py --positive-control   # must FAIL

A copy of disk.img boots with `panic_store=survey`: the first boot has no
earlier stamp and says so; a warm `reboot` (QEMU keeps RAM across one)
must then report every range it stamped as KEPT, and name them in the
summary line that says which `panic_store=` to use. ALL of them kept is
also the proof they were RESERVED: a desktop boot allocates the low
ranges, so one the allocator handed out comes back changed. Then
`panic_store=<a kept range>` keeps a real panic (panic_store_test.py
--boot-word does that half).

`--positive-control` restarts COLD instead -- QEMU stopped and started --
so nothing can survive and the KEPT checks must fail.
"""

RUNNER = {"covers": "panic_store=survey stamps candidate ranges and the next boot reports which a restart kept"}

import argparse  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import sys  # noqa: E402
import tempfile  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import install_grub  # noqa: E402
from harness import Results, copy_disk  # noqa: E402
from panic_store_test import BOOT_MARK, serial, vm, wait_for_guest, wait_serial  # noqa: E402

REPO = toolpath.REPO


def survey_lines(text):
    return [ln for ln in text.splitlines() if "panic_store: survey" in ln or "panic_store: SURVEY" in ln]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=3)
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    work = tempfile.mkdtemp(prefix="panic_survey_test.")
    disk, log = os.path.join(work, "disk.img"), os.path.join(work, "serial.log")
    copy_disk(args.disk, disk)
    ok, why = install_grub.add_boot_word(disk, "panic_store=survey")
    if not ok:
        print(f"panic_store_survey_test: cannot add the boot word: {why}")
        return 2
    res = Results()
    n = args.instance
    g = lambda *c, timeout=60: vm(n, disk, log, "exec", *c, timeout=timeout)  # noqa: E731
    try:
        vm(n, disk, log, "--reboot", "start", timeout=300)
        if not res.check("the guest boots", wait_for_guest(g), serial(log)[-400:]):
            return res.finish("panic_store_survey_test")
        first = survey_lines(g("dmesg"))
        stamped = [ln for ln in first if re.search(r"survey 0x[0-9a-f]+: no stamp", ln)]
        res.check("the first survey boot stamps ranges and has nothing earlier to read",
                  stamped and any("nothing from an earlier survey boot" in ln for ln in first),
                  "\n".join(first)[-500:])
        res.check("...and keeps no store that boot", any("no store this boot" in ln for ln in first))

        if args.positive_control:
            vm(n, disk, log, "stop")
            vm(n, disk, log, "--reboot", "start", timeout=300)
        else:
            before = serial(log).count(BOOT_MARK)
            g("reboot", timeout=10)
            wait_serial(log, lambda s: s.count(BOOT_MARK) > before, 60)
        if not res.check("the guest comes back", wait_for_guest(g), serial(log)[-400:]):
            return res.finish("panic_store_survey_test")
        second = survey_lines(g("dmesg"))
        kept = [ln for ln in second if ln.rstrip().endswith(": KEPT")]
        res.check("a warm restart keeps every range the first boot stamped (so none was allocated)",
                  len(stamped) > 0 and len(kept) == len(stamped),
                  f"stamped {len(stamped)}, kept {len(kept)}:\n" + "\n".join(second)[-700:])
        summary = next((ln for ln in second if "kept across the restart:" in ln), "")
        named = re.findall(r"0x[0-9a-f]+", summary.split("kept across the restart:")[-1])
        res.check("the summary names the kept ranges and the word to use",
                  "panic_store=<one of them>" in summary and len(named) == len(kept) and kept,
                  summary)
    finally:
        vm(n, disk, log, "stop")
        shutil.rmtree(work, ignore_errors=True)
    return res.finish("panic_store_survey_test")


if __name__ == "__main__":
    sys.exit(main())
