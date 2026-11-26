#!/usr/bin/env python3
"""The host widget harness: every tools/hostchecks/uitest/*_uitest.c, built
against the REAL toolkit with the host gcc and run in seconds, no QEMU
(docs/gui-unittest-design.md, stage 1).

WHAT IT BUILDS. Every .c in userland/libs/{base,net,media,ui}/ and the
kernel/lib sources ring 3 shares (read from the Makefile's
`$(BUILD)/userland/shared/NAME.o` list, so a new one is picked up with no
edit here), compiled against SaunaOS's own libc headers as the guest
compiles them, into one archive under build/hostui/. Objects are rebuilt
when their source or any header they included (gcc -MMD) is newer, so a
second run compiles nothing. Each case links the harness
(tools/lib/uitest/) and the archive with --gc-sections, which keeps only
what the case reaches.

WHAT IS STUBBED is the platform, not the toolkit: the sys_* calls, in
tools/lib/uitest/uitest_sys.c. A widget reaching a call nobody stubbed
fails to LINK, naming it. The C library underneath is the host's glibc,
and that is the trap this tool guards: the toolkit is compiled against
TOLIBC's headers, so any glibc call taking a struct both libcs define
differently (stat, readdir, struct tm) would be handed the wrong layout
and corrupt memory quietly. After each link the binary's glibc imports
are compared against ABI_UNSAFE; one of those is a refusal, and the fix
is a stub in uitest_sys.c in tolibc's own layout.

THE ROOT. A guest path resolves under build/hostui/root/ (UITEST_ROOT):
/usr/share/fonts is the tree's data/fonts, /usr/share/ui every program's
.ui file (as the Makefile stages them), the rest of /usr/share the
tree's data/usr/share, /tmp is empty, and there is no
/etc -- so no setting exists unless a case declares it
(uitest_setting, or a declaration written with uitest_file()) and the
host's own files never answer. /etc and /home are EACH CASE'S OWN
(root/private.<pid>/), since cases run in parallel over one root.

`--positive-control` rebuilds the archive with each control's edit
applied to a COPY of its source, and requires every case the control
names to go red while the others stay green. `--keep DIR` writes each
case's uitest_keep() frames there as PPMs. `--list` names the cases;
positional names run a subset. Needs gcc.

A C++ APP (`uitest-app:` naming a directory of .cpp sources, the uixx
layer's examples) is built with the host g++ against glibc and libstdc++,
not tolibc, and linked with g++; its `main()` takes no arguments. Without
g++ on PATH that case fails to compile, naming it.
"""

# The toolkit's widgets, driven by name through the real router on the
# host: every tools/hostchecks/uitest/*_uitest.c against a host build of
# userland/libs/*. gcc and glibc. Carries a --positive-control that must
# go red.
RUNNER = {"name": "uitest_host", "covers": "widgets by name through the real router, on the host (tools/hostchecks/uitest/)",
          "parallel": True, "gate": True}

import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
ROOT = toolpath.REPO

BUILD = os.path.join(ROOT, "build", "hostui")
HARNESS = os.path.join(ROOT, "tools", "lib", "uitest")
CASES = os.path.join(ROOT, "tools", "hostchecks", "uitest")
LIBS = ("base", "net", "media", "ui")
# Shared kernel/lib sources that are NOT the toolkit's on the host: the
# allocator (glibc's malloc is the one in use) and UBSan's runtime.
SHARED_SKIP = {"heap_core", "ubsan", "ubsan_cases"}

# ccache in front of gcc when it is installed, as the Makefile does: a
# preflight starts from `make clean`, which takes build/hostui/ with it,
# and ccache turns that cold rebuild of every toolkit source into lookups.
GCC = (["ccache"] if shutil.which("ccache") else []) + ["gcc"]
GXX = (["ccache"] if shutil.which("ccache") else []) + ["g++"]

