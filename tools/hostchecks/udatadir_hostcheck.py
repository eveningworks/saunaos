#!/usr/bin/env python3
"""Check userland/libs/base/udatadir.c -- where a program's data is written
and found -- on the HOST, in a scratch tree.

DOOM's IWADs and ScummVM's games are downloaded into the person's data
folder and found there BEFORE the system's copy, so a wrong order either
hides a download behind an old system file or never finds the system's.
Checked against the XDG Base Directory rules:

  - home: $XDG_DATA_HOME/REL; unset or relative, <home>/.local/share/REL,
    <home> being udir_home() -- $HOME, else the desktop account's home,
    which the driver stands in for with /home/user
  - find: the home folder first, then each $XDG_DATA_DIRS entry in order;
    a relative entry is skipped; nowhere is NULL
  - prepare: home's answer, with every folder above it made

    python3 tools/hostchecks/udatadir_hostcheck.py
    python3 tools/hostchecks/udatadir_hostcheck.py --positive-control   # must FAIL
"""

# base/udatadir -- the data folders' order -- in a scratch tree.
RUNNER = {"covers": "per-user data folder: write path, lookup order, folder making", "gate": True}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

DRIVER = r"""
#include "base/udatadir.h"
#include "base/udirs.h"
#include <stdlib.h>
#include <string.h>

// udir_home()'s contract (base/udirs.h) without /etc/passwd: $HOME when
// it is a folder other than `/`, else the desktop account's home.
const char *udir_home(void) {
    const char *h = getenv("HOME");
    return h && h[0] == '/' && h[1] ? h : "/home/user";
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    char *p = !strcmp(argv[1], "home") ? udatadir_home(argv[2])
            : !strcmp(argv[1], "find") ? udatadir_find(argv[2])
            : !strcmp(argv[1], "state") ? udatadir_state(argv[2], "/var/lib/m")
            : !strcmp(argv[1], "cache") ? udatadir_cache(argv[2], "/var/cache/m")
            : udatadir_prepare(argv[2]);
    printf("%s\n", p ? p : "NULL");
    free(p);
    return 0;
}
"""

# The ring-3 futex lock udatadir_state_file() takes, as a host spin lock:
# this check runs one thread.
ULOCK = r"""
#pragma once
static inline void ulock_take(volatile int *l) { while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {} }
static inline void ulock_drop(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
"""

# Looks in the system folders without trying the person's first.
CONTROL = ("    if (p && access(p, F_OK) == 0) return p;\n    free(p);\n",
           "    free(p);\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="compile a lookup that skips the person's folder; the run must fail")
    args = ap.parse_args()
    fails = []

    with tempfile.TemporaryDirectory() as tmp:
        hostcheck.stage(tmp, "userland/libs/base/udatadir.h", dest="base/udatadir.h")
        hostcheck.stage(tmp, "userland/libs/base/udirs.h", dest="base/udirs.h")
        hostcheck.write(tmp, "rt/ulock.h", ULOCK)
        src = hostcheck.stage(tmp, "userland/libs/base/udatadir.c", edits=[CONTROL],
                              apply=args.positive_control, tool="udatadir_hostcheck")
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        exe = hostcheck.compile(tmp, "udatadir_host", ["-D_GNU_SOURCE", src, drv], includes=[tmp],
                                tool="udatadir_hostcheck")

        def run(op, rel, **env):
            e = {k: v for k, v in os.environ.items()
                 if k not in ("XDG_DATA_HOME", "XDG_DATA_DIRS", "XDG_STATE_HOME", "XDG_CACHE_HOME", "HOME")}
            e.update({k: v for k, v in env.items() if v is not None})
            return subprocess.run([exe, op, rel], env=e, capture_output=True, text=True).stdout.strip()

        def expect(what, got, want):
            if got != want:
                fails.append(f"{what}: got {got!r}, want {want!r}")

        home = os.path.join(tmp, "h")
        sys1, sys2 = os.path.join(tmp, "s1"), os.path.join(tmp, "s2")

        def touch(base, rel):
            p = os.path.join(base, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").close()
            return p

        expect("home from XDG_DATA_HOME", run("home", "doom/a.wad", XDG_DATA_HOME=home),
               f"{home}/doom/a.wad")
        expect("home from HOME", run("home", "doom/a.wad", HOME="/u"), "/u/.local/share/doom/a.wad")
        expect("a relative XDG_DATA_HOME is ignored",
               run("home", "doom/a.wad", XDG_DATA_HOME="rel", HOME="/u"), "/u/.local/share/doom/a.wad")
        expect("no HOME at all: the account's home", run("home", "doom/a.wad"),
               "/home/user/.local/share/doom/a.wad")

        dirs = f"rel:{sys1}:{sys2}"
        expect("nowhere is NULL", run("find", "games/x", XDG_DATA_HOME=home, XDG_DATA_DIRS=dirs), "NULL")
        in2 = touch(sys2, "games/x")
        expect("found in the second system folder",
               run("find", "games/x", XDG_DATA_HOME=home, XDG_DATA_DIRS=dirs), in2)
        in1 = touch(sys1, "games/x")
        expect("the first system folder before the second",
               run("find", "games/x", XDG_DATA_HOME=home, XDG_DATA_DIRS=dirs), in1)
        mine = touch(home, "games/x")
        expect("the person's folder before every system one",
               run("find", "games/x", XDG_DATA_HOME=home, XDG_DATA_DIRS=dirs), mine)
        only = touch(home, "games/y")
        expect("the person's folder alone",
               run("find", "games/y", XDG_DATA_HOME=home, XDG_DATA_DIRS=dirs), only)

        # The person's state and cache (users stage D1c); root's /var path
        # is the euid rule, which a check not run as root cannot reach.
        expect("state from XDG_STATE_HOME", run("state", "app/x", XDG_STATE_HOME=home), f"{home}/app/x")
        expect("state from HOME", run("state", "app/x", HOME="/u"), "/u/.local/state/app/x")
        expect("cache from XDG_CACHE_HOME", run("cache", "thumbnails", XDG_CACHE_HOME=home),
               f"{home}/thumbnails")
        expect("cache from HOME", run("cache", "thumbnails", HOME="/u"), "/u/.cache/thumbnails")

        made = run("prepare", "licenses/deep/l.txt", XDG_DATA_HOME=home)
        expect("prepare answers home's path", made, f"{home}/licenses/deep/l.txt")
        if not os.path.isdir(os.path.join(home, "licenses", "deep")):
            fails.append("prepare did not make the folders above the file")
        if os.path.exists(made):
            fails.append("prepare made the file itself")

    for f in fails:
        print(f"  FAIL  {f}")
    if args.positive_control:
        if fails:
            print(f"udatadir_hostcheck: positive control FAILED as it must ({len(fails)} finding(s))")
            return 0
        print("udatadir_hostcheck: positive control passed -- the check cannot see the order",
              file=sys.stderr)
        return 1
    print(f"udatadir_hostcheck: {'FAIL' if fails else 'PASS'} ({len(fails)} finding(s))")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
