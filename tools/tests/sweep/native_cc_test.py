#!/usr/bin/env python3
"""GCC ON SaunaOS: C and C++ compiled on a guest run as the host's build does.

docs/cc-design.md, stage 3's exit criterion: on the machine, `cc hello.c
-o hello && ./hello`, the same with `c++` (iostream and an exception), and a
two-file C program -- every one COMPILED, ASSEMBLED AND LINKED on the guest
by /usr/bin/cc and /usr/bin/c++ (`make native-toolchain`, then `make iso`).

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - "the compiler exited 0" passes a driver that wrote no file or a link
    with no start code: each program must RUN and print to its last line;
  - "it printed something" passes a miscompile that drops a line or gets
    a number wrong: the output is compared LINE FOR LINE with the same
    source built by the host's gcc/g++ and run on the host;
  - "C works" says nothing about the driver's other paths: the two-file
    program is compiled with -c into two objects and linked by a separate
    `cc` call, the C one is built again with -pipe (cc1 into as through a
    pipe, which starts the children through posix_spawn's file actions),
    and the C++ one throws through a destructor and catches by base class,
    which needs the unwinder and RTTI libstdc++ carries.

It also MEASURES, and prints rather than asserts (a number that moves
with the host must not gate): each compile's wall time, and the largest
`cc1`/`cc1plus` the guest's `ps` saw during the C++ compile. Speed is
only meaningful with --kvm (CLAUDE.md: TCG says nothing about speed).

  python3 tools/tests/sweep/native_cc_test.py [--instance N] [--kvm] [--mem MIB]
  python3 tools/tests/sweep/native_cc_test.py --positive-control
      # every guest compile gets -nostartfiles (no crt0.o): must FAIL

A SKIP (exit 77) unless this checkout built the native toolchain and seeded
it: `make native-toolchain` then `make iso`. Boots its own guest on a COPY
of disk.img.
"""

RUNNER = {"covers": "GCC on the guest compiles C and C++ that run and match the host's output",
          "needs": ["native_cc", "no native toolchain on the image -- "
                    "`make native-toolchain`, then `make iso`"]}

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, copy_disk, poll  # noqa: E402

REPO = toolpath.REPO
GDIR = "/tmp/nc"

HELLO_C = r"""
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct point { int x, y; double w; char tag[8]; };
static struct point scale(struct point p, int k) { p.x *= k; p.y *= k; p.w *= k; return p; }
static long sum(int n, ...) {
    va_list ap; long s = 0;
    va_start(ap, n);
    while (n--) s += va_arg(ap, long);
    va_end(ap);
    return s;
}
static jmp_buf jb;
static void deep(int n) { if (n == 0) longjmp(jb, 42); deep(n - 1); }
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    printf("c: hello from cc on SaunaOS\n");
    int v[64];
    for (int i = 0; i < 64; i++) v[i] = (i * 37) % 64;
    qsort(v, 64, sizeof v[0], cmp);
    printf("c: qsort %d %d %d\n", v[0], v[31], v[63]);
    struct point q = scale((struct point){ 3, -4, 0.25, "pt" }, 5);
    printf("c: struct %d %d %.3f %s\n", q.x, q.y, q.w, q.tag);
    printf("c: varargs %ld\n", sum(5, 1L, 2L, 3L, 40000000000L, -5L));
    if (setjmp(jb) == 0) deep(50); else printf("c: longjmp ok\n");
    printf("c: float %.6f %g %.2e\n", 1.0 / 3.0, 0.125, 6.02e23);
    char *s = malloc(100);
    snprintf(s, 100, "%s-%05d-%x", "fmt", 42, 0xbeef);
    printf("c: %s %zu\n", s, strlen(s));
    free(s);
    printf("c: done\n");
    return 0;
}
"""

# Two translation units, compiled apart and linked by a third call.
TWO_H = "int mix(int a, int b);\nextern const char *greeting;\n"
TWO_MAIN = r"""
#include <stdio.h>
#include "two.h"
int main(void) {
    int t = 0;
    for (int i = 0; i < 10; i++) t += mix(i, 10 - i);
    printf("two: %s %d\n", greeting, t);
    printf("two: done\n");
    return 0;
}
"""
TWO_UTIL = r"""
#include "two.h"
const char *greeting = "linked across units";
int mix(int a, int b) { return a * 3 + b * 7 - (a ^ b); }
"""