INC_LIBC = ["-Iuserland/libs/libc/include"]
INC_API = ["-Ikernel/include/api", "-Ikernel/include/abi", "-Ibuild/gen", "-Iuserland", "-Iuserland/libs"]
# -fno-asynchronous-unwind-tables as on the guest: a C++ exception that
# unwinds into the toolkit must terminate here as it does there, or a case
# could not see the C++ layer's guard missing (throwxx_app).
TOOLKIT_FLAGS = ["-std=gnu11", "-ffreestanding", "-O2", "-g", "-w", "-fno-strict-aliasing",
                 "-ffunction-sections", "-fdata-sections", "-fno-tree-loop-distribute-patterns",
                 "-fno-asynchronous-unwind-tables"]
EXTRA = {   # per-source flags the Makefile adds
    "userland/libs/media/uvid_indeo3.c": ["-Iuserland/third_party/indeo3/upstream",
                                          "-Iuserland/third_party/indeo3/glue"],
}
# The guest's warnings (USERLAND_CFLAGS), made errors -- less the two the
# guest's positional initialisers and utest.h's bounded lines trip.
CASE_FLAGS = ["-std=gnu11", "-ffreestanding", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
              "-Wno-missing-field-initializers", "-Wno-format-truncation"]
# A C++ app (the uixx layer's, docs/uixx-design.md) is compiled HOSTED,
# against glibc and the host's libstdc++: tolibc's headers cannot sit under
# libstdc++'s, and the kernel's come AFTER the system's, since abi/ has an
# errno.h of its own. Only the toolkit's headers are shared, and they lay
# out nothing a libc defines.
CXX_FLAGS = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Iuserland", "-Iuserland/libs",
             "-idirafter", "kernel/include/api", "-idirafter", "kernel/include/abi", "-idirafter", "build/gen"]

# glibc calls whose argument or result is a struct tolibc lays out
# differently (or whose state the toolkit reads through tolibc's macros).
# A binary importing one of these from glibc is REFUSED: give the call a
# stub in uitest_sys.c in tolibc's layout instead.
ABI_UNSAFE = {
    "stat", "fstat", "lstat", "fstatat", "stat64", "fstat64", "lstat64", "statx",
    "opendir", "readdir", "readdir64", "closedir", "scandir", "fdopendir",
    "localtime", "localtime_r", "gmtime", "gmtime_r", "mktime", "timegm", "strftime",
    "localeconv", "nl_langinfo", "setlocale", "newlocale", "uselocale",
    "sigaction", "signal", "getdents64",
    "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_destroy",
    "pthread_cond_wait", "pthread_cond_signal", "pthread_once", "pthread_attr_init",
    "pthread_create", "pthread_join",
    # Not a struct but the same trap: tolibc's O_* values are SYS_O_*'s,
    # not Linux's, and a path glibc is handed is never put under the root,
    # so the HOST's own files would answer (or be unlinked).
    "open", "open64", "openat", "creat", "access", "unlink", "unlinkat", "rename", "renameat",
    "remove", "rmdir", "mkdir", "truncate",
    # The account databases: glibc's read the HOST's /etc/passwd through
    # NSS, never the case's own (uitest_file()) -- tolibc's pwd.c and grp.c
    # read it with the wrapped fopen.
    "getpwnam", "getpwuid", "getpwent", "setpwent", "endpwent", "getpwnam_r", "getpwuid_r",
    "getgrnam", "getgrgid", "getgrent", "setgrent", "endgrent", "getgrnam_r", "getgrgid_r",
    "getgrouplist",
}

# tolibc's OWN implementation of a call in ABI_UNSAFE, compiled into the
# harness so it sits on uitest_sys.c's stubs in tolibc's own layout.
# printf_float.c is not an ABI matter: it is kfmt's %f, which only ring 3's
# libc carries (the kernel has no FPU state to spare).
# AN ARCHIVE, ahead of glibc on the link line: a member comes in only when
# something reaches it, and then wins over glibc's symbol of that name.
LIBC_OWN = ["userland/libs/libc/stat.c", "userland/libs/libc/time.c", "userland/libs/libc/tz.c",
            "userland/libs/libc/locale.c", "userland/libs/libc/printf_float.c",
            "userland/libs/libc/access.c", "userland/libs/libc/pthread.c",
            "userland/libs/libc/pwd.c", "userland/libs/libc/grp.c", "userland/libs/libc/etcdb.c"]

