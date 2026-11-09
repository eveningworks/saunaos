"""The optional components (docs/software-design.md), read and checked on the host.

One module for the three readers that must agree: the seed step
(tools/build/stage_components.py, which stages them with their sizes),
the static check (tools/checks/check_optional_components.py) and the
update server (tools/dev/update_server.py, which tags each manifest line
with `component=<id>`). The machine's own reader is
userland/libs/base/ucomponent.c, and the format is
data/components/README.md.

    defs = components.load(components.SOURCE_DIR)      # {id: Component}
    problems = components.check(defs, staging)          # [str]
    owner = components.owner_map(defs)                  # path -> id or None

A FILE BELONGS TO THE COMPONENT WITH THE LONGEST PATH THAT COVERS IT: a
file path, or a directory ending `/` and everything beneath. So
`/tests/nsmonkey` in the Web page engine is carved out of `/tests/` in
Demos and tests. The same path in two components is refused.
"""
import os
import re

import toolpath

REPO = toolpath.REPO
SOURCE_DIR = os.path.join(REPO, "data", "components")
STAGED_DIR = "usr/share/components"      # under the staging tree, and on the machine
SUFFIX = ".component"

# Keys a data/components file may carry. Size= and Files= are the
# STAGED copy's, written by the seed step from the files it found.
KEYS = {"Name", "Summary", "Project", "Category", "License", "Icon", "Needs", "Paths", "Built"}
GENERATED = {"Size", "Files"}
CATEGORIES = {"games", "internet", "devtools", "win16", "demos", "system", "graphics",
              "multimedia", "utility"}
ID = re.compile(r"^[a-z][a-z0-9-]{0,22}$")    # ucomponent.h's UCOMPONENT_ID_MAX


class Component:
    def __init__(self, cid, path, keys):
        self.id = cid
        self.file = path
        self.keys = keys
        self.paths = keys.get("Paths", "").split()
        self.needs = keys.get("Needs", "").split()
        self.license = keys.get("License", "")
        self.if_present = keys.get("Built", "") == "if-present"

    def is_gpl(self):
        return is_gpl(self.license)


def is_gpl(expr):
    """GPL or AGPL anywhere in the expression -- never LGPL, a library
    licence a closed program may link (the Makefile's GPL_ONLY_PORTS
    rule, and ucomponent.c's)."""
    return any(re.match(r"^A?GPL-", w) for w in re.split(r"[\s()]+", expr))


def parse(path):
    """(keys, problems) for one file: `key=value` lines and `#` comments."""
    keys, problems = {}, []
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            k, sep, v = line.partition("=")
            if not sep:
                problems.append(f"{path}:{n}: not key=value")
                continue
            k, v = k.strip(), v.strip()
            if k in keys:
                problems.append(f"{path}:{n}: {k}= twice (one line each; the machine reads the first)")
            keys[k] = v
    return keys, problems


def load(directory, problems=None):
    """{id: Component} for every <id>.component in `directory`."""
    out = {}
    if not os.path.isdir(directory):
        return out
    for name in sorted(os.listdir(directory)):
        if not name.endswith(SUFFIX):
            continue
        path = os.path.join(directory, name)
        keys, bad = parse(path)
        if problems is not None:
            problems.extend(bad)
        cid = name[:-len(SUFFIX)]
        out[cid] = Component(cid, path, keys)
    return out


def owner_of(defs, path):
    """The id of the component that holds `path` on a machine, or None for
    the base system: the longest covering Paths= entry wins."""
    best, best_len = None, -1
    for c in defs.values():
        for p in c.paths:
            hit = path == p if not p.endswith("/") else path.startswith(p)
            if hit and len(p) > best_len:
                best, best_len = c.id, len(p)
    return best


def owner_map(defs):
    """A fast owner_of for many paths (the manifest's thousands)."""
    entries = sorted(((p, c.id) for c in defs.values() for p in c.paths),
                     key=lambda e: len(e[0]), reverse=True)
    exact = {p: cid for p, cid in entries if not p.endswith("/")}
    dirs = [(p, cid) for p, cid in entries if p.endswith("/")]

    def owner(path):
        if path in exact:
            return exact[path]
        for p, cid in dirs:          # longest first
            if path.startswith(p):
                return cid
        return None
    return owner


