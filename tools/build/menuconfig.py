#!/usr/bin/env python3
"""`make menuconfig` -- build.conf's options and drivers in a text UI.

Linux's menuconfig in its shape, two panes rather than nested dialogs:
the groups on the left, the selected group's options (or drivers) on the
right with their values, and the selected one's help below, with the make
variable it sets and what changing it costs. Python's curses, like
kconfiglib's menuconfig, so it needs nothing installed.

**IT SAVES TO build.local.conf, NOT build.conf** (`s`): the untracked
file build_conf.py reads on top of build.conf, Linux's `.config` beside a
committed `defconfig`. An experiment -- KASAN on, a driver as a module --
then cannot be committed by accident, and update_server.py will not
publish a build made with one. `S` is the deliberate other path: it writes
every local change INTO build.conf, value by value with every comment
kept, asks first, and empties the local file. Nothing is written that
build_conf.py would refuse: each save is parsed by it before it lands.

    python3 tools/build/menuconfig.py build.conf --local build.local.conf
    make menuconfig

Keys: arrows move, Tab or Left/Right changes pane, Space toggles or cycles
a value, Enter edits a number or text (on `cmdline`, a checklist of
docs/boot-flags.md's words), / searches, d lists the changes, s saves to
build.local.conf, S writes build.conf, w writes the changes as a profile
(configs/<name>.conf), q quits. A driver shows whether it CAN be a module
(gen_modalias.py's export check on its built object); the Profiles group
loads one into the session.
"""
import argparse
import curses
import os
import re
from struct import error as struct_error
import sys
import tempfile
import textwrap

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_conf as bc  # noqa: E402
sys.path.insert(0, os.path.join(bc.REPO, "tools", "gen"))
import gen_modalias as gm  # noqa: E402

REPO = bc.REPO
LOCAL_HEADER = (
    "# build.local.conf -- THIS CHECKOUT's overrides of build.conf, written by\n"
    "# `make menuconfig` (its `s`). Untracked, read on top of build.conf; delete\n"
    "# it to build as build.conf says. update_server.py refuses to publish a\n"
    "# build made with it; menuconfig's `S` makes these build.conf's own.\n"
)
DRIVER_RE = re.compile(r'DRIVER_DECLARE\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"')


# --- what there is to configure ------------------------------------------

def option_help(conf_path):
    """build.conf's own comment block, `#   name   kind   text` plus the
    indented lines under it: {name: (kind, [text lines])}. The help lives
    in the file it describes, so it has one owner."""
    out, cur = {}, None
    with open(conf_path) as fh:
        for line in fh:
            m = re.match(r"^#   ([a-z_]+)\s+(\S+)(?:\s{2,}(.*))?$", line.rstrip("\n"))
            if m and m.group(1) in bc.OPTIONS:
                cur = m.group(1)
                out[cur] = (m.group(2), [m.group(3)] if m.group(3) else [])
                continue
            m = re.match(r"^#\s{20,}(\S.*)$", line.rstrip("\n"))
            if m and cur:
                out[cur][1].append(m.group(1))
                continue
            cur = None
    return out


def find_drivers():
    """Every driver build.conf can name: [(name, class, description, path)],
    the name being its source file's basename, which is what build.conf
    keys on (build_conf.find_source())."""
    found = []
    for root, _dirs, files in os.walk(bc.DRIVERS):
        for f in files:
            if not f.endswith(".c") or f.endswith("_test.c"):
                continue
            path = os.path.join(root, f)
            with open(path, errors="replace") as fh:
                m = DRIVER_RE.search(fh.read())
            if m:
                found.append((f[:-2], m.group(2), m.group(3), os.path.relpath(path, REPO)))
    return sorted(found, key=lambda d: (d[1], d[0]))


_EXPORTS = None
_IMPORTS = {}   # object path -> (mtime, imports)
BUILD_DIR = os.path.join(REPO, "build")


def module_imports(name, path, value):
    """(missing, object): the functions the driver's built object calls
    that kernel/core/kexports.c does not export -- [] means it can be a
    module -- or None when it is not built. gen_modalias.py's own check,
    run early: a module compiles with the builtin's flags but the code
    model, so the builtin object's imports are the module's."""
    global _EXPORTS
    obj = (os.path.join(BUILD_DIR, "modules", name + ".o") if value == "module"
           else os.path.join(BUILD_DIR, path[:-2] + ".o"))
    try:
        mtime = os.stat(obj).st_mtime_ns
    except OSError:
        return None, obj
    if _EXPORTS is None:
        _EXPORTS = gm.exports_in(os.path.join(REPO, "kernel", "core", "kexports.c"))
    hit = _IMPORTS.get(obj)
    if not hit or hit[0] != mtime:   # parsed once per build of it, not per redraw
        try:
            hit = (mtime, set(gm.undefined_symbols(gm.Elf(obj))))
        except (SystemExit, ValueError, IndexError, struct_error):
            return None, obj
        _IMPORTS[obj] = hit
    return sorted(hit[1] - _EXPORTS), obj