# Each control: (source, before, after, cases that must go red). The edit
# is applied to a COPY; the checked-in file is never touched.
CONTROLS = [
    # uui_sbar hands the painter a TOP-based position, unconverted: the
    # mirrored thumb rule 10 (userland/CLAUDE.md) is about. The scroll
    # case's table and hex grid embed the bar, so they go red with it.
    ("userland/libs/ui/uui_sbar.c",
     "return b->horiz ? b->top : max_top(b) - b->top;",
     "return b->top;",
     ["sbar", "scroll"]),
    # uui_grid's painter handed its from-the-top scroll raw, as three
    # widgets once did: the Retro Launcher's mirrored thumb.
    ("userland/libs/ui/uui_grid.c",
     "uui_scrollbar_flip(content_h(g), g->h, g->scroll), uui_grid_c_bg(g)",
     "g->scroll, uui_grid_c_bg(g)",
     ["scroll"]),
    # Esc in an open dropdown keeps what the arrows moved to, instead of
    # the choice the list opened with: the bug this harness found first.
    ("userland/libs/ui/uui_dropdown.c",
     "    if (d->opened_with >= -1 && d->opened_with < d->list.count) d->list.selected = d->opened_with;",
     "",
     ["settings_rows"]),
    # The scroll view's hit answering everywhere: it no longer clips its
    # children, so the clipped half of a straddling control takes a click.
    ("userland/libs/ui/uui_scrollview.c",
     "    return cx >= sv->x && cx < sv->x + sv->w && cy >= sv->y && cy < sv->y + sv->h;",
     "    (void)sv; (void)cx; (void)cy; return 1;",
     ["settings_rows"]),
    # A button that commits on a release dragged OFF it -- the rule every
    # control keeps; Calculator's own main must show the 9 it should not,
    # and the C++ Counter its count of 1.
    ("userland/libs/ui/uui_button.c",
     "    int was = b->pressed;\n    b->pressed = 0;",
     "    int was = 1;\n    b->pressed = 0;",
     ["calculator_app", "counterxx_app"]),
    # A plain button's face filled square again, as it was beside the
    # rounded outlined one (System Settings' square buttons).
    ("userland/libs/ui/uui_primitives.c",
     "    uui_fill_round_rect(s, x, y, w, h, uui_control_radius(0), fill);\n    uui_button_draw_label",
     "    ugfx_fill_rect(s, x, y, w, h, fill);\n    uui_button_draw_label",
     ["button_corners"]),
    # Where a value came from, blind to the person's file: the editor's
    # From column says Machine for a value of the person's own.
    ("userland/libs/base/usetting_schema.c",
     "    return o->has_user ? USCHEMA_FROM_USER :",
     "    return 0 ? USCHEMA_FROM_USER :",
     ["confedit_app"]),
    # The tree's chevron back in the muted grey, its hover chip gone, and
    # a double-click on a row that toggles nothing -- the three picks of
    # 2026-11-25 for every uui_tree.
    ("userland/libs/ui/uui_tree.c",
     "selected && strong ? sel_fg : ugfx_blend(fg, bg, 40));",
     "selected && strong ? sel_fg : ugfx_blend(fg, bg, 120));",
     ["tree_expander"]),
    ("userland/libs/ui/uui_tree.c",
     "            if (node == t->exp_hot) {",
     "            if (0 && node == t->exp_hot) {",
     ["tree_expander"]),
    ("userland/libs/ui/uui_tree.c",
     "    if (!t->no_dbl_toggle && n >= 0 && !on_exp",
     "    if (0 && !t->no_dbl_toggle && n >= 0 && !on_exp",
     ["tree_expander", "confedit_app"]),
    # uui_chart's legend measured on every ask again: n squared text
    # measurements a frame at sixteen lines (the code review's finding).
    ("userland/libs/ui/uui_chart.c",
     "    if (!(k->valid && k->w == c->w",
     "    if (1 || !(k->valid && k->w == c->w",
     ["chart_lines"]),
    # A glide that asks for a redraw on every call, not on a whole pixel.
    ("userland/libs/ui/uui_chart.c",
     "    if (px == c->shift) return 0;\n    c->shift = px;",
     "    c->shift = px;",
     ["chart_lines"]),
    # Gliding to the NEWEST sample: the right edge extrapolated, not one behind.
    ("userland/libs/ui/uui_chart.c",
     "    p->behind = c->smooth && lines_on(c)",
     "    p->behind = 0 && c->smooth && lines_on(c)",
     ["chart_lines"]),
    # Off no longer stepped: the chart glides whatever desktop.animations says.
    ("userland/libs/ui/uui_chart.c",
     "    return c->smooth && !c->fit && c->sample_ms > 0 && c->newest_ns;",
     "    return !c->fit && c->sample_ms > 0 && c->newest_ns;",
     ["chart_lines"]),
    # Lines mode counting the undrawn main series as drawn: hover_index() +
    # sample() then name a column the pointer is not over.
    ("userland/libs/ui/uui_chart.c",
     "    if (lines_on(c)) return 0;   // the main series is filled, never drawn\n",
     "",
     ["chart_lines"]),
    # Minesweeper records a flag and never draws it -- "it responds" is
    # not "it is drawn" (a spared-3x3 control would not do: the fixed
    # board opens nine cells either way, the ~88% the GUI tool notes).
    ("userland/apps/apps/mines/mines.c",
     "        draw_flag(s, x, y + dy, c);",
     "        (void)dy;",
     ["mines_app"]),
    # The file view's paths cut at the 256 bytes it once held: a folder
    # past that descends to, and hands out, a path that names another.
    ("userland/libs/ui/uui_fileview.c",
     "    return upath_join(fv_dir(fv), e->name);\n",
     "    char *p = upath_join(fv_dir(fv), e->name);\n    if (p && strlen(p) > 255) p[255] = '\\0';\n    return p;\n",
     ["deeppath"]),
    # The path bar's old cap on the segments it lays out: a folder deeper
    # than its slots showed a folder ABOVE the one you are in, as the last.
    ("userland/libs/ui/uui_pathbar.c",
     "    int n = seg_count(path_of(p));\n    int avail",
     "    int n = seg_count(path_of(p));\n    if (n > UUI_PATHBAR_SEGS - 1) n = UUI_PATHBAR_SEGS - 1;\n    int avail",
     ["deeppath"]),
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def shared_names():
    with open(os.path.join(ROOT, "Makefile")) as fh:
        body = fh.read()
    names = sorted(set(re.findall(r"\$\(BUILD\)/userland/shared/([A-Za-z0-9_]+)\.o", body)))
    return [n for n in names if n not in SHARED_SKIP and os.path.exists(os.path.join(ROOT, "kernel/lib", n + ".c"))]


def toolkit_sources():
    srcs = []
    for lib in LIBS:
        srcs += sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, "userland/libs", lib, "**/*.c"), recursive=True))
    srcs += ["kernel/lib/%s.c" % n for n in shared_names()]
    return srcs


