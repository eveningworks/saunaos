"""What runs each test, read from the test itself.

A test's FOLDER names its runner, and a `RUNNER = {...}` literal near
its top says the rest -- the shape of LLVM lit's `REQUIRES:` lines. The
runners discover tests from here instead of each keeping a list, so a
new test is run by the right runner from the moment it is saved:

    tests/gate/     preflight.sh            every one, every gate
    tests/gui/      gui_regress.py          a fresh image and guest each
    tests/sweep/    ondemand_sweep.py       on demand; skips what cannot run here
    tests/manual/   nothing                 by hand -- `manual` says why
    hostchecks/     hostchecks.py           host gcc vs a foreign oracle; `gate`
                                            ones in preflight, the rest in the sweep

RUNNER is read with `ast.literal_eval`, never imported, so it must stay a
plain literal: a dict, or a list of dicts for a tool run more than once
(`audio_test.py` and `audio_test.py --card hda`). Keys:

    covers     what a pass means, in a line (REQUIRED)
    name       the short name runners print and `-k`/`--only` match;
               default: the file name without `_test.py`/`_hostcheck.py`/`.py`
    args       extra arguments for this entry
    vm         gui: extra `vm.py` flags for this tool's guest (`--audio hda`)
    parallel   sweep/hostchecks: may run beside others (default: alone)
    needs      sweep/hostchecks: [precondition key, why it skips] --
               the keys are `ondemand_sweep.precondition_met()`'s
    shared_vm  sweep: attaches to the shared `vm.py` guest, booted for it
    gate       hostchecks: also run by preflight
    manual     tests/manual and hostchecks: why no runner runs it

    python3 tools/lib/runner_meta.py            # every entry, by folder
    python3 tools/lib/runner_meta.py --check    # what checks/check_runners.py runs
    python3 tools/lib/runner_meta.py --folder tests/gate   # path TAB name TAB covers, for a shell
"""
import ast
import os
import sys

import toolpath

FOLDERS = {
    "tests/gate": "preflight.sh",
    "tests/gui": "gui_regress.py",
    "tests/sweep": "ondemand_sweep.py",
    "tests/manual": None,
    "hostchecks": "hostchecks.py",
}
KEYS = {"covers", "name", "args", "vm", "parallel", "needs", "shared_vm", "gate", "manual"}
_SUFFIXES = ("_test.py", "_hostcheck.py", ".py", ".sh")


class Entry:
    """One run of one tool."""

    def __init__(self, path, folder, d):
        self.path = path
        self.script = os.path.basename(path)
        self.folder = folder
        self.covers = d.get("covers", "")
        self.name = d.get("name") or _stem(self.script)
        self.args = list(d.get("args", []))
        self.vm = list(d.get("vm", []))
        self.parallel = bool(d.get("parallel", False))
        self.needs = tuple(d["needs"]) if d.get("needs") else None
        self.shared_vm = bool(d.get("shared_vm", False))
        self.gate = bool(d.get("gate", False))
        self.manual = d.get("manual")

    @property
    def rel(self):
        return os.path.relpath(self.path, toolpath.REPO)

    def __repr__(self):
        return f"Entry({self.name!r}, {self.rel!r})"


def _stem(script):
    for s in _SUFFIXES:
        if script.endswith(s):
            return script[: -len(s)]
    return script


def read(path):
    """The RUNNER literal of `path` as a list of dicts; [] when it has none."""
    if not path.endswith(".py"):
        return []
    tree = ast.parse(open(path, encoding="utf-8").read(), path)
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and getattr(node.targets[0], "id", None) == "RUNNER"):
            v = ast.literal_eval(node.value)
            return v if isinstance(v, list) else [v]
    return []


def scripts(folder):
    """Every script in a tools/ folder, sorted."""
    d = os.path.join(toolpath.TOOLS, folder)
    if not os.path.isdir(d):
        return []
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith((".py", ".sh")))


def entries(folder):
    """Every entry of every script in `folder` (a FOLDERS key)."""
    out = []
    for p in scripts(folder):
        out += [Entry(p, folder, d) for d in read(p)]
    return out


def check():
    """Problems, as strings: a test with no RUNNER, an unknown key, a name used twice."""
    problems = []
    seen = {}
    for folder in FOLDERS:
        for p in scripts(folder):
            rel = os.path.relpath(p, toolpath.REPO)
            try:
                ds = read(p)
            except (SyntaxError, ValueError) as e:
                problems.append(f"{rel}: RUNNER is not a plain literal ({e})")
                continue
            if not ds:
                problems.append(f"{rel}: no RUNNER = {{...}} -- its folder's runner cannot "
                                "say what it covers (tools/lib/runner_meta.py)")
                continue
            for d in ds:
                bad = set(d) - KEYS
                if bad:
                    problems.append(f"{rel}: unknown RUNNER key(s) {sorted(bad)}")
                if not d.get("covers"):
                    problems.append(f"{rel}: RUNNER has no 'covers'")
                if folder == "tests/manual" and not d.get("manual"):
                    problems.append(f"{rel}: in tests/manual/ with no 'manual' reason")
                if d.get("manual") and folder not in ("tests/manual", "hostchecks"):
                    problems.append(f"{rel}: 'manual' outside tests/manual/ -- move it there")
                e = Entry(p, folder, d)
                if e.name in seen and seen[e.name] != rel:
                    problems.append(f"{rel}: name {e.name!r} is also {seen[e.name]}'s")
                seen.setdefault(e.name, rel)
    # a test outside the runner folders is a test nothing runs
    for name, p in toolpath.all_tools().items():
        rel = os.path.relpath(p, toolpath.TOOLS)
        if name.endswith(("_test.py", "_hostcheck.py")) and not rel.startswith(("tests/", "hostchecks/")):
            if name not in ("qmp_test.py", "mkpart_test.py"):   # libraries, named before this rule
                problems.append(f"tools/{rel}: a test outside tests/ and hostchecks/ -- no runner finds it")
    return problems


def main():
    if "--folder" in sys.argv:
        for e in entries(sys.argv[sys.argv.index("--folder") + 1]):
            print(f"{e.rel}\t{e.name}\t{e.covers}")
        return 0
    if "--check" in sys.argv:
        problems = check()
        for p in problems:
            print("  " + p)
        print(f"runner_meta: {len(problems)} problem(s)" if problems else "runner_meta: every test is run")
        return 1 if problems else 0
    for folder, runner in FOLDERS.items():
        es = entries(folder)
        print(f"{folder}/  ({runner or 'by hand'}, {len(es)})")
        for e in es:
            print(f"  {e.name:<18} {e.script:<30} {e.covers[:70]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
