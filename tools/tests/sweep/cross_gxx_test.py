#!/usr/bin/env python3
"""The host's x86_64-saunaos-g++ builds a C++ program that runs on SaunaOS.

docs/cc-design.md, stage 2's exit criterion: a C++ program cross-built with NO
flags runs on a guest, and its output matches the host g++'s build of the same
source LINE FOR LINE -- a global whose constructor prints (and destructor
prints at exit), exceptions thrown across two functions and across two
translation units, std::string, std::vector, std::map, std::unique_ptr, a
virtual call, iostream, and std::thread with a mutex. And every C library
header in the sysroot compiles as C++ on its own.

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - "it links" passes a program whose constructors never run (the stage-1
    runtime): the global's constructor line is the FIRST line compared, and its
    destructor's -- printed after main() has returned -- is the last;
  - "it prints" passes a program that terminates at the first throw (no unwind
    tables, or none registered): every throw is caught and printed, one in the
    other translation unit, and the run must reach the destructor's line;
  - "the line count matches" passes reordered output: the comparison is line for
    line, in order.

  python3 tools/tests/sweep/cross_gxx_test.py [--instance N] [--toolchain DIR]
  python3 tools/tests/sweep/cross_gxx_test.py --positive-control eh_frame
      # links with a copy of saunaos.ld that discards .eh_frame: must FAIL
  python3 tools/tests/sweep/cross_gxx_test.py --positive-control init_array
      # ... that keeps no .init_array: must FAIL

A SKIP (exit 77) without a toolchain, or one built before C++ (stage 1):
`make toolchain EXTRAS=1`. Boots its own guest, on a COPY of disk.img.
"""

RUNNER = {"covers": "x86_64-saunaos-g++'s default build runs C++ on a guest and matches the host's output",
          "needs": ["cross_gcc", "no x86_64-saunaos toolchain -- `make toolchain EXTRAS=1`"]}

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
TARGET = "x86_64-saunaos"
GUEST_BIN = "/tmp/cxx_hello"
GUEST_OUT = "/tmp/cxx_hello.out"
END = "cxx: global destroyed"

SHARED_H = r"""
#include <stdexcept>
#include <string>
struct tu_error : std::runtime_error {
    int code;
    tu_error(const std::string &what, int c) : std::runtime_error(what), code(c) {}
};
void tu_throw(int code);          // thrower.cpp
int tu_rethrow(int code);         // thrower.cpp: catches, adds, rethrows
"""

THROWER = r"""
#include "shared.h"
void tu_throw(int code) {
    if (code > 0) throw tu_error("from the other unit", code);
}
int tu_rethrow(int code) {
    try {
        tu_throw(code);
    } catch (tu_error &e) {
        e.code += 1000;
        throw;
    }
    return 0;
}
"""

