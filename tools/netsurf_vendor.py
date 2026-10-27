#!/usr/bin/env python3
"""Vendor NetSurf and its libraries into userland/ports/, at pinned commits.

WHAT IT DOES. Clones NetSurf and the ten libraries below at the commits
in PINS, builds them ON THE HOST with upstream's own build system (the
libraries, then NetSurf's headless `monkey` front end), and copies into
userland/ports/<name>/ exactly the files that build compiled -- read off
the compiler's dependency files, never listed by hand. Generated sources
(libcss's property parsers, libhubbub's entity and element tables,
libparserutils' charset aliases, libnspsl's suffix table) are produced by
that host build and vendored with the rest, so SaunaOS's own build runs
no Perl, gperf or generator.

WHY THE HOST BUILD, rather than a list of files. Upstream's Makefiles are
the only authority on which sources a configuration needs; a list kept
here would be a second copy of somebody else's build, wrong the first
time a file moves. The dependency files also name every HEADER each
source reached, so nothing is copied that nothing includes.

THE HOST BUILD IS ALSO STAGE 1'S ORACLE. The `nsmonkey` it leaves in the
work directory is the same pinned source, built for Linux;
docs/browser-design.md's stage 1 diffs its plot output against SaunaOS's.

**IT NEVER COPIES `testament.h`.** NetSurf generates it from the build
host and it records the builder's user name, host name and checkout
path. A neutral one is written instead (userland/ports/netsurf/
generated/testament.h) carrying only the pinned revision.

Needs on the host: git, gcc, make, perl, python3, gperf, pkg-config,
patch, the host's expat (libdom's binding links it there), and bison and
flex for nsgenbind.
Reaches github.com (the netsurf-browser organisation's mirrors, and
libexpat's).

OUR CHANGES ARE PATCHES, never edits in place: every port directory is
deleted and recopied here, so an edit made to it is lost silently at the
next re-vendor. userland/backends/<port>/patches/*.patch are applied to
each checkout before the host build, so the oracle and the copy are the
same patched source (BACKENDS below).

    python3 tools/netsurf_vendor.py                 # clone, build, vendor
    python3 tools/netsurf_vendor.py --work DIR      # the host build in DIR
                                                    # (default ~/.cache/saunaos/netsurf-host)
    python3 tools/netsurf_vendor.py --no-copy       # build the oracle only
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PORTS = os.path.join(REPO, "userland", "ports")
UPSTREAM = "https://github.com/netsurf-browser/{}.git"

# NOT under build/: the Makefile includes every .d file it finds there
# (`-include $(shell find $(BUILD) -name '*.d')`), and the host build
# leaves hundreds of its own -- one of them not even make syntax.
DEFAULT_WORK = os.path.join(os.environ.get("XDG_CACHE_HOME") or
                            os.path.expanduser("~/.cache"), "saunaos", "netsurf-host")

# Each at its master head on the day it was vendored (2026-10-12). NetSurf
# itself releases rarely (3.11, December 2023) while master is still
# committed to, which is why these are commits and not tags.
PINS = [
    ("buildsystem",    "0005ae300283ff01c2e2b05e7376b3e55dea21f7"),  # 2025-12-21
    ("libwapcaplet",   "c7c128d3eb3223b216c974471f82e9337fbcf4ba"),  # 2026-02-19
    ("libparserutils", "6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca"),  # 2026-01-03
    ("libhubbub",      "6651b8cf87a4aa87bcdb2ff024a02659cd3f9402"),  # 2026-01-03
    ("libdom",         "f69781e1f062444b5af3f62d431d7d94018da53b"),  # 2026-01-03
    ("libsvgtiny",     "073283b29dd800e4d938db42af7e131db848bf3b"),  # 2026-01-03
    ("libcss",         "499f1c4601ad39942fd1b2204053a387bec9b989"),  # 2026-08-23
    ("libnsgif",       "22e99eb6818b1284d0f3ff1b7f46159e87221220"),  # 2026-08-06
    ("libnsbmp",       "ea063c9f46acb43e90208da14073332b505ef7e7"),  # 2026-01-05
    ("libnsutils",     "0bd39060740b6163bd50875326654a722df97eb2"),  # 2026-01-05
    ("libnspsl",       "82815c2bc7fd70d1b6afccfa89a9a0f3fa73db8a"),  # 2026-01-05
    ("netsurf",        "39da3c3a40af4566d86500ff3052dfdc7f9a0378"),  # 2026-02-19
    ("nsgenbind",      "44c6736937ae17d4065d02959b82813b8f06a51e"),  # 2026-01-03
]
# Host tools: built and run on the host, never vendored. nsgenbind turns
# NetSurf's .bnd files and the WebIDL into the Duktape DOM bindings, whose
# OUTPUT is vendored (generated/duktape/).
HOST_TOOLS = ("buildsystem", "nsgenbind")
LIBS = [n for n, _ in PINS if n not in HOST_TOOLS + ("netsurf",)]

# The configuration: what the SaunaOS build will have. curl, OpenSSL,
# libpng and libjpeg are replaced by SaunaOS's own (libhttp, uimg) in later
# stages; JavaScript is Duktape (stage 6); SVG is libsvgtiny, over
# libdom's expat binding and EXPAT below. utf8proc only normalises IDN
# hosts.
NETSURF_FLAGS = [
    "TARGET=monkey",
    "NETSURF_USE_CURL=NO", "NETSURF_USE_OPENSSL=NO",
    "NETSURF_USE_JPEG=NO", "NETSURF_USE_PNG=NO", "NETSURF_USE_WEBP=NO",
    "NETSURF_USE_JPEGXL=NO", "NETSURF_USE_DUKTAPE=YES",
    "NETSURF_USE_UTF8PROC=NO", "NETSURF_USE_NSLOG=NO",
    "NETSURF_USE_NSSVG=YES", "NETSURF_USE_RSVG=NO", "NETSURF_USE_ROSPRITE=NO",
    "NETSURF_USE_HARU_PDF=NO",
]

# Compiled on the host, NOT vendored, per port.
#   netsurf: frontends/monkey/main.c -- its loop waits in select() on
#                stdin, which SaunaOS has not got; userland/tests/nsmonkey.c
#                replaces it.
#            content/fs_backing_store.c -- the on-disk cache, which only
#                monkey's main.c names. Ours runs from the memory cache
#                until a stage decides where a disk cache lives.
#   libnsutils: src/unistd.c -- nsu_pread()/nsu_pwrite(), whose only
#                caller is that disk cache, over pread()/pwrite(), which
#                tolibc has not got: the kernel has no positioned read.
NOT_VENDORED = {
    "netsurf": ("frontends/monkey/main.c", "content/fs_backing_store.c"),
    "libnsutils": ("src/unistd.c",),
}

# EXPAT, the XML parser under libdom's binding and so under libsvgtiny.
# Not a netsurf-browser repository and not built here: the host oracle
# links the host's own expat. Vendored from its release tag -- the
# translation units its lib/Makefile.am lists unconditionally, plus the
# one entropy source SaunaOS's configuration picks (getentropy(), over
# SYS_GETRANDOM); the headers they reach are read off `gcc -MM`, as the
# rest are off dependency files. expat_config.h is OURS
# (userland/backends/expat/), what its configure would have written.
EXPAT = ("libexpat", "https://github.com/libexpat/libexpat.git",
         "e127b43ac1f514e0b320508893aa338ecd5e72be", "R_2_9_0")  # 2026-10-05
EXPAT_EXTRA_UNITS = ["random_getentropy.c"]

# OUR CHANGES to a port are patch files, applied in name order to the
# checkout BEFORE the host build -- so the stage 1 oracle runs the same
# patched source the copy carries: userland/backends/<port>/patches/
# *.patch, each `-p1` against the port's own root (a/content/...,
# b/content/...). A patch that no longer applies stops the tool --
# re-vendoring is where a stale one is found, never later. FreeBSD's ports and Firefox's `mach vendor`
# have the same shape.
BACKENDS = os.path.join(REPO, "userland", "backends")

# Resources the engine loads by name at run time, which no dependency
# file can name: the user-agent stylesheets (resource:default.css and
# the rest), copied as upstream has them, and the English Messages file
# the host build generated for the monkey front end (gzipped, which
# utils/hashtable.c reads either way). The image stages both at
# /usr/share/netsurf/ (Makefile).
RESOURCES = ["resources/default.css", "resources/quirks.css",
             "resources/adblock.css", "resources/internal.css"]
GENERATED_RESOURCES = {"frontends/monkey/res/en/Messages": "generated/res/Messages"}

# Upstream's own licence file, by its own name.
LICENCE_FILE = "COPYING"


def run(cmd, cwd=None, env=None, log=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, stdout=log or subprocess.DEVNULL,
                       stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit(f"netsurf_vendor: failed ({r.returncode}): {' '.join(cmd)}"
                         + (f" -- see {log.name}" if log else ""))


def clone(work, name, sha):
    dest = os.path.join(work, "src", name)
    if os.path.isdir(os.path.join(dest, ".git")):
        head = subprocess.run(["git", "-C", dest, "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        if head == sha:
            return dest
        shutil.rmtree(dest)
    os.makedirs(dest)
    run(["git", "init", "-q"], cwd=dest)
    run(["git", "fetch", "-q", "--depth", "1", UPSTREAM.format(name), sha], cwd=dest)
    run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=dest)
    return dest


def host_build(work):
    src = os.path.join(work, "src")
    prefix = os.path.join(work, "prefix")
    shared = os.path.join(prefix, "share", "netsurf-buildsystem")
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, "lib", "pkgconfig"),
               PATH=os.path.join(prefix, "bin") + os.pathsep + os.environ.get("PATH", ""))
    # A build directory left by another configuration names ITS sources in
    # its dependency files, which are what decides the vendored set.
    for d in glob.glob(os.path.join(src, "netsurf", "build*")):
        shutil.rmtree(d)
    with open(os.path.join(work, "build.log"), "w") as log:
        run(["make", "-C", os.path.join(src, "buildsystem"), "install",
             f"PREFIX={prefix}"], env=env, log=log)
        run(["make", "-C", os.path.join(src, "nsgenbind"), "install", f"PREFIX={prefix}",
             f"NSSHARED={shared}"], env=env, log=log)
        for lib in LIBS:
            run(["make", "-C", os.path.join(src, lib), "install", f"PREFIX={prefix}",
                 f"NSSHARED={shared}"], env=env, log=log)
        run(["make", "-C", os.path.join(src, "netsurf"), f"PREFIX={prefix}",
             f"NSSHARED={shared}"] + NETSURF_FLAGS, env=env, log=log)
    return os.path.join(src, "netsurf", "nsmonkey")


def dep_files(root):
    """Every path a dependency file under `root`'s build directory names,
    and the subset that were COMPILED -- each rule's first prerequisite.
    The difference matters: libhubbub #includes one generated .c into
    another, so "every .c" is not "every translation unit"."""
    paths, compiled = set(), set()
    for d in glob.glob(os.path.join(root, "build*", "**", "*.d"), recursive=True):
        with open(d) as f:
            text = f.read().replace("\\\n", " ")
        for line in text.splitlines():
            if ":" not in line:
                continue
            deps = line.split(":", 1)[1].split()
            if deps and deps[0].endswith(".c"):
                compiled.add(os.path.normpath(os.path.join(root, deps[0])))
            for p in deps:
                paths.add(os.path.normpath(os.path.join(root, p)))
    return paths, compiled


def vendored_set(root, name):
    """Sources and headers inside the repository the build reached, plus
    the public include/ tree (other components include it through the
    installed copy, so their dependency files name the prefix instead)."""
    root = os.path.normpath(root)
    keep, generated = set(), {}
    paths, compiled = dep_files(root)
    for p in paths:
        if not p.startswith(root + os.sep):
            continue
        rel = os.path.relpath(p, root)
        if rel.startswith("build"):
            # nsgenbind's output, under the build's OBJROOT/duktape/: kept
            # at generated/duktape/, which is on the core's include path
            # as OBJROOT is upstream's.
            parts = rel.split(os.sep)
            if name == "netsurf" and "duktape" in parts:
                generated[rel] = os.path.join("generated", *parts[parts.index("duktape"):])
            continue
        if rel.startswith(NOT_VENDORED.get(name, ())):
            continue
        keep.add(rel)
    inc = os.path.join(root, "include")
    for p in glob.glob(os.path.join(inc, "**", "*.h"), recursive=True):
        keep.add(os.path.relpath(p, root))
    keep.add(LICENCE_FILE)
    units = sorted([os.path.relpath(c, root) for c in compiled
                    if os.path.relpath(c, root) in keep] +
                   [generated[os.path.relpath(c, root)] for c in compiled
                    if os.path.relpath(c, root) in generated])
    return sorted(keep), units, generated


TESTAMENT = """\
/* testament.h -- written by tools/netsurf_vendor.py, NOT upstream's.
 *
 * Upstream generates this from the build host, and that copy records the
 * builder's user name, host name and checkout path. This one carries the
 * pinned revision and nothing about who vendored it.
 */
