#!/usr/bin/env python3
"""The optional components' build half, on the HOST: the check, the staging, the manifest tags.

docs/software-design.md, stage 1. Three readers must agree about which
file is whose -- tools/lib/components.py (the check and the seed step),
tools/build/stage_components.py, and tools/dev/update_server.py's
manifest -- and this holds each against a staging tree it builds in a
temporary directory, so the answer is known:

  1. The shipped data/components pass the check, and against this
     checkout's seed/sync when it has one.
  2. Every fault the check exists for is REPORTED: a path missing from
     the tree, the same path in two components, a Needs= naming
     nothing, a hand-written Size=, a GPL-built program in an MIT
     component, the Browser no longer needing the engine, and a
     Built=if-present component with half its paths.
  3. A Built=if-present component with none of its paths is not staged,
     and is not an error.
  4. stage_components.py writes each definition with Size= and Files=
     summed over ITS files: /tests/nsmonkey counts to the Web page
     engine, carved out of Demos and tests' /tests/.
  5. The manifest tags each file with its component, tags nothing in
     the base, and never tags an /etc line.

    python3 tools/hostchecks/components_hostcheck.py
    python3 tools/hostchecks/components_hostcheck.py --positive-control

THE POSITIVE CONTROL builds every faulty fixture in check 2 WITHOUT its
fault and still expects the fault reported, and claims nsmonkey for Demos
in check 4: each must then fail, or the check is reporting nothing.
"""

# Python only: the check and the server's tagging against fixture trees.
RUNNER = {"name": "components_host", "covers": "optional components: the check, staging sizes and manifest tags",
          "parallel": True, "gate": True}

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import components  # noqa: E402
import update_server  # noqa: E402
from harness import Results  # noqa: E402

ROOT = toolpath.REPO