MAIN = r"""
#include "shared.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <typeinfo>
#include <vector>

struct Global {
    std::string name;
    Global() : name("the global") { std::cout << "cxx: global constructed: " << name << std::endl; }
    ~Global() { std::cout << "cxx: global destroyed" << std::endl; }
};
static Global g_global;

struct Shape {
    virtual ~Shape() {}
    virtual double area() const = 0;
    virtual const char *kind() const { return "shape"; }
};
struct Rect : Shape {
    double w, h;
    Rect(double w_, double h_) : w(w_), h(h_) {}
    double area() const override { return w * h; }
    const char *kind() const override { return "rect"; }
};
struct Square : Rect {
    explicit Square(double s) : Rect(s, s) {}
    const char *kind() const override { return "square"; }
};

struct Noisy {
    int id;
    explicit Noisy(int i) : id(i) { std::cout << "cxx: noisy " << id << " made" << std::endl; }
    ~Noisy() { std::cout << "cxx: noisy " << id << " freed" << std::endl; }
};

static int depth2(int n) {
    Noisy guard(n);           // destroyed by the unwind
    if (n == 2) throw std::runtime_error("two levels down");
    return n;
}
static int depth1(int n) { return depth2(n) + 1; }

int main() {
    std::cout << "cxx: main starts" << std::endl;

    // A throw across two functions, unwinding a destructor on the way.
    try {
        depth1(2);
        std::cout << "cxx: no throw?" << std::endl;
    } catch (const std::exception &e) {
        std::cout << "cxx: caught: " << e.what() << std::endl;
    }

    // Across translation units, by base class, and rethrown.
    try {
        tu_throw(7);
    } catch (const std::runtime_error &e) {
        const tu_error *te = dynamic_cast<const tu_error *>(&e);
        std::cout << "cxx: caught across units: " << e.what() << " code "
                  << (te ? te->code : -1) << std::endl;
    }
    try {
        tu_rethrow(5);
    } catch (tu_error &e) {
        std::cout << "cxx: rethrown: " << e.code << std::endl;
    }
    try {
        throw 42;
    } catch (...) {
        std::cout << "cxx: catch-all" << std::endl;
    }

    // std::string
    std::string s = "Sauna";
    s += "OS";
    s.insert(0, "hello ");
    std::cout << "cxx: string: " << s << " (" << s.size() << ") find " << s.find("OS")
              << " upper " << (char)toupper(s[6]) << std::endl;

    // std::vector + algorithm + a lambda
    std::vector<int> v = {5, 3, 9, 1, 7};
    for (int i = 0; i < 1000; i++) v.push_back(i % 13);
    std::sort(v.begin(), v.end(), [](int a, int b) { return a > b; });
    long sum = 0;
    for (int x : v) sum += x;
    std::cout << "cxx: vector: " << v.size() << " first " << v.front() << " last "
              << v.back() << " sum " << sum << std::endl;

    // std::map, in key order
    std::map<std::string, int> m;
    for (const char *w : {"pear", "apple", "fig", "apple", "kiwi", "fig", "apple"}) m[w]++;
    std::ostringstream os;
    for (const auto &kv : m) os << kv.first << "=" << kv.second << " ";
    std::cout << "cxx: map: " << os.str() << std::endl;

    // std::unique_ptr, and a virtual call through the base
    {
        std::vector<std::unique_ptr<Shape>> shapes;
        shapes.push_back(std::make_unique<Rect>(2.0, 3.5));
        shapes.push_back(std::make_unique<Square>(4.0));
        for (const auto &sh : shapes)
            std::cout << "cxx: virtual: " << sh->kind() << " area " << sh->area() << std::endl;
        std::unique_ptr<Noisy> owned(new Noisy(9));
        std::unique_ptr<Noisy> moved = std::move(owned);
        std::cout << "cxx: unique_ptr: moved " << (owned ? "kept" : "empty") << " -> "
                  << moved->id << std::endl;
    }
    std::cout << "cxx: typeid: " << typeid(Square).name() << std::endl;

    // iostream formatting
    std::cout << "cxx: format: " << std::setw(6) << 42 << "|" << std::hex << 255 << std::dec
              << "|" << std::setprecision(4) << 3.14159265 << "|" << std::boolalpha << true
              << std::endl;

    // std::thread, std::mutex, std::atomic
    {
        std::mutex mu;
        long counter = 0;
        std::atomic<int> started{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; t++)
            ts.emplace_back([&] {
                started++;
                for (int i = 0; i < 2500; i++) {
                    std::lock_guard<std::mutex> lk(mu);
                    counter++;
                }
            });
        for (auto &t : ts) t.join();
        std::cout << "cxx: threads: " << started.load() << " ran, counter " << counter << std::endl;
    }

    std::printf("cxx: printf still works: %d\n", 17);
    std::fflush(stdout);
    std::cout << "cxx: main returns" << std::endl;
    return 0;
}
"""

# The two breaks the control can make, as edits to a copy of saunaos.ld.
BREAKS = {
    "eh_frame": ("    .eh_frame : {\n",
                 "    /DISCARD/ : { *(.eh_frame) }\n    .eh_frame : {\n"),
    "init_array": ("        KEEP(*(.init_array))\n        PROVIDE_HIDDEN(__init_array_end = .);",
                   "        PROVIDE_HIDDEN(__init_array_end = .);"),
}


