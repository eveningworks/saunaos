#!/usr/bin/env python3
"""Check lib/uwinsetup.c -- Windows 3.x installed from its disks.

WHAT IS UNDER TEST. userland/lib/uwinsetup.c (with ufatimg.c and
umsexpand.c under it), the engine behind `w16 --install`, compiled with
the host gcc under AddressSanitizer and UBSan beside a small driver.

What it asserts:

  - SYNTHETIC DISKS from tools/gen_win3_disks.py, in all four forms (one
    folder, a folder per disk, floppy images, folders and images mixed):
    the installed tree is EXACTLY the generator's own table -- every file
    in its folder under its name, byte for byte, the decoys for other
    hardware and languages absent, no `.part` left
  - the INI files: kept when they are there and keeping was asked,
    replaced when it was not; the title in UTF-8
  - refusals: a disk set missing one file plans it as missing and the
    install stops naming it, leaving nothing half-written; a folder with
    no SETUP.INF is refused with a sentence
  - REAL DISKS, when ~/CodingProjects/toy-osother/win311 is there (local
    only -- Microsoft's files): from the English folder and from its eight
    floppy images, every installed file lands where Express Setup put it
    on the reference VM and matches libmspack's expansion byte for byte;
    the Finnish set plans with nothing missing
  - HOSTILE INPUT: mutated SETUP.INFs (long lines, fields, references to
    nowhere) and damaged compressed files, under ASan: each plans and
    installs or refuses, and nothing reads or writes out of bounds

POSITIVE CONTROL (--positive-control): two edits to a COPY of the source.
"0:system" sends a section to WINDOWS (the placement goes red), and a
field stops being bounded by its buffer (ASan goes red on a long field).

    python3 tools/uwinsetup_hostcheck.py [--positive-control] [--fuzz N]

Exit status is non-zero on any violation. Needs gcc with ASan/UBSan and mtools.
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_win3_disks  # noqa: E402
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined")
W311 = os.path.expanduser(os.environ.get("TOYOS_W311", "~/CodingProjects/toy-osother/win311"))
REF_IMG = os.path.join(os.path.dirname(W311), "win311-vm", "win311.img")

DRIVER = r"""
#include "lib/uwinsetup.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char err[256];
    if (argc < 3) return 2;
    struct uwinsetup *s = uwinsetup_open(argv[2], err, sizeof err);
    if (!s) { printf("error %s\n", err); return 0; }
    printf("title %s\nlang %s\nwindir %s\ncontainers %d\nmissing %d\n", uwinsetup_title(s),
           uwinsetup_lang(s), uwinsetup_windir(s), uwinsetup_containers(s), uwinsetup_missing(s));
    for (int i = 0; i < uwinsetup_count(s); i++) {
        const struct uws_item *it = uwinsetup_item(s, i);
        printf("item %s %s %s\n", it->dest ? "S" : "W", it->name, it->disk_name[0] ? it->disk_name : "-");
    }
    if (!strcmp(argv[1], "run") && argc == 5) {
        int r = uwinsetup_run(s, argv[3], !strcmp(argv[4], "keep") ? UWS_KEEP_INI : 0, 0, 0, err, sizeof err);
        printf("run %d %s\n", r, r ? err : "");
    }
    uwinsetup_close(s);
    return 0;
}
"""

CONTROL = [
    ('to = ends_with(f[k + 1], "system") ? UWS_SYSTEM : UWS_WINDOWS;',
     'to = ends_with(f[k + 1], "system") ? UWS_WINDOWS : UWS_SYSTEM;'),
    ("if (k < FIELD - 1) out[n][k++] = *c;", "if (k < FIELD * 4) out[n][k++] = *c;"),
]

failures = []


def check(name, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", name, ("   [%s]" % detail) if detail and not ok else ""))
    if not ok:
        failures.append(name)
    return ok


def run(exe, *args):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, *map(str, args)], capture_output=True, env=env, timeout=600)
    out = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0:
        lines = r.stderr.decode("latin-1").strip().splitlines()
        out += "\nDIED " + next((l for l in lines if "ERROR" in l or "runtime error" in l),
                                lines[-1] if lines else "exit %d" % r.returncode)
    return out


def tree(root):
    """{relative path: bytes} of every file under root."""
    got = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            got[os.path.relpath(p, root).replace(os.sep, "/")] = open(p, "rb").read()
    return got


def synthetic(exe, tmp):
    print("synthetic disks (tools/gen_win3_disks.py):")
    for form in ("flat", "disks", "images", "mixed"):
        src, c = os.path.join(tmp, "src-" + form), os.path.join(tmp, "C-" + form)
        want = gen_win3_disks.build(src, form)
        os.makedirs(c)
        out = run(exe, "run", src, c, "keep")
        got = tree(c)
        bad = sorted(set(want) ^ set(got)) + sorted(p for p in want if p in got and got[p] != want[p])
        check("%s: the installed tree is exactly the expected %d files" % (form, len(want)),
              "run 0" in out and not bad, (out.strip().splitlines() or ["?"])[-1] + " " + ", ".join(bad[:5]))
        if form == "flat":
            check("the title is UTF-8 and the language the INF's",
                  "title Microsoft Testi-Windows 3.11 Työryhmä" in out and "lang fin" in out, out[:160])

    src, c = os.path.join(tmp, "src-flat"), os.path.join(tmp, "C-keep")
    ini = os.path.join(c, "WINDOWS", "WIN.INI")
    os.makedirs(os.path.dirname(ini))
    with open(ini, "wb") as f:
        f.write(b"[mine]\r\n")
    run(exe, "run", src, c, "keep")
    check("an existing WIN.INI is kept when asked", open(ini, "rb").read() == b"[mine]\r\n")
    run(exe, "run", src, c, "replace")
    check("...and replaced from WIN.SRC when not", open(ini, "rb").read() == gen_win3_disks.content("WIN.SRC"))

    broken, c = os.path.join(tmp, "src-missing"), os.path.join(tmp, "C-missing")
    shutil.copytree(src, broken)
    os.remove(os.path.join(broken, "CALC.EX_"))
    os.makedirs(c)
    out = run(exe, "run", broken, c, "keep")
    check("a file on no disk is planned as missing", "missing 1" in out and "item W CALC.EXE -" in out, out[:200])
    check("...and the install stops naming it", "run -" in out and "CALC.EXE" in out.split("run -")[-1],
          out.strip().splitlines()[-1])
    check("...leaving no half-written file", not any(p.endswith(".part") for p in tree(c)))
    empty = os.path.join(tmp, "nothing")
    os.makedirs(empty)
    out = run(exe, "plan", empty)
    check("a folder with no SETUP.INF is refused with a sentence", "no SETUP.INF" in out, out.strip())


def reference_tree():
    """Express Setup's own C:\\WINDOWS on the reference VM: {NAME: 'W' or 'S'}."""
    if not os.path.exists(REF_IMG) or not shutil.which("mdir"):
        return None
    ref = {}
    for where, path in (("W", "::/WINDOWS"), ("S", "::/WINDOWS/SYSTEM")):
        out = subprocess.run(["mdir", "-b", "-i", REF_IMG + "@@32256", path], capture_output=True, text=True,
                             env=dict(os.environ, MTOOLS_SKIP_CHECK="1")).stdout
        for line in out.splitlines():
            name = line.strip().split("/")[-1].upper()
            if name and name != "SYSTEM":
                ref[name] = where
    return ref