HELLO_CPP = r"""
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct Base : std::runtime_error { using std::runtime_error::runtime_error; };
struct Derived : Base { using Base::Base; };
struct Guard {
    const char *n;
    ~Guard() { std::cout << "cpp: unwound " << n << "\n"; }
};
struct Shape { virtual ~Shape() = default; virtual int area() const = 0; };
struct Rect : Shape { int w, h; Rect(int a, int b) : w(a), h(b) {} int area() const override { return w * h; } };

static void thrower(int depth) {
    Guard g{"a frame"};
    if (depth == 0) throw Derived("from the bottom");
    thrower(depth - 1);
}

int main() {
    std::cout << "cpp: hello from c++ on SaunaOS" << std::endl;
    std::vector<std::unique_ptr<Shape>> v;
    for (int i = 1; i <= 3; i++) v.push_back(std::make_unique<Rect>(i, i + 1));
    int total = 0;
    for (auto &s : v) total += s->area();
    std::cout << "cpp: areas " << total << "\n";
    std::map<std::string, int> m{{"one", 1}, {"two", 2}, {"three", 3}};
    for (auto &[k, val] : m) std::cout << "cpp: map " << k << "=" << val << "\n";
    try {
        thrower(0);
    } catch (const Base &e) {
        std::cout << "cpp: caught " << e.what() << " as Base\n";
    }
    std::cout << "cpp: done" << std::endl;
    return 0;
}
"""

# The guest's own stopwatch: the monotonic clock in microseconds, read
# before and after each compile by the script that runs it, so a figure
# does not include the host's polling. Compiled ON the guest, first.
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

# (name, guest compile commands, the binary, the prefix of its lines)
CASES = [
    ("hello.c", [f"cc -O2 {GDIR}/hello.c -o {GDIR}/hello_c"], f"{GDIR}/hello_c", "c:"),
    ("two", [f"cc -O2 -c {GDIR}/two_main.c -o {GDIR}/two_main.o",
             f"cc -O2 -c {GDIR}/two_util.c -o {GDIR}/two_util.o",
             f"cc {GDIR}/two_main.o {GDIR}/two_util.o -o {GDIR}/two"], f"{GDIR}/two", "two:"),
    ("hello.cpp", [f"c++ -O2 {GDIR}/hello.cpp -o {GDIR}/hello_cpp"], f"{GDIR}/hello_cpp", "cpp:"),
    # -pipe: cc1's output goes straight into as through a pipe, so the
    # driver's children are started with file actions (posix_spawn's
    # fork path) rather than one plain spawn each.
    ("pipe", [f"cc -pipe -O2 {GDIR}/hello.c -o {GDIR}/hello_pipe"], f"{GDIR}/hello_pipe", "c:"),
    # A static library: ar writes it (binutils asks F_GETFL whether its
    # temporary file is writable, and refused every archive while the
    # answer was O_RDONLY), and ld takes the member from it.
    ("archive", [f"cc -O2 -c {GDIR}/two_util.c -o {GDIR}/two_util.o",
                 f"ar rcs {GDIR}/libtwo.a {GDIR}/two_util.o",
                 f"cc -O2 {GDIR}/two_main.c {GDIR}/libtwo.a -o {GDIR}/two_ar"], f"{GDIR}/two_ar", "two:"),
]