def obj_name(src):
    return src.replace("/", "__")[:-2] + ".o"


def stale(obj, src):
    """Is obj older than src or than any header its .d names?"""
    if not os.path.exists(obj):
        return True
    t = os.path.getmtime(obj)
    if os.path.getmtime(os.path.join(ROOT, src)) > t:
        return True
    dep = obj[:-2] + ".d"
    if not os.path.exists(dep):
        return True
    with open(dep) as fh:
        text = fh.read().replace("\\\n", " ")
    for path in text.split(":", 1)[-1].split():
        p = path if os.path.isabs(path) else os.path.join(ROOT, path)
        if not os.path.exists(p) or os.path.getmtime(p) > t:
            return True
    return False


def compile_one(src, obj, staged=None):
    shared = src.startswith("kernel/lib/")
    flags = TOOLKIT_FLAGS + ([] if shared else INC_LIBC) + INC_API + EXTRA.get(src, [])
    if staged:   # a sibling `#include "x.h"` is looked for beside the COPY otherwise
        flags = flags + ["-I" + os.path.dirname(src)]
    r = run([*GCC, *flags, "-MMD", "-MF", obj[:-2] + ".d", "-c", "-o", obj, staged or src])
    return src, r.returncode, r.stderr


def flags_stamp():
    h = hashlib.sha256(repr((TOOLKIT_FLAGS, INC_LIBC, INC_API, EXTRA)).encode()).hexdigest()
    return h[:16]