def clean_lines(text):
    return [ln.rstrip() for ln in text.splitlines() if ln.startswith("cxx:")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--instance", type=int, default=6, metavar="N",
                    help="the VM slot (vm.py's numbering; default 6)")
    ap.add_argument("--toolchain", default=os.environ.get("TOOLCHAIN", "toolchain/local"),
                    help="the prefix holding bin/x86_64-saunaos-g++ (default: $TOOLCHAIN "
                         "or toolchain/local)")
    ap.add_argument("--disk", default="disk.img", help="the image to boot a COPY of")
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--positive-control", choices=sorted(BREAKS),
                    help="link with a saunaos.ld that loses this section: must fail")
    args = ap.parse_args()

    prefix = os.path.join(REPO, args.toolchain)
    gxx = os.path.join(prefix, "bin", f"{TARGET}-g++")
    if not os.access(gxx, os.X_OK):
        print(f"cross_gxx_test: SKIP -- no {gxx}; `make toolchain EXTRAS=1` builds it")
        return 77
    tmp = tempfile.mkdtemp(prefix="cross_gxx-")
    try:
        return run(args, gxx, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(args, gxx, tmp):
    res = Results()
    for name, text in (("shared.h", SHARED_H), ("thrower.cpp", THROWER), ("main.cpp", MAIN)):
        with open(os.path.join(tmp, name), "w") as f:
            f.write(text)
    srcs = [os.path.join(tmp, "main.cpp"), os.path.join(tmp, "thrower.cpp")]
    sysroot = os.path.join(REPO, "build", "sysroot")
    extra = []
    if os.path.realpath(os.path.join(os.path.dirname(gxx), "..")) != os.path.realpath(
            os.path.join(REPO, "toolchain", "local")):
        extra = [f"--sysroot={sysroot}"]
    broken = []
    if args.positive_control:
        old, new = BREAKS[args.positive_control]
        with open(os.path.join(sysroot, "usr/lib/saunaos.ld")) as f:
            script = f.read()
        if script.count(old) != 1:
            print(f"cross_gxx_test: the control's edit no longer matches saunaos.ld: {old!r}")
            return 2
        bad = os.path.join(tmp, "broken.ld")
        with open(bad, "w") as f:
            f.write(script.replace(old, new))
        broken = ["-T", bad]
        print(f"cross_gxx_test: POSITIVE CONTROL -- {args.positive_control} lost from the "
              "link; must FAIL")

    # 1. The thread model the libraries were built for.
    v = subprocess.run([gxx, *extra, "-v"], capture_output=True, text=True)
    res.check("g++ reports the POSIX thread model", "Thread model: posix" in v.stderr,
              v.stderr.strip()[-200:])

    # 2. Every C library header compiles as C++ on its own: extern "C"
    # around it, no C-only keyword, no C++ keyword as a name. The toolkit's
    # headers (base/ net/ media/ ui/) are not C++-clean yet, and are left out.
    inc = os.path.join(sysroot, "usr/include")
    bad = []
    headers = sorted(os.path.relpath(os.path.join(d, f), inc)
                     for d, dirs, files in os.walk(inc)
                     if os.path.relpath(d, inc).split(os.sep)[0] not in
                     ("base", "net", "media", "ui", "saunaos")
                     for f in files if f.endswith(".h"))
    for h in headers:
        r = subprocess.run([gxx, *extra, "-std=c++17", "-Wall", "-fsyntax-only", "-x", "c++", "-"],
                           input=f"#include <{h}>\nint main() {{ return 0; }}\n",
                           capture_output=True, text=True)
        if r.returncode or r.stderr.strip():
            bad.append(f"{h}: {r.stderr.strip().splitlines()[0] if r.stderr.strip() else r.returncode}")
    res.check(f"every C library header compiles as C++ ({len(headers)})",
              bool(headers) and not bad, "\n    ".join(bad)[:1200])

    # 3. The build, with no flags but the language level.
    exe = os.path.join(tmp, "cxx_hello")
    cc = subprocess.run([gxx, *extra, *broken, "-std=c++17", "-O2", *srcs, "-o", exe],
                        capture_output=True, text=True)
    if not res.check("`x86_64-saunaos-g++ -O2 main.cpp thrower.cpp` links", cc.returncode == 0,
                     (cc.stdout + cc.stderr).strip()[:800]):
        return res.finish("cross_gxx_test")
    size = os.path.getsize(exe)
    strip = shutil.which(f"{TARGET}-strip", path=os.path.dirname(gxx)) or "strip"
    stripped = os.path.join(tmp, "cxx_hello.stripped")
    subprocess.run([strip, "-o", stripped, exe], check=False)
    print(f"cross_gxx_test: binary {size} bytes, {os.path.getsize(stripped) if os.path.exists(stripped) else 0} stripped")
    sections = subprocess.run(["readelf", "-SW", exe], capture_output=True, text=True).stdout
    res.check("the binary keeps .eh_frame and .init_array",
              bool(re.search(r"\.eh_frame\s", sections)) and ".init_array" in sections
              or bool(args.positive_control))

    # 4. The oracle: the same source, the host's plain g++, run here.
    host_exe = os.path.join(tmp, "cxx_hello-host")
    subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", *srcs, "-o", host_exe], check=True)
    want = clean_lines(subprocess.run([host_exe], capture_output=True, text=True).stdout)
    res.check(f"the host build ends at the destructor's line ({len(want)} lines)",
              bool(want) and want[-1] == END)

    # 5. On the guest.
    disk = copy_disk(args.disk, os.path.join(tmp, "disk.img"), cwd=REPO)

    def vm(*argv, timeout=180):
        r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
                            "--disk", disk, *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start", timeout=300)):
        return res.finish("cross_gxx_test")
    try:
        out = vm("put", exe, GUEST_BIN)
        res.check("the binary is copied in", "fail" not in out.lower(), out.strip()[:300])
        vm("exec", f"rm {GUEST_OUT}")
        vm("exec", f"spawn /bin/tosh -c {GUEST_BIN} > {GUEST_OUT}")

        def read():
            return clean_lines(vm("exec", f"cat {GUEST_OUT}"))

        got = poll(lambda: (lambda lines: lines if END in lines else None)(read()), 60) or read()
        res.check("it runs on the guest to the global's destructor, after main", END in got,
                  f"got {len(got)} lines")
        pad = max(len(want), len(got))
        diff = [f"    host: {w!r}\n    test: {g!r}"
                for w, g in zip(want + [""] * pad, got + [""] * pad) if w != g]
        res.check(f"its output matches the host build's, line for line ({len(want)} lines)",
                  got == want, "\n".join(diff)[:2000])
    finally:
        vm("stop")
    return res.finish("cross_gxx_test")


if __name__ == "__main__":
    sys.exit(main())
