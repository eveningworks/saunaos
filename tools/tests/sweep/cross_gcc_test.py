#!/usr/bin/env python3
"""The host's x86_64-saunaos-gcc builds a program that runs on SaunaOS.

docs/cc-design.md, stage 1's exit criterion: `x86_64-saunaos-gcc hello.c -o
hello`, with NO flags, gives a binary that `spawn`s and prints on a guest.
The flags the tree passes by hand (Makefile's USERLAND_CFLAGS and its link
lines) are the cross compiler's DEFAULTS now -- gcc/config/saunaos.h in
toolchain/patches/gcc/ -- so this compiles with none and checks they arrived.
The default is a DYNAMIC program, as /bin's are; -static is the static one.
Both are built and both run. Then SDL, through the SDK: an SDL3 program built
with `pkg-config sdl3` and an SDL2 one with `sdl2-config`, each run on the
guest under SDL's dummy video driver.

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - "it produced a binary" passes a link with no crt0 (the entry falls back
    to whatever is first in .text): the ELF must be ET_EXEC, entry == _start,
    loaded at the 0x8000000000 base the scripts name -- the default with
    PT_INTERP /lib/ld-toy.so and libc.so NEEDED, -static with no PT_INTERP;
  - "it ran" passes a program that printed half its lines and faulted: the
    guest's output is compared LINE FOR LINE with the same source built by
    the host's plain gcc and run here -- printf, malloc, a struct passed by
    value, varargs, setjmp/longjmp, floating point, errno;
  - "the defaults are right" passes a spec that dropped the global stack
    guard and still links (libc.a has both symbols): a protected function's
    assembly must read __stack_chk_guard, not %fs:40, and __saunaos__ must
    be predefined where __linux__ is not.

  python3 tools/tests/sweep/cross_gcc_test.py [--instance N] [--toolchain DIR]
  python3 tools/tests/sweep/cross_gcc_test.py --positive-control
      # the same run with a specs file that drops crt0.o from the link:
      # must FAIL (the ELF checks and the guest's output both)

A SKIP (exit 77) without a toolchain: `make toolchain EXTRAS=1` builds one.
Boots its own guest, on a COPY of disk.img (`make iso` first).
"""

RUNNER = {"covers": "x86_64-saunaos-gcc's default build runs on a guest and matches the host's output",
          "needs": ["cross_gcc", "no x86_64-saunaos toolchain -- `make toolchain EXTRAS=1`"]}

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, copy_disk, poll  # noqa: E402

REPO = toolpath.REPO
TARGET = "x86_64-saunaos"
BASE = 0x8000000000          # userland/libs/rt/link.ld, VMM_USER_BASE
GUEST_BIN = "/tmp/cc_hello"
GUEST_OUT = "/tmp/cc_hello.out"
END = "cc_hello: done"

# No `0` flag on a float conversion: tolibc ignores it (docs/bugs.md), and
# this test is about the compiler, not that.
PROGRAM = r"""
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct point { int x, y; double w; char tag[8]; };

static struct point scale(struct point p, int k) {
    p.x *= k; p.y *= k; p.w *= k;
    return p;
}

static long sum(int n, ...) {
    va_list ap;
    long s = 0;
    va_start(ap, n);
    while (n--) s += va_arg(ap, long);
    va_end(ap);
    return s;
}

static jmp_buf jb;
static void deep(int n) {
    if (n == 0) longjmp(jb, 42);
    deep(n - 1);
}

int main(void) {
    printf("cc_hello: hello from x86_64-saunaos-gcc\n");
    int *v = malloc(1000 * sizeof *v);
    if (!v) { printf("malloc failed\n"); return 1; }
    long t = 0;
    for (int i = 0; i < 1000; i++) v[i] = i * 3;
    for (int i = 0; i < 1000; i++) t += v[i];
    printf("malloc: sum %ld\n", t);
    v = realloc(v, 4000 * sizeof *v);
    printf("realloc: %s, kept %d\n", v ? "ok" : "failed", v ? v[999] : -1);
    free(v);
    struct point p = { 3, -4, 0.25, "pt" };
    struct point q = scale(p, 5);
    printf("struct: %d %d %.3f %s\n", q.x, q.y, q.w, q.tag);
    printf("varargs: %ld\n", sum(5, 1L, 2L, 3L, 40000000000L, -5L));
    int r = setjmp(jb);
    if (r == 0) { deep(50); printf("longjmp: did not jump\n"); }
    else printf("longjmp: %d\n", r);
    printf("float: %.6f %.3f %g %.2e\n", 1.0 / 3.0, 2.5 * 1.5, 0.125, 6.02e23);
    char buf[32];
    snprintf(buf, sizeof buf, "%08.3f|%-5d|%x", 3.14159, 42, 0xbeef);
    printf("snprintf: %s (%zu)\n", buf, strlen(buf));
    errno = 0;
    long bad = strtol("99999999999999999999", NULL, 10);
    printf("errno: ERANGE %s, clamped %s\n", errno == ERANGE ? "set" : "unset",
           bad == 9223372036854775807L ? "yes" : "no");
    printf("%s\n", "cc_hello: done");
    return 0;
}
"""