def expand(staging, comp):
    """[(machine path, local file)] for the files a component holds in a
    staging tree -- symbolic links left out, as the update manifest does."""
    out = []
    for p in comp.paths:
        local = os.path.join(staging, p.lstrip("/"))
        if p.endswith("/"):
            for root, dirs, names in os.walk(local):
                dirs.sort()
                for n in sorted(names):
                    full = os.path.join(root, n)
                    if os.path.islink(full):
                        continue
                    rel = os.path.relpath(full, staging).replace(os.sep, "/")
                    out.append(("/" + rel, full))
        elif os.path.isfile(local) and not os.path.islink(local):
            out.append((p, local))
    return out


def present(staging, comp):
    """How many of its Paths= exist in the staging tree."""
    return sum(1 for p in comp.paths if os.path.lexists(os.path.join(staging, p.lstrip("/"))))


def _makefile():
    with open(os.path.join(REPO, "Makefile")) as fh:
        return fh.read()


def gpl_only_ports():
    """{port: [program stems built from it]} for each GPL-only port, as the
    Makefile declares them (PORT_LICENSE_ and PORT_PROGRAMS_)."""
    text = _makefile()
    lic = dict(re.findall(r"^PORT_LICENSE_(\w+)\s*=\s*(.*?)\s*$", text, re.M))
    progs = {k: v.split() for k, v in re.findall(r"^PORT_PROGRAMS_(\w+)\s*=\s*(.*?)\s*$", text, re.M)}
    out = {}
    for port, expr in lic.items():
        ids = expr.split()[0::2]
        if ids and all(i.startswith(("GPL-", "AGPL-")) for i in ids):
            out[port] = progs.get(port, [])
    return out


def needs_port_programs():
    """{port: [program stems]} from component.mk's NEEDS_PORT -- the programs
    that are useless without a port, which NOGPL=1 drops too."""
    out = {}
    for top in ("apps", "services", "utils"):
        for root, _dirs, files in os.walk(os.path.join(REPO, "userland", top)):
            if "component.mk" not in files:
                continue
            text = open(os.path.join(root, "component.mk")).read()
            for m in re.finditer(r"^NEEDS_PORT\s*=\s*(\S+)", text, re.M):
                rel = os.path.relpath(root, os.path.join(REPO, "userland"))
                out.setdefault(m.group(1), []).append(f"{rel}/{os.path.basename(root)}")
    return out


def installed_path(stem):
    """Where the seed step puts the program built from userland/<stem>.c --
    the Makefile's "Destination comes from the SOURCE DIRECTORY" rule."""
    text = _makefile()
    m = re.search(rf"^SEED_NAME_{re.escape(stem)}\s*=\s*(\S+)", text, re.M)
    parts = stem.split("/")
    name = m.group(1) if m else parts[-1]
    if parts[0] == "apps" and len(parts) >= 4:
        mk = os.path.join(REPO, "userland", *parts[:-1], "component.mk")
        if os.path.isfile(mk):
            im = re.search(r"^INSTALL_AS\s*=\s*(\S+)", open(mk).read(), re.M)
            if im:
                name = im.group(1)
        return f"/bin/wm/{'/'.join(parts[1:-2])}/{name}"
    if parts[0] == "tests":
        return f"/tests/{name}"
    return f"/bin/{name}"