def build_toolkit(jobs):
    """Every toolkit object up to date, and the archive over them."""
    objdir = os.path.join(BUILD, "obj")
    stamp = os.path.join(BUILD, "flags")
    if not os.path.exists(stamp) or open(stamp).read() != flags_stamp():
        shutil.rmtree(objdir, ignore_errors=True)   # a CFLAGS change is not dependency-tracked
    os.makedirs(objdir, exist_ok=True)
    with open(stamp, "w") as fh:
        fh.write(flags_stamp())
    srcs = toolkit_sources()
    todo = [(s, os.path.join(objdir, obj_name(s))) for s in srcs]
    todo = [(s, o) for s, o in todo if stale(o, s)]
    failed = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for src, rc, err in ex.map(lambda so: compile_one(*so), todo):
            if rc:
                failed.append((src, err))
    if failed:
        for src, err in failed:
            print(f"uitest_host: host compile of {src} failed:\n{err}", file=sys.stderr)
        sys.exit(1)
    lib = os.path.join(BUILD, "libhostui.a")
    objs = [os.path.join(objdir, obj_name(s)) for s in srcs]
    # A source deleted or renamed recompiles nothing, but its member must go.
    members = os.path.join(BUILD, "members")
    listed = "\n".join(objs)
    same = os.path.exists(members) and open(members).read() == listed
    if todo or not same or not os.path.exists(lib):
        if os.path.exists(lib):
            os.unlink(lib)
        r = run(["ar", "rcs", lib, *objs])
        if r.returncode:
            sys.exit(f"uitest_host: ar failed:\n{r.stderr}")
        with open(members, "w") as fh:
            fh.write(listed)
    return lib, len(todo), len(srcs)


def control_archive(lib, control):
    """(archive, override): a copy of the archive with one source rebuilt
    from an edited copy -- or, for an app's own source (userland/apps/),
    the archive as it is and {source: edited copy} for build_app()."""
    src, before, after, _ = control
    pcdir = os.path.join(BUILD, "control")
    os.makedirs(pcdir, exist_ok=True)
    body = open(os.path.join(ROOT, src)).read()
    if before not in body:
        sys.exit(f"uitest_host: the positive control cannot find {before!r} in {src} -- the code moved, fix the control")
    staged = os.path.join(pcdir, os.path.basename(src))
    with open(staged, "w") as fh:
        fh.write(body.replace(before, after, 1))
    if src.startswith("userland/apps/"):
        return lib, {os.path.join(ROOT, src): staged}
    obj = os.path.join(pcdir, obj_name(src))
    # The copy sits elsewhere, so its own directory's headers must still be found.
    _, rc, err = compile_one(src, obj, staged=staged)
    if rc:
        sys.exit(f"uitest_host: compile of the control's {src} failed:\n{err}")
    pclib = os.path.join(pcdir, "libhostui.a")
    shutil.copy(lib, pclib)
    r = run(["ar", "r", pclib, obj])
    if r.returncode:
        sys.exit(f"uitest_host: ar failed:\n{r.stderr}")
    return pclib, {}