# A protected function: -fstack-protector-strong guards a local array.
GUARDED = "void sink(char *); void f(void) { char b[64]; sink(b); }\n"

# The positive control's break: a specs override with no startfile, i.e.
# saunaos.h's STARTFILE_SPEC losing crt0.o.
BROKEN_SPECS = "*startfile:\n%{!shared:}\n\n"


# SDL through the SDK, under the dummy driver (no desktop is needed).
SDL3_PROGRAM = r"""
#include <SDL3/SDL.h>
#include <stdio.h>
int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("cc_sdl: sdl3 init failed %s\n", SDL_GetError()); return 1; }
    SDL_Window *w = SDL_CreateWindow("cc", 64, 48, 0);
    printf("cc_sdl: sdl3 %d.%d.%d %s %s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
           SDL_GetCurrentVideoDriver(), w ? "window" : SDL_GetError());
    SDL_Quit();
    return 0;
}
"""
SDL2_PROGRAM = r"""
#include "SDL.h"
#include <stdio.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("cc_sdl: sdl2 init failed %s\n", SDL_GetError()); return 1; }
    SDL_version v;
    SDL_GetVersion(&v);
    SDL_Window *w = SDL_CreateWindow("cc", 0, 0, 64, 48, 0);
    printf("cc_sdl: sdl2 %d.%d.%d %s %s\n", v.major, v.minor, v.patch,
           SDL_GetCurrentVideoDriver(), w ? "window" : SDL_GetError());
    SDL_Quit();
    return 0;
}
"""


def interp_and_needed(path):
    """(the PT_INTERP string or None, [DT_NEEDED...]), by readelf."""
    out = subprocess.run(["readelf", "-lWd", path], capture_output=True, text=True).stdout
    interp = None
    for line in out.splitlines():
        if "Requesting program interpreter:" in line:
            interp = line.split(":", 1)[1].strip().rstrip("]")
    needed = [ln.split("[", 1)[1].rstrip("]") for ln in out.splitlines() if "(NEEDED)" in ln]
    return interp, needed