def profile_info(name):
    """(first comment line, [(kind, name, value)]) of configs/<name>.conf."""
    path = bc.profile_path(name)
    desc = ""
    with open(path) as fh:
        for line in fh:
            if line.startswith("#") and line[1:].strip():
                desc = line[1:].strip()
                break
    return desc, [(k, n, v) for k, n, v, _ln in bc.parse(path)]


BOOT_FLAGS = os.path.join(REPO, "docs", "boot-flags.md")
# update_server.py's PUBLISHABLE_PROFILES: `w` never writes one, so the
# profile a release is built from changes only by a reviewed commit.
PUBLISHED_PROFILES = {"release"}


class BootWord:
    """One row of docs/boot-flags.md's table: its forms (`smp=run`,
    `smp=park`; `nokaslr`; `loglevel=<0-7>`) and what it does."""
    def __init__(self, forms, desc):
        self.forms, self.desc = forms, desc
        first = forms[0]
        self.key = first.split("=", 1)[0] if "=" in first else None
        # A later form may be a bare value of the first's key: `boot` in
        # `acpimode=legacy` / `boot` / `poweroff`.
        self.choices = [f if ("=" in f or not self.key) else f"{self.key}={f}" for f in forms]
        self.typed = any("<" in f or "[" in f or "..." in f for f in forms)

    def matches(self, word):
        if self.key:
            return word.split("=", 1)[0] == self.key
        return word in self.forms


def boot_words(path=BOOT_FLAGS):
    rows = []
    try:
        fh = open(path)
    except OSError:
        return rows
    with fh:
        for line in fh:
            if not line.startswith("| `"):
                continue
            cells = [c.replace("\\|", "|").strip() for c in re.split(r"(?<!\\)\|", line)]
            if len(cells) < 3:
                continue
            forms = re.findall(r"`([^`]+)`", cells[1])
            if not forms:
                continue
            desc = re.sub(r"[`*]", "", cells[2])
            desc = re.split(r"(?<=[.;:])\s", desc, maxsplit=1)[0].rstrip(".;:")
            rows.append(BootWord(forms, desc))
    return rows


def norm(name, value):
    """A bool's value as yes/no, whatever spelling the file used."""
    if value is None or bc.OPTIONS[name][1] != "bool":
        return value
    return "yes" if value.lower() in bc.BOOL_TRUE else "no"


# --- the model: what is set where, and the two kinds of save -------------