def tree(base, files):
    """A staging tree: {machine path: bytes}."""
    for path, body in files.items():
        full = os.path.join(base, path.lstrip("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(body)


def staging_for(defs, tmp, name="stage"):
    """A tree holding every path the definitions name, one small file per
    file path and two per directory -- so the real definitions pass."""
    base = os.path.join(tmp, name)
    files = {}
    for c in defs.values():
        for p in c.paths:
            if p.endswith("/"):
                files[p + "a.bin"] = b"a" * 10
                files[p + "sub/b.bin"] = b"b" * 20
            else:
                files[p] = c.id.encode() * 3
    files["/bin/ls"] = b"base"
    files["/etc/shells"] = b"/bin/tosh\n"
    tree(base, files)
    return base


def copy_defs(tmp, name="defs"):
    d = os.path.join(tmp, name)
    shutil.copytree(components.SOURCE_DIR, d)
    return d


def edit(path, before, after):
    with open(path) as fh:
        text = fh.read()
    if before not in text:
        sys.exit(f"components_hostcheck: the fixture cannot find {before!r} in {path}")
    with open(path, "w") as fh:
        fh.write(text.replace(before, after, 1))


def reported(c, label, defs_dir, staging, needle, fault, control):
    """Inject `fault` (a function of the defs dir and staging), unless this
    is the control, and require a problem containing `needle`."""
    if not control:
        fault(defs_dir, staging)
    probs = components.check(components.load(defs_dir), staging)
    c.ok(label, any(needle in p for p in probs), "; ".join(probs)[:300] or "no problem reported")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--positive-control", action="store_true",
                    help="leave every fault out and still expect it reported: must FAIL")
    args = ap.parse_args()
    ctl = args.positive_control
    c = Results()
    tmp = tempfile.mkdtemp(prefix="components_hostcheck.")
    try:
        # 1. the shipped definitions
        real = components.load(components.SOURCE_DIR)
        c.ok("the shipped definitions pass the check", not components.check(real),
             "; ".join(components.check(real))[:300])
        seed = os.path.join(ROOT, "seed", "sync")
        if os.path.isdir(seed):
            probs = components.check(real, seed)
            c.ok("...and against this checkout's seed/sync", not probs, "; ".join(probs)[:300])
        base = staging_for(real, tmp)
        c.ok("...and against a tree holding all of their paths", not components.check(real, base),
             "; ".join(components.check(real, base))[:300])

        # 2. each fault, reported
        def fresh(n):
            d = copy_defs(tmp, f"defs{n}")
            s = os.path.join(tmp, f"stage{n}")
            shutil.copytree(base, s)
            return d, s

        cases = [
            ("a path missing from the tree is reported", "/bin/wm/apps/doom is not in the staging tree",
             lambda d, s: os.remove(os.path.join(s, "bin/wm/apps/doom"))),
            ("the same path in two components is reported", "is in both",
             lambda d, s: edit(os.path.join(d, "make.component"), "Paths=/usr/bin/make",
                               "Paths=/usr/bin/make /bin/w16")),
            ("a Needs= naming nothing is reported", "Needs=nosuch",
             lambda d, s: edit(os.path.join(d, "doom.component"), "Needs=", "Needs=nosuch")),
            ("a hand-written Size= is reported", "Size= is written by the seed step",
             lambda d, s: edit(os.path.join(d, "doom.component"), "Needs=", "Size=5\nNeeds=")),
            ("a program built from a GPL-only port in an MIT component is reported",
             "built from the GPL-only port doom",
             lambda d, s: edit(os.path.join(d, "doom.component"), "License=GPL-2.0-or-later",
                               "License=MIT")),
            ("the engine's nsmonkey left in Demos and tests is reported",
             "/tests/nsmonkey is built from the GPL-only port netsurf",
             lambda d, s: edit(os.path.join(d, "webengine.component"), " /tests/nsmonkey", "")),
            ("the Browser no longer needing the engine is reported",
             "needs the GPL-only port netsurf",
             lambda d, s: edit(os.path.join(d, "browser.component"), "Needs=webengine", "Needs=")),
            ("a Built=if-present component with HALF its paths is reported",
             "all or none",
             lambda d, s: os.remove(os.path.join(s, "usr/bin/gcc"))),
            ("a directory named without its / is reported", "a directory's path ends with /",
             lambda d, s: edit(os.path.join(d, "win16.component"), "/usr/share/w16/", "/usr/share/w16")),
        ]
        for n, (label, needle, fault) in enumerate(cases):
            d, s = fresh(n)
            reported(c, label, d, s, needle, fault, ctl)

        # 3. if-present with nothing staged
        d, s = fresh("ip")
        shutil.rmtree(os.path.join(s, "usr", "bin"))
        shutil.rmtree(os.path.join(s, "usr", "libexec"))
        shutil.rmtree(os.path.join(s, "usr", "lib"))
        shutil.rmtree(os.path.join(s, "usr", "include"))
        defs = components.load(d)
        c.ok("a Built=if-present component with none of its paths is no error",
             not components.check(defs, s), "; ".join(components.check(defs, s))[:300])
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/build/stage_components.py"),
                            s, "--defs", d], capture_output=True, text=True)
        staged = os.listdir(os.path.join(s, components.STAGED_DIR)) if r.returncode == 0 else []
        c.ok("...and is not staged, while the rest are",
             r.returncode == 0 and "cc.component" not in staged and "make.component" not in staged
             and "doom.component" in staged, (r.stdout + r.stderr)[-300:])

        # 4. sizes: the carve-out counts to its owner. The control moves
        # nsmonkey to Demos; the GPL rule then refuses the stage, and
        # nothing is staged to read.
        d, s = fresh("sz")
        if ctl:
            edit(os.path.join(d, "webengine.component"), " /tests/nsmonkey", "")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/build/stage_components.py"),
                            s, "--defs", d], capture_output=True, text=True)
        got = {}
        for cid in ("webengine", "demos", "doom"):
            p = os.path.join(s, components.STAGED_DIR, cid + ".component")
            kv = dict(ln.split("=", 1) for ln in open(p).read().splitlines()
                      if "=" in ln and not ln.startswith("#")) if os.path.isfile(p) else {}
            got[cid] = (int(kv.get("Size", -1)), int(kv.get("Files", -1)))
        # webengine: /bin/webcontent (27 B), /tests/nsmonkey (27 B), /usr/share/netsurf/ (10 + 20)
        c.ok("the Web page engine's size counts /tests/nsmonkey (carved out of /tests/)",
             got["webengine"] == (27 + 27 + 30, 4), f"{got['webengine']} {r.stderr[-200:]}")
        # demos: /bin/wm/demos/ and /tests/ (30 each), five entries of 15 B
        c.ok("...and Demos and tests' does not",
             got["demos"] == (30 + 30 + 5 * 15, 9), f"{got['demos']}")
        c.ok("...and DOOM's is its two files", got["doom"] == (12 * 2, 2), f"{got['doom']}")

        # 5. the manifest's tags
        tree(s, {"/etc/settings.d/x": b"y"})
        text, _files = update_server.Manifest(s, kernel=False).build()
        tags = {}
        for ln in text.splitlines():
            f = ln.split(" ")
            if ln.startswith("#") or len(f) < 3:
                continue
            opts = f[3].split(",") if len(f) > 3 else []
            tags[f[2]] = next((o[10:] for o in opts if o.startswith("component=")), None)
        c.ok("the manifest tags a component's file", tags.get("/bin/wm/apps/doom") == "doom",
             str(tags.get("/bin/wm/apps/doom")))
        c.ok("...a carved-out file with its own owner", tags.get("/tests/nsmonkey") == "webengine"
             and tags.get("/tests/a.bin") == "demos", f"{tags.get('/tests/nsmonkey')} {tags.get('/tests/a.bin')}")
        c.ok("...a base file with none", "/bin/ls" in tags and tags["/bin/ls"] is None)
        c.ok("...and the component definitions themselves with none (they are the base's)",
             "/usr/share/components/doom.component" in tags
             and tags["/usr/share/components/doom.component"] is None)
        c.ok("...and never an /etc line", tags.get("/etc/shells", "absent") is None)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    bad = c.failed()
    print(f"\ncomponents_hostcheck: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed")
    if ctl:
        print("components_hostcheck: positive control " +
              ("OK -- the faults are seen" if bad else "DID NOT FAIL -- the check sees nothing"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