def check(defs, staging=None, nogpl=False):
    """Every problem with the definitions, as sentences. With `staging`
    (a seeded tree), also that every path is there -- for each component
    that tree carries (staged())."""
    problems = []
    for c in defs.values():
        where = os.path.relpath(c.file, REPO) if c.file.startswith(REPO) else c.file
        if not ID.match(c.id):
            problems.append(f"{where}: the id {c.id!r} is not lower-case letters, digits and -, "
                            f"at most 23 long")
        for k in c.keys:
            if k in GENERATED:
                problems.append(f"{where}: {k}= is written by the seed step from the files, "
                                f"never by hand")
            elif k not in KEYS:
                problems.append(f"{where}: unknown key {k}=")
        for k in ("Name", "Summary", "Category", "License", "Paths"):
            if not c.keys.get(k):
                problems.append(f"{where}: no {k}=")
        if c.keys.get("Category") and c.keys["Category"] not in CATEGORIES:
            problems.append(f"{where}: Category={c.keys['Category']} is not one of "
                            f"{', '.join(sorted(CATEGORIES))}")
        if c.keys.get("Built", "if-present") != "if-present":
            problems.append(f"{where}: Built= takes only if-present")
        for p in c.paths:
            if not p.startswith("/") or "//" in p or "/../" in p or p.endswith("/.."):
                problems.append(f"{where}: {p!r} is not an absolute path")
            if p.startswith(("/etc/", "/home/", "/var/", "/boot/")) or p in ("/etc/", "/home/"):
                problems.append(f"{where}: {p} is the machine's own, never a component's")
            if p.startswith(("/usr/share/components", "/usr/share/icons")):
                problems.append(f"{where}: {p}: a component's definition and icon are the BASE "
                                f"system's, so a removed one is still listed")
        for n in c.needs:
            if n not in defs:
                problems.append(f"{where}: Needs={n}, and there is no data/components/{n}.component")
            elif defs[n].needs:
                problems.append(f"{where}: Needs={n}, which needs {' '.join(defs[n].needs)} -- "
                                f"one level of Needs= only")
            if n == c.id:
                problems.append(f"{where}: needs itself")

    # The same path in two components: which one holds it is a guess.
    seen = {}
    for c in defs.values():
        for p in c.paths:
            if p in seen and seen[p] != c.id:
                problems.append(f"{p} is in both {seen[p]} and {c.id}")
            seen.setdefault(p, c.id)

    # THE GPL SWITCH CANNOT MISS ONE: a program built from a GPL-only port
    # is in a GPL component, and one useless without such a port is in a
    # component that is GPL or needs one -- what NOGPL=1 drops.
    owner = owner_map(defs)
    for port, stems in gpl_only_ports().items():
        for stem in stems:
            path = installed_path(stem)
            cid = owner(path)
            if cid is None:
                problems.append(f"{path} is built from the GPL-only port {port} and is in no "
                                f"component -- the GPL switch could not hide it")
            elif not defs[cid].is_gpl():
                problems.append(f"{path} is built from the GPL-only port {port}, but its "
                                f"component {cid} says License={defs[cid].license}")
    gpl_ports = set(gpl_only_ports())
    for port, stems in needs_port_programs().items():
        if port not in gpl_ports:
            continue
        for stem in stems:
            path = installed_path(stem)
            cid = owner(path)
            if cid is None or not (defs[cid].is_gpl() or
                                   any(defs[n].is_gpl() for n in defs[cid].needs if n in defs)):
                problems.append(f"{path} needs the GPL-only port {port} and is in "
                                f"{'no component' if cid is None else cid + ', which neither is GPL nor needs one'}")

    if staging:
        for c in defs.values():
            if not staged(staging, c, defs, nogpl):
                continue          # not in this build: never staged
            for p in c.paths:
                local = os.path.join(staging, p.lstrip("/"))
                if not os.path.lexists(local):
                    problems.append(f"{c.id}: {p} is not in the staging tree"
                                    + (" (Built=if-present: all or none)" if c.if_present else ""))
                elif p.endswith("/") != os.path.isdir(local):
                    problems.append(f"{c.id}: {p} is a {'directory' if os.path.isdir(local) else 'file'}"
                                    f" -- a directory's path ends with /")
        for c in defs.values():
            for n in c.needs:
                if n in defs and not staged(staging, defs[n], defs, nogpl) and \
                        staged(staging, c, defs, nogpl):
                    problems.append(f"{c.id} needs {n}, which is not in this build")
    return problems


def staged(staging, comp, defs=None, nogpl=False):
    """Whether this build carries the component: not a Built=if-present
    one with none of it staged, and under NOGPL=1 nothing GPL and nothing
    that needs it (the Makefile drops those programs)."""
    if nogpl and (comp.is_gpl() or any(defs[n].is_gpl() for n in comp.needs if defs and n in defs)):
        return False
    return not (comp.if_present and present(staging, comp) == 0)


def sizes(defs, staging):
    """{id: (bytes, files)} of what each component holds in the staging tree,
    each file counted once, by its owner."""
    owner = owner_map(defs)
    out = {cid: [0, 0] for cid in defs}
    for c in defs.values():
        for path, local in expand(staging, c):
            if owner(path) != c.id:
                continue          # carved out by a longer path
            out[c.id][0] += os.path.getsize(local)
            out[c.id][1] += 1
    return {cid: tuple(v) for cid, v in out.items()}