def build_harness():
    hdir = os.path.join(BUILD, "harness")
    os.makedirs(hdir, exist_ok=True)
    out = []
    for name, host in (("uitest", False), ("uitest_sys", False), ("uitest_host", True)):
        src = os.path.join(HARNESS, name + ".c")
        obj = os.path.join(hdir, name + ".o")
        flags = CASE_FLAGS + ([] if host else INC_LIBC + INC_API) + ["-I" + HARNESS]
        r = run([*GCC, *flags, "-c", "-o", obj, src])
        if r.returncode:
            sys.exit(f"uitest_host: compile of {name}.c failed:\n{r.stderr}")
        out.append(obj)
    own = []
    for src in LIBC_OWN:
        obj = os.path.join(hdir, "libc__" + os.path.basename(src)[:-2] + ".o")
        r = run([*GCC, *TOOLKIT_FLAGS, *INC_LIBC, *INC_API, "-c", "-o", obj, src])
        if r.returncode:
            sys.exit(f"uitest_host: compile of {src} failed:\n{r.stderr}")
        own.append(obj)
    # The app driver (stage 2) is in the archive too: only a case that
    # drives an app reaches it, and only such a case has uitest_app_main.
    obj = os.path.join(hdir, "uitest_app.o")
    r = run([*GCC, *CASE_FLAGS, *INC_LIBC, *INC_API, "-I" + HARNESS, "-c", "-o", obj,
             os.path.join(HARNESS, "uitest_app.c")])
    if r.returncode:
        sys.exit(f"uitest_host: compile of uitest_app.c failed:\n{r.stderr}")
    own.append(obj)
    lib = os.path.join(hdir, "libtolibc_own.a")
    if os.path.exists(lib):
        os.unlink(lib)
    r = run(["ar", "rcs", lib, *own])
    if r.returncode:
        sys.exit(f"uitest_host: ar failed:\n{r.stderr}")
    return out, lib


APP_LINE = re.compile(r"^//\s*uitest-app:\s*(\S+)")


def build_app(case, bindir, name, override):
    """A case whose first line is `// uitest-app: DIR` drives that app
    (stage 2): DIR's sources, compiled as the guest compiles a program but
    with main renamed uitest_app_main, which tools/lib/uitest/uitest_app.c
    runs on its own stack. ([objects], error)."""
    with open(case) as fh:
        m = APP_LINE.match(fh.readline())
    if not m:
        return [], None
    app = m.group(1).rstrip("/")
    srcs = sorted(glob.glob(os.path.join(ROOT, app, "*.c")) + glob.glob(os.path.join(ROOT, app, "*.cpp")))
    if not srcs:
        return [], f"`uitest-app: {app}` names no directory with sources"
    out = []
    for src in srcs:
        base, ext = os.path.splitext(os.path.basename(src))
        obj = os.path.join(bindir, f"{name}__app__{base}.o")
        if ext == ".cpp":
            # main() keeps C++ linkage, so it is renamed again and called from
            # a C-linkage shim with uitest_app_main's signature.
            cmd = [*GXX, *CXX_FLAGS, "-I" + os.path.join(ROOT, app), "-Dmain=uitest_cxx_main"]
        else:
            cmd = [*GCC, *TOOLKIT_FLAGS, *INC_LIBC, *INC_API, "-I" + os.path.join(ROOT, app),
                   "-Dmain=uitest_app_main"]
        r = run([*cmd, "-c", "-o", obj, override.get(src, src)])
        if r.returncode:
            return [], f"compile of {os.path.relpath(src, ROOT)} failed:\n{r.stderr}"
        out.append(obj)
    if any(s.endswith(".cpp") for s in srcs):
        shim = os.path.join(bindir, f"{name}__cxx_main.cpp")
        with open(shim, "w") as fh:
            fh.write('int uitest_cxx_main();\n'
                     'extern "C" int uitest_app_main(int, char **) { return uitest_cxx_main(); }\n')
        obj = shim[:-4] + ".o"
        r = run([*GXX, *CXX_FLAGS, "-c", "-o", obj, shim])
        if r.returncode:
            return [], f"compile of the C++ main shim failed:\n{r.stderr}"
        out.append(obj)
    return out, None