class Model:
    def __init__(self, base, local):
        self.base, self.local = base, local
        self.help = option_help(base)
        self.drivers = find_drivers()
        self.paths = {d[0]: d[3] for d in self.drivers}
        self.loaded_profile = None
        self.reload()

    def module_check(self, name):
        return module_imports(name, self.paths[name], self.value(("driver", name)))

    def load_profile(self, name):
        """Its lines into this session as unsaved changes. Returns why
        not, or None."""
        try:
            _desc, sets = profile_info(name)
        except (SystemExit, OSError) as e:
            return str(e)
        todo = []
        for kind, n, v in sets:
            key = ("option", n) if kind == "option" else ("driver", n)
            v = norm(n, v) if kind == "option" else v
            why = self.check(key, v)
            if why:
                return f"{n}: {why} -- nothing loaded"   # all or none
            todo.append((key, v))
        for key, v in todo:
            self.set(key, v)
        self.loaded_profile = name
        return None

    def write_profile(self, name, desc):
        """Every change from build.conf (local and unsaved) as
        configs/<name>.conf. Returns why not, or None."""
        path = bc.profile_path(name)
        ch = self.changes()
        if not ch:
            return "no change from build.conf to write"
        text = (f"# {desc}\n#\n# A PROFILE, written by `make menuconfig`: `make iso PROFILE={name}`\n"
                "# reads it on top of build.conf, and not build.local.conf.\n\n")
        text += "".join(f"option {k[1]} = {now}\n" if k[0] == "option" else f"{k[1]} = {now}\n"
                        for k, _was, now, _w in ch)
        with tempfile.TemporaryDirectory() as d:
            tmp = os.path.join(d, name + ".conf")
            with open(tmp, "w") as fh:
                fh.write(text)
            try:
                bc.load(self.base, profile=tmp)
            except SystemExit as e:
                return str(e)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        write_atomic(path, text)
        return None

    def reload(self):
        base_o, base_d = bc.load(self.base)
        all_o, all_d = bc.load(self.base, self.local)
        self.base_vals = {("option", k): norm(k, v) for k, (v, _s) in base_o.items()}
        self.base_vals.update({("driver", k): v for k, (v, _s) in base_d.items()})
        self.local_vals = {("option", k): norm(k, v) for k, (v, s) in all_o.items() if s == self.local}
        self.local_vals.update({("driver", k): v for k, (v, s) in all_d.items() if s == self.local})
        self.pending = {}

    def saved(self, key):
        if key in self.local_vals:
            return self.local_vals[key]
        if key in self.base_vals:
            return self.base_vals[key]
        return "builtin" if key[0] == "driver" else None

    def value(self, key):
        return self.pending.get(key, self.saved(key))

    def where(self, key):
        if key in self.pending:
            return "unsaved"
        if key in self.local_vals:
            return "local"
        if key in self.base_vals:
            return "build.conf"
        return "default"

    def check(self, key, value):
        """None, or why `value` is refused -- build_conf.py's own words."""
        kind, name = key
        if kind == "driver":
            return None if value in ("builtin", "module") else "builtin or module"
        try:
            bc.option_value("value", 0, name, value)
        except SystemExit as e:
            return str(e).split(": ", 2)[-1]
        return None

    def set(self, key, value):
        if value == "" and self.saved(key) is None:
            self.pending.pop(key, None)   # left empty: still not set
            return None
        why = self.check(key, value)
        if why:
            return why
        if value == self.saved(key):
            self.pending.pop(key, None)
        else:
            self.pending[key] = value
        return None

    def changes(self):
        """[(key, was, now, where)] for every unsaved and every local change."""
        out = []
        for key in sorted(set(self.local_vals) | set(self.pending)):
            base = self.base_vals.get(key, "builtin" if key[0] == "driver" else None)
            now = self.value(key)
            if now != base:
                out.append((key, base, now, "unsaved" if key in self.pending else "local"))
        return out

    def _validate(self, base_text, local_text):
        with tempfile.TemporaryDirectory() as d:
            b, l = os.path.join(d, "build.conf"), os.path.join(d, "build.local.conf")
            with open(b, "w") as fh:
                fh.write(base_text)
            if local_text is not None:
                with open(l, "w") as fh:
                    fh.write(local_text)
            try:
                bc.load(b, l)
            except SystemExit as e:
                return str(e).replace(d + os.sep, "")
        return None

    def save_local(self):
        """Pending changes into build.local.conf; a value equal to
        build.conf's drops out, and an empty file is removed."""
        merged = dict(self.local_vals)
        merged.update(self.pending)
        keep = {k: v for k, v in merged.items()
                if v != self.base_vals.get(k, "builtin" if k[0] == "driver" else None)}
        text = None
        if keep:
            text = LOCAL_HEADER + "".join(
                f"option {n} = {v}\n" if kind == "option" else f"{n} = {v}\n"
                for (kind, n), v in sorted(keep.items()))
        with open(self.base) as fh:
            why = self._validate(fh.read(), text)
        if why:
            return why
        if text is None:
            if os.path.exists(self.local):
                os.remove(self.local)
        else:
            write_atomic(self.local, text)
        self.reload()
        return None

    def promote(self):
        """Every local and unsaved change INTO build.conf, then no local
        file. Returns why not, or None."""
        changes = {key: now for key, _was, now, _w in self.changes()}
        with open(self.base) as fh:
            text = fh.read()
        new = rewrite_conf(text, changes)
        why = self._validate(new, None)
        if why:
            return why
        write_atomic(self.base, new)   # the tracked file: never half written
        if os.path.exists(self.local):
            os.remove(self.local)
        self.reload()
        return None


def write_atomic(path, text):
    """`text` into `path` through a temporary beside it: an interrupt
    leaves the old file or the new one, never a truncated build.conf."""
    tmp = f"{path}.tmp{os.getpid()}"
    with open(tmp, "w") as fh:
        fh.write(text)
    os.replace(tmp, path)


