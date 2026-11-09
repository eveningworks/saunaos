#!/usr/bin/env python3
"""GNU make ON SaunaOS: a Makefile builds, rebuilds and cleans as the host's make does.

docs/cc-design.md, stage 4's exit criterion: on a guest, /usr/bin/make
(`make native-toolchain`, then `make iso`) drives /usr/bin/cc and c++
through one Makefile -- a two-file C program with a header dependency, a
C++ program, and a sub-make -- and every step does what the HOST's GNU
make does with the same files:

  1. `make calc` from nothing builds the two-file C program, and `make`
     then the C++ one and the sub-make's, and all three run;
  2. `make` again runs no command;
  3. `touch util.h` -> `make` recompiles main.o and util.o, which include
     it, and relinks calc -- and builds nothing of the C++ program or the
     sub-make's; then msg.h CHANGED (a different message) -> `make`
     recompiles main.o alone and relinks, and calc prints the new message;
  4. `make clean` removes every product, through /bin/sh (a glob), and
     a second one, with nothing left to remove, still succeeds (rm -f);
  4a. msg.h changed back and forth with NO pause -> `make calc` after
     each recompiles main.o both times, the second edit landing in the
     same second as the object it outdates (file times finer than a
     second, `st_mtim`), and calc prints the second message;
  5. `make -j2` builds it all again, through the jobserver and the
     sub-make it is passed to;
  5a. `make -O -j2` the same, its output grouped by target -- which
     takes an fcntl() write lock per job (and the sub-make opens the
     same lock file), and must not warn "cannot acquire output lock";
  6. `make -j2 naps` runs two one-second sleeps AT ONCE.

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - "make exited 0" passes a make that rebuilt everything every time, or
    nothing: each step's printed COMMANDS are compared with the host make
    running the same step on the same files (sorted, for -j2, whose order
    is the scheduler's), so a missing rebuild and an extra one both fail;
  - "it rebuilt main.o" passes a stale object relinked: calc's OUTPUT
    after step 3 must carry the new header's message, line for line with
    the host build's;
  - "-j2 built it" passes a make that ignored -j: step 6 is timed by the
    guest's clock and must take well under the two seconds a serial run
    takes (measured just before it, in the same guest).

It also prints the time of step 1's two makes (the two-file program; the
C++ one and the sub-make) and step 3's by the guest's own clock; only a
--kvm figure is a speed (CLAUDE.md: TCG says nothing about speed).

  python3 tools/tests/sweep/native_make_test.py [--instance N] [--kvm] [--mem MIB]
  python3 tools/tests/sweep/native_make_test.py --positive-control
      # main.o's rule loses msg.h: step 3 must FAIL (no rebuild, stale output)

A SKIP (exit 77) unless this checkout built the native toolchain and make
and seeded them: `make native-toolchain`, then `make iso`. Boots its own
guest on a COPY of disk.img.
"""

RUNNER = {"covers": "GNU make on the guest builds, rebuilds only what changed, cleans and runs -j2 as the host's make does",
          "needs": ["native_make", "no native make on the image -- "
                    "`make native-toolchain`, then `make iso`"]}

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, copy_disk, poll  # noqa: E402

REPO = toolpath.REPO
GDIR = "/tmp/nm"