#ifndef NETSURF_REVISION_TESTAMENT
#define NETSURF_REVISION_TESTAMENT "{sha}"
#define USERNAME "saunaos"
#define GECOS "saunaos"
#define WT_ROOT "userland/ports/netsurf/"
#define WT_HOSTNAME "saunaos"
#define WT_COMPILEDATE "vendored"
#define WT_BRANCHPATH "master"
#define WT_BRANCHISMASTER 1
#define WT_REVID "{sha}"
#define WT_MODIFIED 0
#define WT_MODIFICATIONS {{\\
 \\
}}
#endif
"""

README = """\
# {name} -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/{name}/patches/`, which the tool applies to the
checkout before every copy; or change the C library, or our front end in
`userland/backends/netsurf/`. `docs/browser-design.md` is the plan this
serves.

- **Upstream**: {url}
- **Commit**: `{sha}` ({date})
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `{licence}`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.{extra}
- **Licence**: `{licence}`, upstream's own file.{patches}
"""

EXTRA = {
    "netsurf": "\n- **Not taken**: `frontends/monkey/main.c` -- its loop waits in\n"
               "  `select()`, which SaunaOS has not got; `userland/tests/nsmonkey.c`\n"
               "  replaces it, and `content/fs_backing_store.c`, the disk cache only\n"
               "  that main.c named. `generated/testament.h` is the tool's, not upstream's\n"
               "  (upstream's records the build host). The four stylesheets under\n"
               "  `resources/` and `generated/res/Messages` are run-time resources,\n"
               "  staged on the image at `/usr/share/netsurf/`. `generated/duktape/` is\n"
               "  nsgenbind's output on the host: the DOM bindings for Duktape, from\n"
               "  the `.bnd` files and the WebIDL beside `dukky.c`.",
    "libdom": "\n- **Not taken**: `bindings/xml/libxml_xmlparser.c` -- the binding\n"
              "  over libxml2. The expat one is taken, for libsvgtiny.",
    "libsvgtiny": "\n- `src/autogenerated_colors.c` is gperf's output on the host: SVG's\n"
                  "  named colours, `#include`d by `src/svgtiny_parse.c`.",
    "libnsutils": "\n- **Not taken**: `src/unistd.c` -- `nsu_pread()`/`nsu_pwrite()`, whose\n"
                  "  only caller is NetSurf's disk cache (not taken either), over\n"
                  "  `pread()`, which SaunaOS has not got.",
}


def patch_files(name):
    return sorted(glob.glob(os.path.join(BACKENDS, name, "patches", "*.patch")))


def apply_patches(name, root):
    """Puts the port's checkout back to its pin, then applies its patches
    in name order -- so the HOST build (the oracle) and the copy are the
    same patched source."""
    run(["git", "-C", root, "checkout", "-q", "--", "."])
    for p in patch_files(name):
        r = subprocess.run(["patch", "-p1", "-s", "-N", "--no-backup-if-mismatch",
                            "-d", root, "-i", p], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"netsurf_vendor: {os.path.relpath(p, REPO)} no longer applies:\n"
                             + r.stdout + r.stderr)


def patch_notes(name):
    """The README lines naming each patch, by its first line."""
    lines = []
    for p in patch_files(name):
        with open(p) as f:
            first = f.readline().strip()
        lines.append(f"  - `{os.path.basename(p)}` -- {first}")
    if not lines:
        return ""
    return ("\n- **Patched, by SaunaOS**, from `userland/backends/" + name + "/patches/`"
            " (applied to the checkout before the host build and the copy):\n" + "\n".join(lines))


EXPAT_README = """\
# expat -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/expat/patches/`, which the tool applies to the
checkout before every copy. `expat_config.h` is ours, in `userland/backends/expat/`.