def rewrite_conf(text, changes):
    """build.conf with each change's VALUE replaced on its own line --
    spacing and any trailing comment kept -- and a line added only for
    one the file does not set: an option after the last option line, a
    module after the last driver line. A driver back to builtin keeps its
    line, now saying builtin."""
    lines = text.split("\n")

    def last(pred):
        idx = [i for i, ln in enumerate(lines) if pred(ln)]
        return idx[-1] if idx else None

    def is_option(ln):
        return re.match(r"^\s*option\s+\w+\s*=", ln) is not None

    def is_driver(ln):
        return re.match(r"^\s*\w+\s*=", ln) is not None and not is_option(ln)

    for (kind, name), value in sorted(changes.items()):
        if kind == "option":
            pat = re.compile(r"^(\s*option\s+" + re.escape(name) + r"\s*=\s*)([^#]*?)(\s*#.*)?$")
        else:
            pat = re.compile(r"^(\s*" + re.escape(name) + r"\s*=\s*)([^#]*?)(\s*#.*)?$")
        for i, ln in enumerate(lines):
            m = pat.match(ln)
            if m:
                lines[i] = m.group(1) + value + (m.group(3) or "")
                break
        else:
            if kind == "option":
                at = last(is_option)
                new = f"option {name} = {value}"
            elif value == "module":
                at = last(is_driver)
                new = f"{name} = {value}"
            else:
                continue   # builtin with no line: already what it is
            if at is None:
                while lines and lines[-1] == "":
                    lines.pop()
                lines += [new, ""]
            else:
                lines.insert(at + 1, new)
    return "\n".join(lines)


# --- the screen ----------------------------------------------------------

def fit(text, width):
    """`text` in `width` columns, cut at a word with an ellipsis."""
    if len(text) <= width:
        return text
    cut = text[:max(width - 1, 0)]
    if " " in cut:
        cut = cut[:cut.rindex(" ")]
    return cut.rstrip(" ,(") + "\u2026"


class Item:
    def __init__(self, key, label, help_lines):
        self.key, self.label, self.help = key, label, help_lines


def build_groups(model):
    groups = []
    for title, names in bc.GROUPS:
        items = []
        for n in names:
            var, kind, short = bc.OPTIONS[n]
            hk, lines = model.help.get(n, ("", []))
            items.append(Item(("option", n), short, lines or [short]))
        groups.append((title, items))
    drv = [Item(("driver", name), f"{desc} ({cls})",
                [desc + ".", f"Class {cls}, {path}.",
                 "builtin links it into kernel.bin; module builds /lib/modules/"
                 f"{name}.ko, loaded when a matching device is present. Only a",
                 "self-contained driver can be a module -- the build names a missing export."])
           for name, cls, desc, path in model.drivers]
    groups.insert(3, ("Drivers", drv))
    profs = []
    for name in bc.profiles():
        try:
            desc, sets = profile_info(name)
        except (SystemExit, OSError) as e:
            profs.append(Item(("broken", name), f"BROKEN: {e}",
                              [f"configs/{name}.conf cannot be read: {e}",
                               "Fix the file by hand; menuconfig cannot load it."]))
            continue
        what = ", ".join(f"{n} = {v}" for _k, n, v in sets)
        publish = ("update_server.py --publish accepts a build made with it."
                   if name == "release" else "A build made with it is never published.")
        profs.append(Item(("profile", name), what or "(sets nothing)",
                          [desc, f"Sets: {what or 'nothing'}.",
                           f"Enter loads it into this session; s or S then saves it as usual. "
                           f"From the command line: make iso PROFILE={name} (build.conf, then "
                           f"configs/{name}.conf; build.local.conf is not read).", publish]))
    groups.append(("Profiles", profs))
    return groups


def cycle(model, item):
    kind, name = item.key
    cur = model.value(item.key)
    if kind == "driver":
        return "module" if cur == "builtin" else "builtin"
    k = bc.OPTIONS[name][1]
    if k == "bool":
        return "no" if cur == "yes" else "yes"
    if isinstance(k, dict):
        k = tuple(k)
    if isinstance(k, tuple):
        i = k.index(cur) if cur in k else -1
        return k[(i + 1) % len(k)]
    return None   # a number or text: Enter edits it


def toggle_word(ui, row, words, typed=False):
    """`words` with `row`'s word switched: a plain word on or off, a
    choice stepped to the next value (then off), a typed one asked for --
    Enter asks for a choice's value too."""
    on = [wd for wd in words if row.matches(wd)]
    rest = [wd for wd in words if not row.matches(wd)]
    if row.typed or (typed and row.key):
        hint = row.forms[0] if row.typed else " / ".join(row.choices)
        cur = on[0] if on else (row.key + "=" if row.key else row.forms[0])
        new = ui.prompt(f"{hint}: ", cur)
        if new is None:
            return words
        new = new.strip()
        return rest + ([new] if new and new != (row.key or "") + "=" else [])
    if len(row.choices) > 1:
        i = row.choices.index(on[0]) if on and on[0] in row.choices else -1
        return rest + ([row.choices[i + 1]] if i + 1 < len(row.choices) else [])
    return rest if on else words + [row.forms[0]]