MAKEFILE = """\
# Stage 4's exit criterion (tools/tests/sweep/native_make_test.py).
CFLAGS = -O2
CXXFLAGS = -O2

all: calc shapes sub/hello

calc: main.o util.o
\t$(CC) $(CFLAGS) -o $@ main.o util.o

main.o: main.c util.h msg.h
util.o: util.c util.h

shapes: shapes.o
\t$(CXX) $(CXXFLAGS) -o $@ shapes.o

sub/hello: sub/hello.c
\t$(MAKE) -C sub

naps: nap1 nap2
nap1 nap2:
\tsleep 1

clean:
\trm -f calc shapes *.o
\t$(MAKE) -C sub clean

.PHONY: all clean naps nap1 nap2
"""
SUB_MAKEFILE = """\
hello: hello.c
\t$(CC) -O2 -o $@ hello.c

clean:
\trm -f hello
"""
UTIL_H = "int mix(int a, int b);\nlong fold(const int *v, int n);\n"
MSG_V1 = '#define GREETING "calc: built by make on SaunaOS"\n'
MSG_V2 = '#define GREETING "calc: rebuilt after msg.h changed"\n'
MAIN_C = r"""
#include <stdio.h>
#include "util.h"
#include "msg.h"
int main(void) {
    int v[16];
    for (int i = 0; i < 16; i++) v[i] = mix(i, 16 - i);
    printf("%s\n", GREETING);
    printf("calc: fold %ld\n", fold(v, 16));
    printf("calc: done\n");
    return 0;
}
"""
UTIL_C = r"""
#include "util.h"
int mix(int a, int b) { return a * 5 - b * 3 + (a & b); }
long fold(const int *v, int n) { long s = 0; for (int i = 0; i < n; i++) s = s * 31 + v[i]; return s; }
"""
SHAPES_CPP = r"""
#include <iostream>
#include <memory>
#include <vector>
struct Shape { virtual ~Shape() = default; virtual int area() const = 0; };
struct Sq : Shape { int s; explicit Sq(int a) : s(a) {} int area() const override { return s * s; } };
int main() {
    std::vector<std::unique_ptr<Shape>> v;
    for (int i = 1; i <= 4; i++) v.push_back(std::make_unique<Sq>(i));
    int t = 0;
    for (auto &s : v) t += s->area();
    std::cout << "shapes: total " << t << "\n" << "shapes: done" << std::endl;
}
"""
HELLO_C = r"""
#include <stdio.h>
int main(void) { printf("sub: hello from a sub-make\nsub: done\n"); return 0; }
"""
TICK_C = r"""
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    printf("%lld\n", (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000);
    return 0;
}
"""

PRODUCTS = "calc shapes main.o util.o shapes.o sub/hello"
RUN_ALL = ["./calc", "./shapes", "./sub/hello"]
# (name, the shell lines of the step, whether its command order is the
# scheduler's). Each step's make output is <name>.log; its programs'
# output <name>.out. The `sleep 2`s are from when TFS3's times were whole
# seconds; "fast" is the step that needs them not to be.
STEPS = [
    ("calc", ["make calc", "./calc"], False),
    ("build", ["make", *RUN_ALL], False),
    ("again", ["make"], False),
    ("touch", ["sleep 2", "touch util.h", "make"], False),
    ("header", ["sleep 2", "cp msg_v2.h msg.h", "make", "./calc"], False),
    # No pause anywhere: the second cp lands within the second the first
    # `make calc` built main.o in, which only a sub-second time can order.
    ("fast", ["cp msg_v1.h msg.h", "make calc", "cp msg_v2.h msg.h", "make calc", "./calc"], False),
    # twice: the second removes nothing, so its `rm -f` meets only
    # missing names and an unmatched `*.o` -- and must still succeed.
    ("clean", ["make clean",
               f"for f in {PRODUCTS}; do if [ -e $f ]; then echo left: $f; fi; done",
               "echo cleaned", "make clean"], False),
    ("j2", ["make -j2", *RUN_ALL], True),
    ("osync", ["make clean", "make -O -j2", *RUN_ALL], True),
    ("serial", ["make naps"], False),
    ("naps", ["make -j2 naps"], True),
]


def files(positive_control):
    mk = MAKEFILE
    if positive_control:
        mk = mk.replace("main.o: main.c util.h msg.h", "main.o: main.c util.h")
    return {"Makefile": mk, "util.h": UTIL_H, "msg.h": MSG_V1, "msg_v1.h": MSG_V1, "msg_v2.h": MSG_V2,
            "main.c": MAIN_C, "util.c": UTIL_C, "shapes.cpp": SHAPES_CPP,
            "sub/Makefile": SUB_MAKEFILE, "sub/hello.c": HELLO_C, "tick.c": TICK_C}