def host_lines(tmp):
    """The oracle: each case built by the host's own compilers and run."""
    want = {}
    b = os.path.join(tmp, "host")
    os.makedirs(b, exist_ok=True)
    subprocess.run(["gcc", "-O2", os.path.join(tmp, "hello.c"), "-o", f"{b}/c"], check=True)
    subprocess.run(["gcc", "-O2", "-I", tmp, os.path.join(tmp, "two_main.c"),
                    os.path.join(tmp, "two_util.c"), "-o", f"{b}/two"], check=True)
    subprocess.run(["g++", "-O2", os.path.join(tmp, "hello.cpp"), "-o", f"{b}/cpp"], check=True)
    for name, exe, pre in (("hello.c", "c", "c:"), ("two", "two", "two:"), ("hello.cpp", "cpp", "cpp:")):
        out = subprocess.run([f"{b}/{exe}"], capture_output=True, text=True).stdout
        want[name] = [ln.rstrip() for ln in out.splitlines() if ln.startswith(pre)]
    want["pipe"] = want["hello.c"]
    want["archive"] = want["two"]
    return want


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--instance", type=int, default=6, metavar="N",
                    help="the VM slot (vm.py's numbering; default 6)")
    ap.add_argument("--disk", default="disk.img", help="the image to boot a COPY of")
    ap.add_argument("--kvm", action="store_true", help="time it on KVM (the only speed worth quoting)")
    ap.add_argument("--mem", type=int, default=0, metavar="MIB",
                    help="guest RAM (default: vm.py's); a C++ compile needs more than a small guest has")
    ap.add_argument("--timeout", type=int, default=1200,
                    help="seconds one compile may take (TCG is slow: default 1200)")
    ap.add_argument("--positive-control", action="store_true",
                    help="compile every case with -nostartfiles on the guest: must fail")
    args = ap.parse_args()

    if not os.access(os.path.join(REPO, "seed", "sync", "usr", "bin", "cc"), os.X_OK):
        print("native_cc_test: SKIP -- the seed tree has no /usr/bin/cc; "
              "`make native-toolchain`, then `make iso`")
        return 77
    tmp = tempfile.mkdtemp(prefix="native_cc-")
    try:
        return run(args, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(args, tmp):
    res = Results()
    files = {"hello.c": HELLO_C, "two.h": TWO_H, "two_main.c": TWO_MAIN,
             "two_util.c": TWO_UTIL, "hello.cpp": HELLO_CPP, "tick.c": TICK_C}
    for n, text in files.items():
        with open(os.path.join(tmp, n), "w") as f:
            f.write(text)
    want = host_lines(tmp)
    extra = " -nostartfiles" if args.positive_control else ""
    if extra:
        print("native_cc_test: POSITIVE CONTROL -- every guest compile drops crt0.o; must FAIL")

    disk = copy_disk(args.disk, os.path.join(tmp, "disk.img"), cwd=REPO)

    def vm(*argv, timeout=180):
        r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
                            "--disk", disk, *(["--kvm"] if args.kvm else []),
                            *(["--mem", str(args.mem)] if args.mem else []), *argv],
                           cwd=REPO, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start", timeout=300)):
        return res.finish("native_cc_test")
    try:
        res.check("the image carries /usr/bin/cc and /usr/bin/c++",
                  "cc" in vm("exec", "ls /usr/bin") and "c++" in vm("exec", "ls /usr/bin"))
        # The aliases are HARD LINKS of the drivers (the staging tree
        # links them and tfs3_writer.py's sync seeds a link as one): one
        # inode, two links -- not two 2.4 MB copies.
        for alias, real in (("/usr/bin/cc", "/usr/bin/gcc"), ("/usr/bin/c++", "/usr/bin/g++"),
                            ("/bin/sh", "/bin/dash")):
            sa, sr = vm("exec", f"stat {alias}"), vm("exec", f"stat {real}")
            ia = re.search(r"inode:\s+(\d+)", sa)
            ir = re.search(r"inode:\s+(\d+)", sr)
            res.check(f"{alias} is a hard link of {real}: one inode, two links",
                      ia and ir and ia.group(1) == ir.group(1) and re.search(r"links:\s+2\b", sa),
                      f"{alias}: {sa.strip()[:160]!r}; {real}: {sr.strip()[:160]!r}")
        vm("exec", f"mkdir {GDIR}")
        for n in files:
            out = vm("put", os.path.join(tmp, n), f"{GDIR}/{n}")
            res.check(f"{n} is copied in", "fail" not in out.lower(), out.strip()[:200])

        # The stopwatch, built with no flags of the test's -- so the
        # positive control's break reaches only the cases.
        with open(os.path.join(tmp, "tick.sh"), "w") as f:
            f.write(f"export PATH=/bin:/usr/bin\ncd {GDIR}\n"
                    f"cc -O2 tick.c -o tick > tick.log 2>&1\necho $? > tick.rc\n")
        vm("put", os.path.join(tmp, "tick.sh"), f"{GDIR}/tick.sh")
        vm("exec", f"spawn /bin/dash {GDIR}/tick.sh")
        have_tick = poll(lambda: "0" in vm("exec", f"cat {GDIR}/tick.rc").split(), args.timeout)
        if not have_tick:
            print("native_cc_test: no guest stopwatch -- times below are the host's, polling included")

        for name, cmds, exe, pre in CASES:
            # A dash script, for `2>&1`: a compiler's diagnostics are on
            # stderr, and a failure with no message is not a report. It
            # sets the PATH init gives every session: a program the debug
            # console spawns has NO environment, and the driver finds its
            # own prefix (so cc1) by looking argv[0] up on PATH.
            log = f"{GDIR}/{name}.log"
            marker = f"{GDIR}/{name}.rc"
            script = os.path.join(tmp, f"{name}.sh")
            with open(script, "w") as f:
                f.write(f"export PATH=/bin:/usr/bin\ncd {GDIR}\n./tick > {name}.t0\n{{ " +
                        " && ".join(f"{c}{extra}" for c in cmds) +
                        f"; }} > {log} 2>&1\nrc=$?\n./tick > {name}.t1\necho $rc > {marker}\n")
            vm("put", script, f"{GDIR}/{name}.sh")
            vm("exec", f"rm {marker}")
            t0 = time.time()
            peak = {"kb": 0, "who": ""}
            stop = threading.Event()

            def sample():
                # The largest compiler process the guest's ps reports,
                # sampled while it runs -- a lower bound on its peak.
                while not stop.is_set():
                    for line in vm("exec", "ps", timeout=60).splitlines():
                        f = line.split()
                        if len(f) >= 7 and f[-1] in ("cc1", "cc1plus", "as", "ld", "collect2"):
                            try:
                                kb = int(f[-2])
                            except ValueError:
                                continue
                            if kb > peak["kb"]:
                                peak.update(kb=kb, who=f[-1])
                    stop.wait(1.0)

            th = threading.Thread(target=sample, daemon=True)
            th.start()
            vm("exec", f"spawn /bin/dash {GDIR}/{name}.sh")
            rc = poll(lambda: (lambda o: o.strip().splitlines()[-1]
                               if re.search(r"^\d+\s*$", o, re.M) else None)(
                               vm("exec", f"cat {marker}")), args.timeout)
            stop.set()
            th.join(timeout=90)
            secs = time.time() - t0
            try:
                us = [int(vm("exec", f"cat {GDIR}/{name}.t{i}").split()[-1]) for i in (0, 1)]
                secs, clock = (us[1] - us[0]) / 1e6, "guest clock"
            except (ValueError, IndexError):
                clock = "host clock, polling included"
            print(f"native_cc_test: {name}: compiled in {secs:.2f} s by the {clock} "
                  f"({'KVM' if args.kvm else 'TCG -- not a speed figure'}); largest "
                  f"{peak['who'] or 'compiler process'} seen {peak['kb']} KB")
            ok_rc = rc is not None and rc.strip().split()[-1] == "0"
            if not ok_rc:
                # What the kernel said about it: an exec that could not
                # build an image logs why (sched_fork.c's exec_nomem()).
                klog = [ln for ln in vm("exec", "dmesg").splitlines()
                        if "exec:" in ln or "spawn:" in ln or "oom" in ln.lower()]
            if not res.check(f"{name}: `{' && '.join(cmds)}` succeeds on the guest", ok_rc,
                             f"exit {rc!r}; log:\n" + vm("exec", f"cat {log}")[-1500:] +
                             ("\nkernel log:\n" + "\n".join(klog[-8:]) if not ok_rc and klog else "")):
                continue
            outf = f"{GDIR}/{name}.out"
            vm("exec", f"spawn /bin/tosh -c {exe} > {outf}")
            end = f"{pre} done"

            def read():
                return [ln.rstrip() for ln in vm("exec", f"cat {outf}").splitlines()
                        if ln.startswith(pre)]
            got = poll(lambda: (lambda ls: ls if end in ls else None)(read()), 60) or read()
            pad = max(len(want[name]), len(got))
            diff = [f"    host: {w!r}\n    test: {g!r}"
                    for w, g in zip(want[name] + [""] * pad, got + [""] * pad) if w != g]
            res.check(f"{name}: its output matches the host build's, line for line "
                      f"({len(want[name])} lines)", got == want[name], "\n".join(diff)[:1500])
    finally:
        vm("stop")
    return res.finish("native_cc_test")


if __name__ == "__main__":
    sys.exit(main())