class UI:
    def __init__(self, scr, model):
        self.scr, self.m = scr, model
        self.groups = build_groups(model)
        self.g, self.i, self.top = 0, 0, 0
        self.right = False
        self.msg = ""
        curses.curs_set(0)
        self.color = curses.has_colors()
        if self.color:
            curses.start_color()
            try:
                curses.use_default_colors()
                bg = -1
            except curses.error:
                bg = curses.COLOR_BLACK
            curses.init_pair(1, curses.COLOR_WHITE, curses.COLOR_BLUE)
            curses.init_pair(2, curses.COLOR_YELLOW, bg)
            curses.init_pair(3, curses.COLOR_CYAN, bg)

    def attr(self, n, fallback=curses.A_REVERSE):
        return curses.color_pair(n) if self.color else fallback

    def items(self):
        return self.groups[self.g][1]

    def put(self, y, x, text, a=0, width=None):
        h, w = self.scr.getmaxyx()
        if y < 0 or y >= h or x >= w:
            return
        room = w - x - (1 if y == h - 1 else 0)
        if width is not None:
            text = text[:width].ljust(width)
        try:
            self.scr.addstr(y, x, text[:max(room, 0)], a)
        except curses.error:
            pass

    def draw(self):
        s = self.scr
        s.erase()
        h, w = s.getmaxyx()
        if h < 18 or w < 72:
            self.put(0, 0, "make menuconfig needs a terminal of at least 72x18")
            s.refresh()
            return
        unsaved = len(self.m.pending)
        local = len([c for c in self.m.changes() if c[3] == "local"])
        self.put(0, 1, f"SaunaOS build configuration -- {os.path.basename(self.m.base)}", curses.A_BOLD)
        state = []
        if self.m.loaded_profile and unsaved:
            state.append(f"profile {self.m.loaded_profile} loaded")
        if unsaved:
            state.append(f"{unsaved} change{'s' * (unsaved != 1)} not saved")
        if local:
            state.append(f"{local} local override{'s' * (local != 1)}")
        right = " * ".join(state) or "as build.conf says"
        self.put(0, w - len(right) - 2, right, self.attr(2, curses.A_BOLD) if state else 0)

        help_h = 8
        pane_h = h - help_h - 3
        lw = 24
        self.box(1, 0, pane_h, lw, not self.right)
        self.box(1, lw + 1, pane_h, w - lw - 1, self.right)
        for gi, (title, items) in enumerate(self.groups):
            n = len(items)
            sel = gi == self.g
            a = self.attr(1) if sel and not self.right else (curses.A_BOLD if sel else 0)
            self.put(2 + gi, 1, f" {title:<15}{n:>5} ", a, lw - 2)

        items = self.items()
        rows = pane_h - 2
        if self.i < self.top:
            self.top = self.i
        if self.i >= self.top + rows:
            self.top = self.i - rows + 1
        vw = 9
        nw = 16
        for row, item in enumerate(items[self.top:self.top + rows]):
            idx = self.top + row
            key = item.key
            inner = w - lw - 4
            if key[0] in ("profile", "broken"):
                where = "profile"
                text = f" {key[1]:<{nw}} " + fit(item.label, inner - nw - 3)
            else:
                val = self.m.value(key)
                where = self.m.where(key)
                mark = {"unsaved": "*", "local": "L"}.get(where, " ")
                shown = fit("not set" if val is None else val, 28)
                extra = ""
                if key[0] == "driver":
                    need, _obj = self.m.module_check(key[1])
                    extra = "  " + ("?" if need is None else "yes" if not need else f"needs {len(need)}").ljust(9)
                width = max(vw, len(shown))
                room = inner - width - 3 - len(extra)
                line = f" {key[1]:<{nw}} " + fit(item.label, room - nw - 2)
                text = line.ljust(room) + f"{shown:>{width}}{extra} {mark}"
            a = self.attr(1) if idx == self.i and self.right else 0
            if where in ("unsaved", "local") and not (idx == self.i and self.right):
                a = self.attr(2, curses.A_BOLD)
            self.put(2 + row, lw + 2, text, a, inner)

        self.draw_help(1 + pane_h, w, help_h)
        keys = "Tab pane  Space toggle  Enter edit  / search  d changes  s save local  S build.conf  w profile  q quit"
        self.put(h - 1, 1, self.msg or keys, self.attr(2, curses.A_BOLD) if self.msg else self.attr(3, 0))
        s.refresh()

    def box(self, y, x, hh, ww, focused):
        a = self.attr(3, curses.A_BOLD) if focused else 0
        try:
            win = self.scr.derwin(hh, ww, y, x)
            win.attrset(a)
            win.box()
        except curses.error:
            pass

    def draw_help(self, y, w, hh):
        items = self.items()
        if not items:
            return
        item = items[self.i]
        kind, name = item.key
        self.box(y, 0, hh, w, False)
        self.put(y + 1, 2, f"{name} -- {item.label}", curses.A_BOLD, w - 4)
        lines = textwrap.wrap(" ".join(item.help), max(20, w - 4))
        room = hh - 4 - (2 if kind == "driver" else 0)   # a driver's module verdict below
        for k, ln in enumerate(lines[:room]):
            self.put(y + 2 + k, 2, ln, 0, w - 4)
        if kind in ("profile", "broken"):
            return
        base = self.m.base_vals.get(item.key, "builtin" if kind == "driver" else None)
        if kind == "driver":
            need, obj = self.m.module_check(name)
            rel = os.path.relpath(obj, REPO)
            if need is None:
                why = f"Not built yet ({rel}): run make, and this says whether it can be a module."
            elif not need:
                why = f"Can be a module: every function it calls is exported (checked against {rel})."
            else:
                why = (f"Cannot be a module yet: it calls {len(need)} function(s) kernel/core/kexports.c "
                       f"does not export: {', '.join(need[:6])}{' ...' if len(need) > 6 else ''}.")
            for k, ln in enumerate(textwrap.wrap(why, max(20, w - 4))[:2]):
                self.put(y + hh - 4 + k, 2, ln, self.attr(2, curses.A_BOLD) if need else 0, w - 4)
        if kind == "option":
            var = bc.OPTIONS[name][0]
            val = self.m.value(item.key)
            made = bc.option_value("x", 0, name, val)[1] if val is not None and not self.m.check(item.key, val) else "its Makefile default"
            tail = f"make: {var} ?= {made}   build.conf: {base or 'not set'}"
            if name in ("ubsan", "kasan", "debug_preempt", "lockdep"):
                tail += "   changing it rebuilds the tree"
        else:
            tail = f"build.conf: {base}"
        where = self.m.where(item.key)
        if where == "local":
            tail += "   (build.local.conf overrides it)"
        self.put(y + hh - 2, 2, tail, self.attr(3, 0), w - 4)

    def prompt(self, label, initial=""):
        """A one-line editor on the bottom row: Enter accepts, Esc cancels."""
        h, w = self.scr.getmaxyx()
        buf = list(initial)
        curses.curs_set(1)
        try:
            while True:
                text = label + "".join(buf)
                self.put(h - 1, 1, text, 0, w - 2)
                self.scr.move(h - 1, min(1 + len(text), w - 2))
                self.scr.refresh()
                c = self.scr.get_wch()
                if c in ("\n", "\r", curses.KEY_ENTER):
                    return "".join(buf)
                if c == "\x1b":
                    return None
                if c in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                    if buf:
                        buf.pop()
                elif isinstance(c, str) and c.isprintable():
                    buf.append(c)
        finally:
            curses.curs_set(0)

    def ask(self, question):
        self.msg = question + " (y/n)"
        self.draw()
        self.msg = ""
        c = self.scr.get_wch()
        return c in ("y", "Y")

    def pick_boot_words(self, current):
        """The cmdline as a checklist of docs/boot-flags.md's words. Returns
        the new line, or None for no change. A word the table does not
        know is kept as it is."""
        rows = boot_words()
        words = current.split()
        sel, top, filt = 0, 0, ""
        while True:
            shown = [r for r in rows if not filt or filt in " ".join(r.forms).lower()
                     or filt in r.desc.lower()]
            sel = min(sel, max(0, len(shown) - 1))
            h, w = self.scr.getmaxyx()
            self.scr.erase()
            self.put(0, 1, f"cmdline -- boot words (docs/boot-flags.md, {len(rows)})", curses.A_BOLD)
            if filt:
                self.put(0, w - len(filt) - 12, f"/ filter: {filt}", self.attr(3, 0))
            body = h - 6
            if sel < top:
                top = sel
            if sel >= top + body:
                top = sel - body + 1
            for k, r in enumerate(shown[top:top + body]):
                on = [wd for wd in words if r.matches(wd)]
                if len(r.choices) > 1 and not r.typed:
                    box = f"({' / '.join(c.split('=', 1)[1] for c in r.choices)}) "
                    state = on[0] if on else ""
                    form = f"{state or r.key + '=...'}"
                    tick = "(o)" if on else "( )"
                else:
                    box = ""
                    form = on[0] if on else r.forms[0]
                    tick = "[x]" if on else "[ ]"
                text = f" {tick} {fit(form, 30):<30} {fit(box + r.desc, w - 40)}"
                a = self.attr(1) if top + k == sel else 0
                self.put(2 + k, 1, text, a, w - 2)
            self.put(h - 4, 1, "cmdline would be:", self.attr(3, 0))
            self.put(h - 3, 1, " ".join(words) or "(empty)", self.attr(2, curses.A_BOLD))
            self.put(h - 1, 1, "Space toggles or picks  Enter types a value  / filter  e edit the line  "
                     "Esc done  q cancel", self.attr(3, 0))
            self.scr.refresh()
            c = self.scr.get_wch()
            if c == curses.KEY_RESIZE:
                continue
            if c in (curses.KEY_UP, "k"):
                sel = max(0, sel - 1)
            elif c in (curses.KEY_DOWN, "j"):
                sel = min(len(shown) - 1, sel + 1)
            elif c in (curses.KEY_NPAGE, curses.KEY_PPAGE):
                sel = max(0, min(len(shown) - 1, sel + (body if c == curses.KEY_NPAGE else -body)))
            elif c == "/":
                filt = (self.prompt("/") or "").lower()
                sel = top = 0
            elif c == "e":
                new = self.prompt("cmdline = ", " ".join(words))
                if new is not None:
                    words = new.split()
            elif c == "\x1b":
                line = " ".join(words)
                return line if line != current.strip() else None
            elif c == "q":
                return None
            elif c in (" ", "\n", "\r", curses.KEY_ENTER) and shown:
                words = toggle_word(self, shown[sel], words, typed=c != " ")

    def search(self):
        q = self.prompt("/")
        if not q:
            return
        q = q.lower()
        flat = [(gi, ii) for gi, (_t, its) in enumerate(self.groups) for ii in range(len(its))]
        start = flat.index((self.g, self.i)) if (self.g, self.i) in flat else 0
        for off in range(1, len(flat) + 1):
            gi, ii = flat[(start + off) % len(flat)]
            it = self.groups[gi][1][ii]
            if q in it.key[1].lower() or q in it.label.lower() or any(q in ln.lower() for ln in it.help):
                self.g, self.i, self.right = gi, ii, True
                return
        self.msg = f"nothing matches {q!r}"

    def show_changes(self):
        ch = self.m.changes()
        s = self.scr
        s.erase()
        self.put(0, 1, "Changes from build.conf", curses.A_BOLD)
        if not ch:
            self.put(2, 2, "none -- this checkout builds as build.conf says")
        for row, (key, was, now, where) in enumerate(ch):
            tag = "not saved" if where == "unsaved" else "build.local.conf"
            self.put(2 + row, 2, f"{key[1]:<16} {was or 'not set'} -> {now}   ({tag})")
        h, _w = s.getmaxyx()
        self.put(h - 1, 1, "any key to go back", self.attr(3, 0))
        s.refresh()
        s.get_wch()

    def act(self, c):
        items = self.items()
        if c in (curses.KEY_UP, "k"):
            if self.right:
                self.i = max(0, self.i - 1)
            else:
                self.g = max(0, self.g - 1)
                self.i = self.top = 0
        elif c in (curses.KEY_DOWN, "j"):
            if self.right:
                self.i = min(len(items) - 1, self.i + 1)
            else:
                self.g = min(len(self.groups) - 1, self.g + 1)
                self.i = self.top = 0
        elif c == curses.KEY_RIGHT:
            self.right = True
        elif c == curses.KEY_LEFT:
            self.right = False
        elif c in ("\t", curses.KEY_BTAB):
            self.right = not self.right
        elif c in (curses.KEY_NPAGE, curses.KEY_PPAGE) and self.right:
            step = max(1, self.scr.getmaxyx()[0] - 12)
            self.i = max(0, min(len(items) - 1, self.i + (step if c == curses.KEY_NPAGE else -step)))
        elif c in (" ", "\n", "\r", curses.KEY_ENTER):
            if not self.right:
                self.right = True
                return True
            if not items:
                return True
            item = items[self.i]
            if item.key[0] == "broken":
                self.msg = f"configs/{item.key[1]}.conf is broken -- fix it by hand"
                return True
            if item.key[0] == "profile":
                why = self.m.load_profile(item.key[1])
                self.msg = why or (f"profile {item.key[1]} loaded -- s saves it to "
                                   f"{os.path.basename(self.m.local)}, S into build.conf")
                return True
            if item.key == ("option", "cmdline"):
                new = self.pick_boot_words(self.m.value(item.key) or "")
                if new is None:
                    return True
            else:
                new = cycle(self.m, item)   # None: a number or text, typed in
            if item.key[0] == "driver" and new == "module":
                need, _obj = self.m.module_check(item.key[1])
                if need:
                    self.msg = f"{item.key[1]} needs {len(need)} export(s) to be a module -- see below"
                    return True
            if new is None:
                new = self.prompt(f"{item.key[1]} = ", self.m.value(item.key) or "")
                if new is None:
                    return True
            why = self.m.set(item.key, new)
            if why:
                self.msg = f"{item.key[1]}: {why}"
        elif c == "w":
            if not self.m.changes():
                self.msg = "no change from build.conf to write as a profile"
                return True
            name = self.prompt("new profile name (configs/NAME.conf): ")
            if not name:
                return True
            if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", name):
                self.msg = "a profile name is lower case letters, digits, - and _"
                return True
            if name in PUBLISHED_PROFILES:
                self.msg = f"{name} is the published profile: edit configs/{name}.conf by hand and commit it"
                return True
            path = bc.profile_path(name)
            if os.path.exists(path) and not self.ask(f"configs/{name}.conf exists -- replace it?"):
                return True
            desc = self.prompt("one line saying what it is for: ") or f"The {name} profile."
            why = self.m.write_profile(name, desc)
            self.groups = build_groups(self.m)
            self.msg = why or f"wrote configs/{name}.conf -- make iso PROFILE={name}; it is tracked once added"
        elif c == "/":
            self.search()
        elif c == "d":
            self.show_changes()
        elif c == "s":
            if not self.m.pending:
                self.msg = "nothing to save"
            else:
                why = self.m.save_local()
                self.msg = why or f"saved to {os.path.basename(self.m.local)} (untracked)"
        elif c == "S":
            n = len(self.m.changes())
            if not n and not self.m.local_vals and not self.m.pending:
                self.msg = "build.conf already says all of it"
            elif not n:
                # Every local override was set back to build.conf's value:
                # S clears them, writing nothing into build.conf.
                why = self.m.promote()
                self.msg = why or "build.conf already says all of it; build.local.conf removed"
            elif self.ask(f"Write {n} change{'s' * (n != 1)} into build.conf, the TRACKED file every checkout builds from?"):
                why = self.m.promote()
                self.msg = why or "written into build.conf -- it shows in git diff; build.local.conf removed"
        elif c in ("q", "Q", "\x1b"):
            if self.m.pending and not self.ask(f"Quit without saving {len(self.m.pending)} change(s)?"):
                return True
            return False
        return True

    def run(self):
        while True:
            self.draw()
            c = self.scr.get_wch()
            if c == curses.KEY_RESIZE:
                continue
            self.msg = ""
            if not self.act(c):
                return