def real_disks(exe, tmp):
    if not os.path.isdir(W311):
        print("real disks: SKIPPED -- %s is not here (Microsoft's files stay local)" % W311)
        return
    print("real disks (%s):" % W311)
    ref = reference_tree()
    expd = os.path.join(W311, "en", "expanded")
    exp = {f.rstrip(" .").upper(): f for f in os.listdir(expd)}
    renamed = {"WIN.INI": "WIN.SRC", "SYSTEM.INI": "SYSTEM.SRC", "CONTROL.INI": "CONTROL.SRC",
               "SOUND.DRV": "MMSOUND.DRV", "WINVER.EXE": "WINVER"}
    for label, src in (("English folder", os.path.join(W311, "en", "install")),
                       ("English floppy images", os.path.join(W311, "en", "disks"))):
        c = os.path.join(tmp, "real-" + label.replace(" ", "-"))
        os.makedirs(c)
        out = run(exe, "run", src, c, "keep")
        got = {p.split("/")[-1]: ("S" if p.startswith("WINDOWS/SYSTEM/") else "W", b) for p, b in tree(c).items()}
        wrong = [n for n, (d, _) in got.items() if ref is not None and ref.get(n) != d]
        bytes_bad = [n for n, (_, b) in got.items()
                     if open(os.path.join(expd, exp.get(renamed.get(n, n), "?")), "rb").read() != b] \
            if all(renamed.get(n, n) in exp for n in got) else ["(a file with no expanded twin)"]
        check("%s: %d files, each where Express Setup put it%s" %
              (label, len(got), "" if ref is not None else " (no reference VM here: placement not compared)"),
              "run 0" in out and not wrong and len(got) > 200, ", ".join(wrong[:6]) or out.strip()[-120:])
        check("%s: every file as libmspack expands it" % label, not bytes_bad, ", ".join(bytes_bad[:6]))
    fi = os.path.join(W311, "fi", "install")
    if os.path.isdir(fi):
        out = run(exe, "plan", fi)
        n = out.count("\nitem ")
        check("Finnish set: %d files planned, none missing" % n, "missing 0" in out and n > 200, out[:200])