def link_case(case, harness, lib, bindir, override):
    harness, own = harness
    name = os.path.basename(case)[:-len("_uitest.c")]
    obj = os.path.join(bindir, name + ".o")
    exe = os.path.join(bindir, name)
    # userland/tests/ too: a case may share a fixture header with a guest test.
    r = run([*GCC, *CASE_FLAGS, *INC_LIBC, *INC_API, "-I" + HARNESS, "-Iuserland/tests", "-c", "-o", obj, case])
    if r.returncode:
        return name, None, f"compile failed:\n{r.stderr}"
    app_objs, err = build_app(case, bindir, name, override)
    if err:
        return name, None, err
    # g++ links a C++ app, so libstdc++ comes with it.
    linker = GXX if any(o.endswith("__cxx_main.o") for o in app_objs) else GCC
    r = run([*linker, "-o", exe, obj, *app_objs, *harness, "-Wl,--start-group", lib, own, "-Wl,--end-group",
             "-Wl,--gc-sections", "-Wl,--wrap=fopen", "-lm"])
    if r.returncode:
        return name, None, f"link failed:\n{r.stderr}"
    nm = run(["nm", "-D", "--undefined-only", exe])
    imports = {line.split()[-1].split("@")[0] for line in nm.stdout.splitlines() if line.strip()}
    bad = sorted(imports & ABI_UNSAFE)
    if bad:
        return name, None, ("imports " + ", ".join(bad) + " from glibc, whose structs differ from "
                            "tolibc's -- stub it in tools/lib/uitest/uitest_sys.c")
    # A RAW SYSCALL INSTRUCTION RUNS ON THE HOST'S KERNEL: SaunaOS's numbers
    # are not Linux's, so ugfx.c's WIN_REQ_FONT would be some other call
    # entirely. glibc is dynamic, so the binary's own text carries none
    # unless toolkit code with inline asm was reached.
    dis = run(["objdump", "-d", "--no-show-raw-insn", exe])
    hits = sorted({m.group(1) for m in re.finditer(r"<([^>]+)>:\n(?:(?!\n\n).)*?\ssyscall\b", dis.stdout, re.S)})
    if hits:
        return name, None, ("reaches a raw `syscall` instruction in " + ", ".join(hits) +
                            " -- on the host that is a LINUX syscall; keep the case off that path")
    return name, exe, None


