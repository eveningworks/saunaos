#!/usr/bin/env python3
"""Refuse a program directory the build would misread.

A program of several files is a directory with a component.mk
(docs/tree-design.md, stages 3 and 4): a GUI program
userland/apps/<class>/<name>/, a service userland/services/<name>/, a
command userland/utils/<name>/. Every .c in the directory is linked into
it, <name>.c holds main(), and the manifest says only what the directory
cannot -- the Makefile's "component manifests" section is the format. A
one-file command stays userland/utils/<name>.c. The Makefile reads the
manifests with `include`, so a mistake in one is not a parse error but a
program built wrongly:

  1. A program directory holding .c files but no component.mk is NOT
     BUILT -- discovery is by manifest -- and nothing says so; a desktop
     entry or a services.d file then points at nothing. So is a .c one
     level too deep (services/<name>/<sub>/x.c: the wildcard is one
     directory) or one too shallow (a flat services/x.c).
  2. A component.mk with no <name>.c beside it builds an ELF from no
     main().
  3. A manifest may set EXTRA, ULIBS, INSTALL_AS and NEEDS_PORT, with
     `=` (deferred, as everything in the Makefile is). Anything else --
     a typo'd name, a `:=`, a rule -- leaks into the whole Makefile,
     because an included file shares its namespace.
  4. EXTRA must not name another program's part (an `apps/`, `services/`
     or `utils/` stem): one program's parts are not another's library.
     Shared code goes in userland/lib/ or userland/common/.
  5. Two programs with directories may share a name only if neither has
     anything beyond its own main(): the per-program keys
     (EXTRA_OBJS_<name>, ULIB_SO_<name>) are by bare name, so the second
     manifest would overwrite the first. (A one-file command NAMED like
     a GUI program links that program's parts on purpose:
     utils/screenshot.c and the Screenshot app's settings code.)

Exits 1 on any finding; there is no waiver.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

REPO = toolpath.REPO
USERLAND = os.path.join(REPO, "userland")
# Each tree, and how deep below it a program directory sits: apps/ has a
# class directory first (apps/wallpapers/players/<name>/ is one deeper).
TREES = (("apps", (2, 3)), ("services", (1,)), ("utils", (1,)))
ALLOWED = {"EXTRA", "ULIBS", "INSTALL_AS", "NEEDS_PORT"}
ASSIGN = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*)\s*=(.*)$")


def logical_lines(text):
    """Lines with backslash continuations joined, comments and blanks dropped."""
    out, cur = [], ""
    for raw in text.split("\n"):
        line = raw.split("#", 1)[0].rstrip()
        if line.endswith("\\"):
            cur += line[:-1] + " "
            continue
        cur += line
        if cur.strip():
            out.append(cur.strip())
        cur = ""
    return out


def manifest_problems(rel, path):
    problems, fields = [], {}
    for line in logical_lines(open(path, encoding="utf-8").read()):
        m = ASSIGN.match(line)
        if not m or line.split("=", 1)[0].rstrip().endswith((":", "+", "?", "!")):
            problems.append(f"{rel}: not a plain `NAME = value` line: {line!r}")
            continue
        name, value = m.group(1), m.group(2).strip()
        if name not in ALLOWED:
            problems.append(f"{rel}: sets {name}; a manifest may set only {', '.join(sorted(ALLOWED))}")
            continue
        fields[name] = value
    for stem in fields.get("EXTRA", "").split():
        if stem.startswith(("apps/", "services/", "utils/")):
            problems.append(f"{rel}: EXTRA names another program's part ({stem}) -- "
                            f"shared code belongs in userland/lib/ or userland/common/")
    return problems, fields


def scan_tree(tree, depths, problems, by_name):
    top = os.path.join(USERLAND, tree)
    for root, dirs, files in sorted(os.walk(top)):
        dirs.sort()
        rel = os.path.relpath(root, REPO)
        depth = 0 if root == top else len(os.path.relpath(root, top).split(os.sep))
        name = os.path.basename(root)
        sources = sorted(f for f in files if f.endswith(".c"))
        has_mk = "component.mk" in files
        if tree == "utils" and depth == 0:
            if has_mk:
                problems.append(f"{rel}/component.mk: utils/ itself is no program -- "
                                f"a command of several files is utils/<name>/")
            continue                     # utils/<name>.c: one file, one program
        if not sources and not has_mk:
            continue
        if depth not in depths:
            where = "/".join(["<class>"] * (depths[0] - 1) + ["<name>"])
            problems.append(f"{rel}/ holds {', '.join(sources) or 'component.mk'} outside a "
                            f"program directory -- a program is {tree}/{where}/")
            continue
        if not has_mk:
            problems.append(f"{rel}/ has .c files but no component.mk, so the build never sees it")
            continue
        if name + ".c" not in sources:
            problems.append(f"{rel}/component.mk: no {name}.c beside it to hold main()")
        mk_rel = os.path.join(rel, "component.mk")
        found, fields = manifest_problems(mk_rel, os.path.join(root, "component.mk"))
        problems += found
        busy = bool(fields) or len(sources) > 1
        by_name.setdefault(name, []).append((rel, busy))


def main():
    problems, by_name = [], {}
    for tree, depths in TREES:
        if not os.path.isdir(os.path.join(USERLAND, tree)):
            problems.append(f"userland/{tree}/ does not exist -- this check no longer matches the tree")
            continue
        scan_tree(tree, depths, problems, by_name)
    for name, where in sorted(by_name.items()):
        if len(where) > 1 and any(busy for _, busy in where):
            problems.append(f"two programs are called {name} ({', '.join(r for r, _ in where)}) and "
                            f"one has parts or a manifest: EXTRA_OBJS_{name} is keyed by name, so one "
                            f"would overwrite the other")
    if problems:
        print("check_components: FAILED")
        for p in problems:
            print("  " + p)
        return 1
    print(f"check_components: ok -- {sum(len(w) for w in by_name.values())} program directories, "
          f"each with a manifest the build reads correctly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