def step_script(d, name, lines, guest):
    """One step as a dash script: make's output to <name>.log, the
    programs' to <name>.out, the exit status of its make lines to
    <name>.rc -- and on the guest, its own clock either side of the make
    lines alone (not the sleep before them or the programs after)."""
    out = [f"cd {d}"]
    if guest:
        out = ["export PATH=/bin:/usr/bin"] + out
    out += [f": > {name}.log", f": > {name}.out", "rc=0"]
    makes = [i for i, ln in enumerate(lines) if ln.startswith("make")]
    for i, ln in enumerate(lines):
        if guest and i == makes[0]:
            out.append(f"./tick > {name}.t0")
        if ln.startswith("make"):
            out.append(f"{ln} >> {name}.log 2>&1 || rc=$?")
        else:
            out.append(f"{ln} >> {name}.out 2>&1")
        if guest and i == makes[-1]:
            out.append(f"./tick > {name}.t1")
    out.append(f"echo $rc > {name}.rc")
    return "\n".join(out) + "\n"


def make_lines(text, d):
    """make's output, with this run's directory spelled the guest's way."""
    return [ln.rstrip().replace(d, GDIR) for ln in text.splitlines() if ln.strip()]


def host_run(tmp, positive_control):
    """The oracle: every step by the host's own GNU make, cc and c++."""
    d = os.path.join(tmp, "host")
    for n, text in files(positive_control).items():
        os.makedirs(os.path.dirname(os.path.join(d, n)), exist_ok=True)
        with open(os.path.join(d, n), "w") as f:
            f.write(text)
    got = {}
    for name, lines, _ in STEPS:
        if name in ("serial", "naps"):
            continue
        subprocess.run(["sh", "-c", step_script(d, name, lines, guest=False)], check=True,
                       env={**os.environ, "MAKEFLAGS": "", "MAKELEVEL": ""})
        with open(os.path.join(d, f"{name}.log")) as f:
            log = make_lines(f.read(), d)
        with open(os.path.join(d, f"{name}.out")) as f:
            out = [ln.rstrip() for ln in f.read().splitlines()]
        got[name] = (log, out)
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--instance", type=int, default=6, metavar="N",
                    help="the VM slot (vm.py's numbering; default 6)")
    ap.add_argument("--disk", default="disk.img", help="the image to boot a COPY of")
    ap.add_argument("--kvm", action="store_true", help="time it on KVM (the only speed worth quoting)")
    ap.add_argument("--mem", type=int, default=0, metavar="MIB", help="guest RAM (default: vm.py's)")
    ap.add_argument("--timeout", type=int, default=1200,
                    help="seconds one step may take (TCG is slow: default 1200)")
    ap.add_argument("--positive-control", action="store_true",
                    help="main.o's rule loses msg.h: the header step must fail")
    args = ap.parse_args()

    sync = os.path.join(REPO, "seed", "sync", "usr", "bin")
    if not all(os.access(os.path.join(sync, b), os.X_OK) for b in ("cc", "make")):
        print("native_make_test: SKIP -- the seed tree has no /usr/bin/make or cc; "
              "`make native-toolchain`, then `make iso`")
        return 77
    tmp = tempfile.mkdtemp(prefix="native_make-")
    try:
        return run(args, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(args, tmp):
    res = Results()
    if args.positive_control:
        print("native_make_test: POSITIVE CONTROL -- main.o no longer depends on msg.h; "
              "the header step must FAIL")
    # The host always runs the REAL Makefile: the positive control's is
    # the guest's only. And the oracle itself must show the rebuild, or
    # the comparison below proves nothing about it.
    want = host_run(tmp, False)
    res.check("the host's make rebuilds main.o after msg.h changes (the oracle works)",
              any("main.o" in ln and " -c " in ln for ln in want["header"][0]),
              "\n".join(want["header"][0]))

    disk = copy_disk(args.disk, os.path.join(tmp, "disk.img"), cwd=REPO)

    def vm(*argv, timeout=180):
        r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
                            "--disk", disk, *(["--kvm"] if args.kvm else []),
                            *(["--mem", str(args.mem)] if args.mem else []), *argv],
                           cwd=REPO, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr

    def cat(path):
        return vm("exec", f"cat {path}")

    def run_script(name, text):
        """Put a dash script on the guest, start it, wait for its .rc."""
        local = os.path.join(tmp, f"{name}.sh")
        with open(local, "w") as f:
            f.write(text)
        vm("put", local, f"{GDIR}/{name}.sh")
        vm("exec", f"rm {GDIR}/{name}.rc")
        vm("exec", f"spawn /bin/dash {GDIR}/{name}.sh")
        rc = poll(lambda: (lambda o: o.strip().splitlines()[-1]
                           if re.search(r"^\d+\s*$", o, re.M) else None)(cat(f"{GDIR}/{name}.rc")),
                  args.timeout)
        return rc.strip() if rc else None

    def guest_secs(name):
        try:
            us = [int(cat(f"{GDIR}/{name}.t{i}").split()[-1]) for i in (0, 1)]
            return (us[1] - us[0]) / 1e6
        except (ValueError, IndexError):
            return None

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start", timeout=300)):
        return res.finish("native_make_test")
    try:
        ls = vm("exec", "ls /usr/bin")
        res.check("the image carries /usr/bin/make and /usr/bin/cc",
                  "make" in ls.split() and "cc" in ls.split(), ls.strip()[:200])
        vm("exec", f"mkdir {GDIR}")
        vm("exec", f"mkdir {GDIR}/sub")
        for n, text in files(args.positive_control).items():
            local = os.path.join(tmp, n.replace("/", "_"))
            with open(local, "w") as f:
                f.write(text)
            out = vm("put", local, f"{GDIR}/{n}")
            res.check(f"{n} is copied in", "fail" not in out.lower(), out.strip()[:200])

        # The stopwatch, compiled with no make at all.
        tick = run_script("tick", f"export PATH=/bin:/usr/bin\ncd {GDIR}\n"
                                  f"cc -O2 tick.c -o tick > tick.log 2>&1\necho $? > tick.rc\n")
        if not res.check("the guest's stopwatch compiles", tick == "0", cat(f"{GDIR}/tick.log")[-600:]):
            return res.finish("native_make_test")

        serial = None
        for name, lines, unordered in STEPS:
            rc = run_script(name, step_script(GDIR, name, lines, guest=True))
            log = make_lines(cat(f"{GDIR}/{name}.log"), GDIR)
            secs = guest_secs(name)
            if not res.check(f"{name}: `{'; '.join(lines)}` exits 0 on the guest", rc == "0",
                             f"exit {rc!r}; make said:\n" + "\n".join(log)[-1500:]):
                continue
            if name in ("calc", "build", "touch", "header"):
                print(f"native_make_test: {name}: {secs:.2f} s by the guest clock "
                      f"({'KVM' if args.kvm else 'TCG -- not a speed figure'})"
                      if secs is not None else f"native_make_test: {name}: no guest time")
            if name == "serial":
                serial = secs
                continue
            if name == "osync":
                warn = [ln for ln in log if "output lock" in ln.lower() or "fcntl" in ln.lower()]
                res.check("make -O takes its output lock (no warning)", not warn, "\n".join(warn))
            if name == "naps":
                res.check("make -j2 runs two one-second jobs at once",
                          secs is not None and serial is not None and secs < serial - 0.6,
                          f"-j2 {secs} s, serial {serial} s")
                continue
            wlog, wout = want[name]
            same = sorted(log) == sorted(wlog) if unordered else log == wlog
            pad = max(len(wlog), len(log))
            diff = [f"    host: {w!r}\n    test: {g!r}"
                    for w, g in zip(wlog + [""] * pad, log + [""] * pad) if w != g]
            res.check(f"{name}: make ran the same commands as the host's make"
                      f"{' (in any order)' if unordered else ''}", same, "\n".join(diff)[:1500])
            out = [ln.rstrip() for ln in cat(f"{GDIR}/{name}.out").splitlines()
                   if re.match(r"^(calc|shapes|sub|left|cleaned)", ln)]
            diff = [f"    host: {w!r}\n    test: {g!r}"
                    for w, g in zip(wout + [""] * len(out), out + [""] * len(wout)) if w != g]
            res.check(f"{name}: its programs' output matches the host build's "
                      f"({len(wout)} lines)", out == wout, "\n".join(diff)[:1500])
    finally:
        vm("stop")
    return res.finish("native_make_test")


if __name__ == "__main__":
    sys.exit(main())