def fuzz(exe, tmp, iters):
    print("hostile input (%d mutated SETUP.INFs, %d damaged files):" % (iters, iters // 4))
    rnd = random.Random(23)
    base = gen_win3_disks.INF.encode("latin-1")
    src = os.path.join(tmp, "src-flat")
    died = []
    for i in range(iters):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 12)):
            op = rnd.randrange(5)
            at = rnd.randrange(len(b))
            if op == 0:
                b[at] = rnd.randrange(256)
            elif op == 1:
                b[at:at] = bytes([rnd.choice(b",:#=\"[];")]) * rnd.randrange(1, 400)
            elif op == 2:
                b[at:at] = b"\n#" + bytes(rnd.choice(b"abcdefgh.")
                                         for _ in range(rnd.randrange(1, 60))) + b", 0:system\n"
            elif op == 3:
                del b[at:at + rnd.randrange(1, 200)]
            else:
                b[at:at] = b"x" * rnd.randrange(100, 3000)
        d = os.path.join(tmp, "fz")
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(src, d)
        with open(os.path.join(d, "SETUP.INF"), "wb") as f:
            f.write(bytes(b))
        out = run(exe, "plan", d)
        if "DIED" in out:
            died.append(out.split("DIED")[-1].strip())
            break
    check("every mutated SETUP.INF plans or refuses, nothing out of bounds", not died, "; ".join(died[:1]))
    died = []
    names = [n for n in os.listdir(src) if n.endswith("_")]
    for i in range(iters // 4):
        d, c = os.path.join(tmp, "fz"), os.path.join(tmp, "fzC")
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(c, ignore_errors=True)
        shutil.copytree(src, d)
        os.makedirs(c)
        victim = os.path.join(d, rnd.choice(names))
        b = bytearray(open(victim, "rb").read())
        if rnd.random() < 0.3:
            b = b[:rnd.randrange(len(b))]
        for _ in range(rnd.randrange(1, 10)):
            if b:
                b[rnd.randrange(len(b))] = rnd.randrange(256)
        with open(victim, "wb") as f:
            f.write(bytes(b))
        out = run(exe, "run", d, c, "keep")
        if "DIED" in out:
            died.append(out.split("DIED")[-1].strip())
            break
    check("every damaged compressed file installs or stops the install, nothing out of bounds",
          not died, "; ".join(died[:1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("--fuzz", type=int, default=400)
    args = ap.parse_args()
    if not shutil.which("mformat"):
        sys.exit("uwinsetup_hostcheck: needs mtools")
    with tempfile.TemporaryDirectory(prefix="uwinsetup-") as tmp:
        drv = hostcheck.write(tmp, "uws_host.c", DRIVER)
        srcs = [hostcheck.stage(tmp, "userland/lib/uwinsetup.c", dest="lib/uwinsetup.c", edits=CONTROL,
                                apply=args.positive_control, tool="uwinsetup_hostcheck")]
        for rel in ("userland/lib/ufatimg.c", "userland/lib/umsexpand.c"):
            srcs.append(hostcheck.stage(tmp, rel, dest="lib/" + os.path.basename(rel)))
        exe = hostcheck.compile(tmp, "uws_host", [drv, *srcs], flags=SAN,
                                includes=[tmp, os.path.join(ROOT, "userland")], tool="uwinsetup_hostcheck")
        synthetic(exe, tmp)
        real_disks(exe, tmp)
        fuzz(exe, tmp, args.fuzz)
    if args.positive_control:
        print("\nuwinsetup_hostcheck: POSITIVE CONTROL -- %d check(s) went red%s" %
              (len(failures), "" if failures else " -- NONE: the check cannot see the break"))
        return 0 if failures else 1
    print("\nuwinsetup_hostcheck: %s" % ("PASS" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
