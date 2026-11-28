#!/usr/bin/env python3
"""The vendored uACPI against its own test suite, on the host.

WHAT IS UNDER TEST. kernel/third_party/uacpi/upstream/ -- the release
plus whatever patches/ applied -- run by upstream's own tests/run_tests.py:
every ASL case in tests/test-cases/ (and the generated ones) compiled with
iasl and executed by upstream's host test runner, then its API tests.
The runner is built here with the host gcc (no CMake), under ASan and
UBSan, in upstream's test CONFIGURATION (UACPI_SIZED_FREES, its own libc
helpers) -- not the kernel's (UACPI_FLAGS: OVERRIDE_LIBC,
NATIVE_ALLOC_ZEROED, no sized frees), whose callbacks only a kernel can
supply. So SaunaOS's glue and its options are the kernel's KTESTs' to
test, suite `uacpi`; this one tests the interpreter's own source.

So this is the check that a patch to the port, or a re-vendor, did not
break the interpreter -- the oracle is upstream's expectations, written
against Windows-compatible behaviour, not ours.

POSITIVE CONTROL (--positive-control): a copy of the interpreter whose
AddOp subtracts. The AML suite must go red.

    python3 tools/hostchecks/uacpi_hostcheck.py [--positive-control] [-j N]
"""

# uACPI's upstream AML + API tests against the vendored copy. ~30 s.
# OFF THE GATE until its 8 red API cases are understood (docs/test-flakes.md,
# 2026-11-28): they went red the first time iasl let it run at all.
RUNNER = {"covers": "the vendored uACPI interpreter against upstream's own test suite",
          "needs": ["iasl", "needs gcc and iasl (ACPICA's ASL compiler: pacman -S acpica, apt install acpica-tools)"]}

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
TOOL = "uacpi_hostcheck"
UPSTREAM = os.path.join(ROOT, "kernel", "third_party", "uacpi", "upstream")
RUNNER_SRCS = ["test_runner.c", "helpers.c", "interface_impl.c", "resource_tests.c",
               "api_tests.c", "aml_threads.c", "event_tests.c"]
# What upstream's tests/runner/CMakeLists.txt passes for a GCC build.
FLAGS = ["-std=gnu17", "-O1", "-g", "-Wall", "-Wextra", "-D_GNU_SOURCE", "-DUACPI_SIZED_FREES",
         "-fsanitize=address,undefined", "-pthread"]
CONTROL = ("        res = lhs + rhs;\n", "        res = lhs - rhs;\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    args = ap.parse_args()

    if not os.path.isdir(UPSTREAM):
        sys.exit(f"{TOOL}: no {os.path.relpath(UPSTREAM, ROOT)} -- run tools/fetch/uacpi_vendor.py")
    if not shutil.which("iasl"):
        print(f"{TOOL}: SKIP -- no iasl on PATH")
        return 77

    with tempfile.TemporaryDirectory(prefix=TOOL + "-") as tmp:
        # A copy of the interpreter's sources, so the control edits the
        # copy and never the vendored tree.
        src = os.path.join(tmp, "source")
        shutil.copytree(os.path.join(UPSTREAM, "source"), src)
        hostcheck.stage(tmp, "kernel/third_party/uacpi/upstream/source/interpreter.c",
                        dest="source/interpreter.c", edits=[CONTROL],
                        apply=args.positive_control, tool=TOOL)
        sources = sorted(glob.glob(os.path.join(src, "*.c")))
        sources += [os.path.join(UPSTREAM, "tests", "runner", f) for f in RUNNER_SRCS]
        exe = hostcheck.compile(tmp, "test-runner", sources, flags=FLAGS,
                                includes=[os.path.join(UPSTREAM, "include"),
                                          os.path.join(UPSTREAM, "tests", "runner")],
                                tool=TOOL)
        # run_tests.py writes its compiled cases beside a directory it
        # names; point it at the scratch tree, never at upstream/.
        r = subprocess.run([sys.executable, os.path.join(UPSTREAM, "tests", "run_tests.py"),
                            "--test-runner", exe, "--binary-directory", os.path.join(tmp, "bin"),
                            "--parallelism", str(args.jobs)],
                           cwd=tmp, capture_output=True, text=True)
        out = r.stdout + r.stderr
        lines = [ln for ln in out.splitlines() if ln.strip()]
        # The failures by name, wherever they fell -- the tail alone is the
        # summary and iasl's chatter, which names none of them.
        failed = [ln for ln in lines if "FAIL" in ln and "SUMMARY" not in ln]
        print("\n".join(failed[:40] + lines[-12:]))
        if r.returncode != 0:
            print(f"{TOOL}: FAIL -- upstream's suite went red (exit {r.returncode})")
            return 1
    print(f"{TOOL}: PASS -- upstream's AML and API tests, on the vendored copy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