def main():
    ap = argparse.ArgumentParser(description="build.conf's options and drivers in a text UI")
    ap.add_argument("conf", nargs="?", default=os.path.join(REPO, "build.conf"))
    ap.add_argument("--local", default=None, help="the untracked overrides file (default: build.local.conf beside conf)")
    ap.add_argument("--build", default=None, help="the build directory the module check reads (default: build)")
    args = ap.parse_args()
    global BUILD_DIR
    if args.build:
        BUILD_DIR = os.path.abspath(args.build)
    local = args.local or os.path.join(os.path.dirname(os.path.abspath(args.conf)), "build.local.conf")
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        sys.exit("menuconfig: needs a terminal")
    try:
        model = Model(args.conf, local)
    except SystemExit as e:
        # A broken build.local.conf stops every build; the tool that
        # writes it offers to set it aside rather than refuse to start.
        bc.load(args.conf)   # build.conf itself broken: exits with its message
        if not os.path.exists(local):
            raise
        print(f"menuconfig: {e}")
        aside = local + ".broken"
        if input(f"Set {os.path.basename(local)} aside as {os.path.basename(aside)} "
                 "and start from build.conf? [y/N] ").strip().lower() != "y":
            return 1
        os.replace(local, aside)
        model = Model(args.conf, local)
    os.environ.setdefault("ESCDELAY", "25")
    curses.wrapper(lambda scr: UI(scr, model).run())
    ch = [c for c in model.changes() if c[3] == "local"]   # an unsaved one was thrown away
    if ch:
        print(f"menuconfig: {len(ch)} change(s) from build.conf in {os.path.basename(local)}:")
        for key, was, now, _w in ch:
            print(f"  {key[1]}: {was or 'not set'} -> {now}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