- **Upstream**: {url}
- **Release**: `{tag}`, commit `{sha}` ({date})
- **Files**: `expat/lib/`'s translation units that `lib/Makefile.am`
  lists unconditionally, plus `random_getentropy.c` (the entropy source
  SaunaOS's configuration picks), the headers they reach (`gcc -MM`), and
  `COPYING`. Under libdom's XML binding, for libsvgtiny.
- **Licence**: `COPYING`, upstream's own file.{patches}
"""


def vendor_expat(work):
    name, url, sha, tag = EXPAT
    root = os.path.join(work, "src", name)
    if not os.path.isdir(os.path.join(root, ".git")) or subprocess.run(
            ["git", "-C", root, "rev-parse", "HEAD"], capture_output=True,
            text=True).stdout.strip() != sha:
        if os.path.isdir(root):
            shutil.rmtree(root)
        os.makedirs(root)
        run(["git", "init", "-q"], cwd=root)
        run(["git", "fetch", "-q", "--depth", "1", url, sha], cwd=root)
        run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=root)
    apply_patches("expat", root)
    lib = os.path.join(root, "expat", "lib")
    with open(os.path.join(lib, "Makefile.am")) as f:
        am = f.read().replace("\\\n", " ")
    units = None
    for line in am.splitlines():
        if line.startswith("libexpat_la_SOURCES ="):
            units = [w for w in line.split("=", 1)[1].split() if w.endswith(".c")]
            break
    if not units:
        raise SystemExit("netsurf_vendor: no libexpat_la_SOURCES in expat's lib/Makefile.am")
    units += EXPAT_EXTRA_UNITS
    keep = set()
    for u in units:
        r = subprocess.run(["gcc", "-MM", "-I", os.path.join(BACKENDS, "expat"), "-I", lib,
                            os.path.join(lib, u)], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"netsurf_vendor: gcc -MM {u}: {r.stderr}")
        for p in r.stdout.replace("\\\n", " ").split(":", 1)[1].split():
            p = os.path.normpath(os.path.join(lib, p) if not os.path.isabs(p) else p)
            if p.startswith(lib + os.sep):
                keep.add(os.path.relpath(p, lib))
    dest = os.path.join(PORTS, "expat")
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(os.path.join(dest, "lib"))
    for rel in sorted(keep):
        shutil.copy2(os.path.join(lib, rel), os.path.join(dest, "lib", rel))
    shutil.copy2(os.path.join(root, "expat", LICENCE_FILE), os.path.join(dest, LICENCE_FILE))
    date = subprocess.run(["git", "-C", root, "log", "-1", "--format=%cs"],
                          capture_output=True, text=True).stdout.strip()
    with open(os.path.join(dest, "README.md"), "w") as f:
        f.write(EXPAT_README.format(url=url[:-4], tag=tag, sha=sha, date=date,
                                    patches=patch_notes("expat")))
    with open(os.path.join(dest, "sources.mk"), "w") as f:
        f.write("# Written by tools/netsurf_vendor.py: expat's translation units for\n"
                "# SaunaOS's configuration, which is not every .c in it.\n"
                "NS_SRCS_expat = \\\n")
        f.write(" \\\n".join(f"\tuserland/ports/expat/lib/{u}" for u in units) + "\n")
    print(f"  {'expat':16s} {sha[:12]}  {len(keep)} files, {len(units)} compiled")


def vendor(work):
    vendor_expat(work)
    for name, sha in PINS:
        if name in HOST_TOOLS:
            continue
        root = os.path.join(work, "src", name)
        files, units, generated = vendored_set(root, name)
        dest = os.path.join(PORTS, name)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        for rel in files:
            os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
            shutil.copy2(os.path.join(root, rel), os.path.join(dest, rel))
        for src_rel, rel in generated.items():
            os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
            # nsgenbind spells its includes with the output directory it
            # was given ("build/Linux-monkey/duktape/binding.h"); ours is
            # generated/, on the include path, so they become "duktape/...".
            with open(os.path.join(root, src_rel), encoding="latin-1") as f:
                text = f.read()
            parts = src_rel.split(os.sep)
            if "duktape" in parts:
                obj = os.path.join(*parts[:parts.index("duktape")])
                text = text.replace(f'"{obj}/duktape/', '"duktape/')
            with open(os.path.join(dest, rel), "w", encoding="latin-1") as f:
                f.write(text)
        date = subprocess.run(["git", "-C", root, "log", "-1", "--format=%cs"],
                              capture_output=True, text=True).stdout.strip()
        with open(os.path.join(dest, "README.md"), "w") as f:
            f.write(README.format(name=name, url=UPSTREAM.format(name)[:-4], sha=sha,
                                  date=date, licence=LICENCE_FILE,
                                  extra=EXTRA.get(name, ""),
                                  patches=patch_notes(name)))
        var = name
        with open(os.path.join(dest, "sources.mk"), "w") as f:
            f.write(f"# Written by tools/netsurf_vendor.py: the translation units the host\n"
                    f"# build compiled from this port, which is not every .c in it.\n"
                    f"NS_SRCS_{var} = \\\n")
            f.write(" \\\n".join(f"\tuserland/ports/{name}/{u}" for u in units) + "\n")
        if name == "netsurf":
            for rel in RESOURCES:
                os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
                shutil.copy2(os.path.join(root, rel), os.path.join(dest, rel))
            for src, rel in GENERATED_RESOURCES.items():
                os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
                shutil.copy2(os.path.join(root, src), os.path.join(dest, rel))
            os.makedirs(os.path.join(dest, "generated"), exist_ok=True)
            with open(os.path.join(dest, "generated", "testament.h"), "w") as f:
                f.write(TESTAMENT.format(sha=sha))
        print(f"  {name:16s} {sha[:12]}  {len(files)} files, {len(units)} compiled")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--work", default=DEFAULT_WORK,
                    help=f"where the clones and the host build live (default {DEFAULT_WORK})")
    ap.add_argument("--no-copy", action="store_true",
                    help="build the host oracle only; leave userland/ports/ alone")
    a = ap.parse_args()
    for tool in ("git", "make", "perl", "gperf", "pkg-config", "bison", "flex", "patch"):
        if not shutil.which(tool):
            raise SystemExit(f"netsurf_vendor: needs `{tool}` on the host")
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    for name, sha in PINS:
        apply_patches(name, clone(work, name, sha))
    print(f"netsurf_vendor: building on the host (log: {work}/build.log)")
    monkey = host_build(work)
    print(f"netsurf_vendor: host oracle {monkey}")
    if not a.no_copy:
        vendor(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