def make_root():
    root = os.path.join(BUILD, "root")
    fonts = os.path.join(root, "usr", "share", "fonts")
    os.makedirs(os.path.dirname(fonts), exist_ok=True)
    if not os.path.islink(fonts):
        if os.path.exists(fonts):
            shutil.rmtree(fonts)
        os.symlink(os.path.join(ROOT, "data", "fonts"), fonts)
    # The rest of /usr/share is the tree's data/usr/share, as seeded.
    share = os.path.join(root, "usr", "share")
    for entry in sorted(os.listdir(os.path.join(ROOT, "data", "usr", "share"))):
        dst = os.path.join(share, entry)
        if not os.path.lexists(dst):
            os.symlink(os.path.join(ROOT, "data", "usr", "share", entry), dst)
    # /usr/share/ui as the Makefile stages it: every program's .ui file,
    # tracked beside its source -- linked from there, never from seed/sync/.
    ui = os.path.join(root, "usr", "share", "ui")
    shutil.rmtree(ui, ignore_errors=True)
    os.makedirs(ui)
    for top in ("userland/apps", "userland/services", "userland/utils"):
        for path in glob.glob(os.path.join(ROOT, top, "**", "*.ui"), recursive=True):
            dst = os.path.join(ui, os.path.basename(path))
            if not os.path.exists(dst):
                os.symlink(path, dst)
    tmp = os.path.join(root, "tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    # Each case's own /etc and /home (uitest_host.c), from earlier runs.
    for old in glob.glob(os.path.join(root, "private.*")):
        shutil.rmtree(old, ignore_errors=True)
    return root


def run_case(name, exe, root, keep):
    env = dict(os.environ, UITEST_ROOT=root)
    if keep:
        env["UITEST_KEEP"] = keep
    try:
        r = subprocess.run([exe], cwd=root, env=env, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return name, False, "timed out after 60 s"
    out = r.stdout + r.stderr
    ok = r.returncode == 0 and re.search(r": all checks passed \(\d+ checks\)", out) is not None
    return name, ok, out


def run_all(cases, harness, lib, jobs, keep, quiet=False, override=None, control=False):
    # A control's binaries apart from the real ones, an app control's too.
    bindir = os.path.join(BUILD, "bin-control" if control else "bin")
    os.makedirs(bindir, exist_ok=True)
    root = make_root()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        built = list(ex.map(lambda c: link_case(c, harness, lib, bindir, override or {}), cases))
        for name, exe, err in built:
            if err:
                results[name] = (False, err)
        runs = [ex.submit(run_case, n, e, root, keep) for n, e, err in built if not err]
        for f in runs:
            name, ok, out = f.result()
            results[name] = (ok, out)
    for name in sorted(results):
        ok, out = results[name]
        if not quiet or not ok:
            for line in out.splitlines():
                if not quiet or "FAIL" in line or not ok:
                    print(f"  {line}")
        print(f"{name}: {'PASS' if ok else 'FAIL'}")
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cases", nargs="*", help="case names (sbar, focusring, ...); default all")
    ap.add_argument("--list", action="store_true", help="name the cases and exit")
    ap.add_argument("--positive-control", action="store_true",
                    help="each control's edit must turn its cases red and leave the rest green")
    ap.add_argument("--keep", metavar="DIR", help="write uitest_keep() frames there as PPMs")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if shutil.which("gcc") is None:
        sys.exit("uitest_host: needs gcc")
    if not os.path.exists(os.path.join(ROOT, "build", "gen", "kconfig.h")):
        sys.exit("uitest_host: build/gen/kconfig.h is missing -- run `make all` once first")
    all_cases = sorted(glob.glob(os.path.join(CASES, "*_uitest.c")))
    names = {os.path.basename(c)[:-len("_uitest.c")]: c for c in all_cases}
    if args.list:
        print("\n".join(names))
        return 0
    unknown = [c for c in args.cases if c not in names]
    if unknown:
        sys.exit(f"uitest_host: no case named {', '.join(unknown)} (--list)")
    cases = [names[c] for c in args.cases] if args.cases else all_cases
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        args.keep = os.path.abspath(args.keep)

    lib, rebuilt, total = build_toolkit(args.jobs)
    print(f"uitest_host: toolkit {total} sources, {rebuilt} rebuilt")
    harness = build_harness()

    if not args.positive_control:
        results = run_all(cases, harness, lib, args.jobs, args.keep)
        bad = [n for n, (ok, _) in results.items() if not ok]
        print(f"uitest_host: {len(results) - len(bad)} of {len(results)} cases passed"
              + (f"; FAILED: {', '.join(sorted(bad))}" if bad else ""))
        return 1 if bad else 0

    wrong = []
    for control in CONTROLS:
        src, _, _, reds = control
        pclib, override = control_archive(lib, control)
        print(f"uitest_host: control in {src} -- {', '.join(reds)} must go red")
        results = run_all(cases, harness, pclib, args.jobs, None, quiet=True, override=override, control=True)
        for name, (ok, _) in results.items():
            if name in reds and ok:
                wrong.append(f"{name} stayed GREEN under the control in {src}")
            elif name not in reds and not ok:
                wrong.append(f"{name} went red under the control in {src}, which it does not cover")
    for w in wrong:
        print(f"uitest_host: {w}")
    print("uitest_host: positive control " + ("FAILED" if wrong else "passed: every control was caught"))
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