def elf_facts(path):
    """(e_type, e_entry, [p_type...], {name: value} for _start/main)."""
    with open(path, "rb") as f:
        data = f.read()
    e_type, = struct.unpack_from("<H", data, 16)
    e_entry, e_phoff = struct.unpack_from("<QQ", data, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    phdrs = [struct.unpack_from("<IIQQQ", data, e_phoff + i * e_phentsize)
             for i in range(e_phnum)]
    nm = subprocess.run(["nm", path], capture_output=True, text=True).stdout
    syms = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("_start", "main"):
            syms[parts[2]] = int(parts[0], 16)
    return e_type, e_entry, phdrs, syms


def clean_lines(text):
    """The program's lines out of a `sh cat` reply, console chatter dropped."""
    return [ln.rstrip() for ln in text.splitlines()
            if ln.startswith(("cc_hello:", "malloc:", "realloc:", "struct:", "varargs:",
                              "longjmp:", "float:", "snprintf:", "errno:", "cc_sdl:"))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--instance", type=int, default=6, metavar="N",
                    help="the VM slot (vm.py's numbering; default 6)")
    ap.add_argument("--toolchain", default=os.environ.get("TOOLCHAIN", "toolchain/local"),
                    help="the prefix holding bin/x86_64-saunaos-gcc (default: $TOOLCHAIN "
                         "or toolchain/local)")
    ap.add_argument("--disk", default="disk.img", help="the image to boot a COPY of")
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="drop crt0.o from the link through a specs file: must fail")
    args = ap.parse_args()

    prefix = os.path.join(REPO, args.toolchain)
    gcc = os.path.join(prefix, "bin", f"{TARGET}-gcc")
    if not os.access(gcc, os.X_OK):
        print(f"cross_gcc_test: SKIP -- no {gcc}; `make toolchain EXTRAS=1` builds it")
        return 77
    tmp = tempfile.mkdtemp(prefix="cross_gcc-")
    try:
        return run(args, gcc, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(args, gcc, tmp):
    res = Results()
    src = os.path.join(tmp, "hello.c")
    with open(src, "w") as f:
        f.write(PROGRAM)
    # The default sysroot is the prefix's symlink to ITS checkout's
    # build/sysroot; a TOOLCHAIN borrowed from another checkout is pointed
    # at this one's SDK instead.
    extra = []
    if os.path.realpath(os.path.join(os.path.dirname(gcc), "..")) != os.path.realpath(
            os.path.join(REPO, "toolchain", "local")):
        extra = [f"--sysroot={os.path.join(REPO, 'build', 'sysroot')}"]
    # The break goes on the program's build only, so the control reaches the
    # ELF and guest checks rather than failing at the first compile.
    broken = []
    if args.positive_control:
        specs = os.path.join(tmp, "broken.specs")
        with open(specs, "w") as f:
            f.write(BROKEN_SPECS)
        broken = [f"-specs={specs}"]
        print("cross_gcc_test: POSITIVE CONTROL -- crt0.o dropped from the link; must FAIL")

    # 1. The predefines and the code-generation defaults.
    macros = subprocess.run([gcc, *extra, "-dM", "-E", "-x", "c", "/dev/null"],
                            capture_output=True, text=True).stdout
    res.check("__saunaos__ is predefined and __linux__ is not",
              "#define __saunaos__ 1" in macros and "__linux__" not in macros)
    asm = subprocess.run([gcc, *extra, "-O2", "-S", "-o", "-", "-x", "c", "-"],
                         input=GUARDED, capture_output=True, text=True)
    res.check("a protected function reads the GLOBAL guard (__stack_chk_guard, not %fs:40)",
              "__stack_chk_guard" in asm.stdout and "%fs:40" not in asm.stdout,
              asm.stderr.strip()[:300])
    res.check("it is position-independent (-fpie: the guard through the GOT or %rip)",
              "__stack_chk_guard@GOTPCREL(%rip)" in asm.stdout
              or "__stack_chk_guard(%rip)" in asm.stdout)

    # 2. The build, with no flags at all -- dynamic -- and with -static.
    exes = {}
    for kind, flags in (("dynamic", []), ("static", ["-static"])):
        exe = os.path.join(tmp, f"hello-{kind}")
        cc = subprocess.run([gcc, *extra, *broken, "-O2", *flags, src, "-o", exe],
                            capture_output=True, text=True)
        what = "-O2 hello.c" if not flags else "-O2 -static hello.c"
        if not res.check(f"`x86_64-saunaos-gcc {what} -o hello` links", cc.returncode == 0,
                         (cc.stdout + cc.stderr).strip()[:600]):
            continue
        exes[kind] = exe
        e_type, e_entry, phdrs, syms = elf_facts(exe)
        res.check(f"{kind}: ET_EXEC, as the kernel's loader requires", e_type == 2, f"e_type={e_type}")
        res.check(f"{kind}: the entry point is _start (crt0.o, linked first)",
                  "_start" in syms and e_entry == syms["_start"],
                  f"entry={e_entry:#x} _start={syms.get('_start', 0):#x}")
        loads = [p for p in phdrs if p[0] == 1]
        res.check(f"{kind}: loaded at 0x8000000000, the base the scripts name",
                  bool(loads) and min(p[3] for p in loads) == BASE,
                  f"lowest vaddr={min((p[3] for p in loads), default=0):#x}")
        interp, needed = interp_and_needed(exe)
        if kind == "dynamic":
            res.check("dynamic (the default): PT_INTERP /lib/ld-toy.so, libc.so NEEDED",
                      interp == "/lib/ld-toy.so" and "libc.so" in needed, f"{interp} {needed}")
        else:
            res.check("-static: no PT_INTERP, nothing NEEDED", interp is None and not needed,
                      f"{interp} {needed}")

    # 2b. SDL through the SDK: pkg-config for SDL3, sdl2-config for SDL2.
    sysroot = os.path.join(REPO, "build", "sysroot")
    env = dict(os.environ, PKG_CONFIG_LIBDIR=os.path.join(sysroot, "usr", "lib", "pkgconfig"))
    sdl = {}
    for name, prog, how in (("sdl3", SDL3_PROGRAM, ["pkg-config", "--cflags", "--libs", "sdl3"]),
                            ("sdl2", SDL2_PROGRAM, ["sh", os.path.join(sysroot, "usr", "bin", "sdl2-config"),
                                                    "--cflags", "--libs"])):
        c = os.path.join(tmp, f"{name}.c")
        with open(c, "w") as f:
            f.write(prog)
        flags = subprocess.run(how, capture_output=True, text=True, env=env).stdout.split()
        exe = os.path.join(tmp, name)
        cc = subprocess.run([gcc, *extra, c, "-o", exe, *flags], capture_output=True, text=True)
        tool = "pkg-config" if how[0] == "pkg-config" else "sdl2-config"
        if res.check(f"an {name.upper()} program links with {tool}'s flags",
                     cc.returncode == 0, f"flags {flags}: " + (cc.stdout + cc.stderr).strip()[:600]):
            sdl[name] = exe
            want_so = "libSDL3.so.0" if name == "sdl3" else "libSDL2-2.0.so.0"
            res.check(f"...needing {want_so} from /lib", want_so in interp_and_needed(exe)[1])

    # 3. The oracle: the same source, the host's plain gcc, run here.
    host_exe = os.path.join(tmp, "hello-host")
    subprocess.run(["gcc", "-O2", src, "-o", host_exe], check=True)
    want = clean_lines(subprocess.run([host_exe], capture_output=True, text=True).stdout)

    # 4. On the guest: put it, spawn it with stdout to a file, read the file.
    disk = copy_disk(args.disk, os.path.join(tmp, "disk.img"), cwd=REPO)

    def vm(*argv, timeout=180):
        r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
                            "--disk", disk, *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start", timeout=300)):
        return res.finish("cross_gcc_test")
    try:
        def run_on_guest(exe, end):
            out = vm("put", exe, GUEST_BIN)
            res.check(f"{os.path.basename(exe)} is copied in", "fail" not in out.lower(), out.strip()[:300])
            vm("exec", f"rm {GUEST_OUT}")
            vm("exec", f"spawn /bin/tosh -c {GUEST_BIN} > {GUEST_OUT}")

            def read():
                return clean_lines(vm("exec", f"cat {GUEST_OUT}"))
            # Wait on what is asserted: the program's LAST line in the file.
            return poll(lambda: (lambda lines: lines if any(end in ln for ln in lines) else None)(read()),
                        30) or read()

        for kind, exe in exes.items():
            got = run_on_guest(exe, END)
            res.check(f"{kind}: it spawns and prints on the guest, to the last line", END in got,
                      f"got {len(got)} lines")
            pad = max(len(want), len(got))
            diff = [f"    host: {w!r}\n    test: {g!r}"
                    for w, g in zip(want + [""] * pad, got + [""] * pad) if w != g]
            res.check(f"{kind}: its output matches the host build's, line for line ({len(want)} lines)",
                      got == want, "\n".join(diff)[:1500])
        for name, version in (("sdl3", "3."), ("sdl2", "2.")):
            if name not in sdl:
                continue
            got = run_on_guest(sdl[name], f"cc_sdl: {name}")
            line = next((ln for ln in got if ln.startswith(f"cc_sdl: {name}")), "")
            res.check(f"{name}: it runs on the guest, under SDL's dummy driver, with a window",
                      line.startswith(f"cc_sdl: {name} {version}") and line.endswith("dummy window"),
                      f"got {got}")
    finally:
        vm("stop")
    return res.finish("cross_gcc_test")


if __name__ == "__main__":
    sys.exit(main())
