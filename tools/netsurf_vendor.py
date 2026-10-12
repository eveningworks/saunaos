#!/usr/bin/env python3
"""Vendor NetSurf and its libraries into userland/ports/, at pinned commits.

WHAT IT DOES. Clones NetSurf and the ten libraries below at the commits
in PINS, builds them ON THE HOST with upstream's own build system (the
libraries, then NetSurf's headless `monkey` front end), and copies into
userland/ports/<name>/ exactly the files that build compiled -- read off
the compiler's dependency files, never listed by hand. Generated sources
(libcss's property parsers, libhubbub's entity and element tables,
libparserutils' charset aliases, libnspsl's suffix table) are produced by
that host build and vendored with the rest, so toy-os's own build runs
no Perl, gperf or generator.

WHY THE HOST BUILD, rather than a list of files. Upstream's Makefiles are
the only authority on which sources a configuration needs; a list kept
here would be a second copy of somebody else's build, wrong the first
time a file moves. The dependency files also name every HEADER each
source reached, so nothing is copied that nothing includes.

THE HOST BUILD IS ALSO STAGE 1'S ORACLE. The `nsmonkey` it leaves in the
work directory is the same pinned source, built for Linux;
docs/browser-design.md's stage 1 diffs its plot output against toy-os's.

**IT NEVER COPIES `testament.h`.** NetSurf generates it from the build
host and it records the builder's user name, host name and checkout
path. A neutral one is written instead (userland/ports/netsurf/
generated/testament.h) carrying only the pinned revision.

Needs on the host: git, gcc, make, perl, python3, gperf, pkg-config.
Reaches github.com (the netsurf-browser organisation's mirrors).

    python3 tools/netsurf_vendor.py                 # clone, build, vendor
    python3 tools/netsurf_vendor.py --work DIR      # the host build in DIR
                                                    # (default ~/.cache/toy-os/netsurf-host)
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
                            os.path.expanduser("~/.cache"), "toy-os", "netsurf-host")

# Each at its master head on the day it was vendored (2026-10-12). NetSurf
# itself releases rarely (3.11, December 2023) while master is still
# committed to, which is why these are commits and not tags.
PINS = [
    ("buildsystem",    "0005ae300283ff01c2e2b05e7376b3e55dea21f7"),  # 2025-12-21
    ("libwapcaplet",   "c7c128d3eb3223b216c974471f82e9337fbcf4ba"),  # 2026-02-19
    ("libparserutils", "6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca"),  # 2026-01-03
    ("libhubbub",      "6651b8cf87a4aa87bcdb2ff024a02659cd3f9402"),  # 2026-01-03
    ("libdom",         "f69781e1f062444b5af3f62d431d7d94018da53b"),  # 2026-01-03
    ("libcss",         "499f1c4601ad39942fd1b2204053a387bec9b989"),  # 2026-08-23
    ("libnsgif",       "22e99eb6818b1284d0f3ff1b7f46159e87221220"),  # 2026-08-06
    ("libnsbmp",       "ea063c9f46acb43e90208da14073332b505ef7e7"),  # 2026-01-05
    ("libnsutils",     "0bd39060740b6163bd50875326654a722df97eb2"),  # 2026-01-05
    ("libnspsl",       "82815c2bc7fd70d1b6afccfa89a9a0f3fa73db8a"),  # 2026-01-05
    ("netsurf",        "39da3c3a40af4566d86500ff3052dfdc7f9a0378"),  # 2026-02-19
]
LIBS = [n for n, _ in PINS if n not in ("buildsystem", "netsurf")]

# The configuration: what the toy-os build will have. curl, OpenSSL,
# libpng and libjpeg are replaced by toy-os's own (libhttp, uimg) in later
# stages; JavaScript (Duktape) is stage 6; SVG needs an XML parser
# (expat), which is not vendored. utf8proc only normalises IDN hosts.
NETSURF_FLAGS = [
    "TARGET=monkey",
    "NETSURF_USE_CURL=NO", "NETSURF_USE_OPENSSL=NO",
    "NETSURF_USE_JPEG=NO", "NETSURF_USE_PNG=NO", "NETSURF_USE_WEBP=NO",
    "NETSURF_USE_JPEGXL=NO", "NETSURF_USE_DUKTAPE=NO",
    "NETSURF_USE_UTF8PROC=NO", "NETSURF_USE_NSLOG=NO",
    "NETSURF_USE_NSSVG=NO", "NETSURF_USE_RSVG=NO", "NETSURF_USE_ROSPRITE=NO",
    "NETSURF_USE_HARU_PDF=NO",
]

# Compiled on the host, NOT vendored, per port.
#   netsurf: frontends/monkey/main.c -- its loop waits in select() on
#                stdin, which toy-os has not got; userland/tests/nsmonkey.c
#                replaces it.
#            content/fs_backing_store.c -- the on-disk cache, which only
#                monkey's main.c names. Ours runs from the memory cache
#                until a stage decides where a disk cache lives.
#   libnsutils: src/unistd.c -- nsu_pread()/nsu_pwrite(), whose only
#                caller is that disk cache, over pread()/pwrite(), which
#                tolibc has not got: the kernel has no positioned read.
#   libdom: bindings/xml/ -- the XML parser binding, over expat, which
#                NetSurf does not use in this configuration.
NOT_VENDORED = {
    "netsurf": ("frontends/monkey/main.c", "content/fs_backing_store.c"),
    "libnsutils": ("src/unistd.c",),
    "libdom": ("bindings/xml/",),
}

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
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, "lib", "pkgconfig"))
    with open(os.path.join(work, "build.log"), "w") as log:
        run(["make", "-C", os.path.join(src, "buildsystem"), "install",
             f"PREFIX={prefix}"], env=env, log=log)
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
    keep = set()
    paths, compiled = dep_files(root)
    for p in paths:
        if not p.startswith(root + os.sep):
            continue
        rel = os.path.relpath(p, root)
        if rel.startswith("build"):
            continue
        if rel.startswith(NOT_VENDORED.get(name, ())):
            continue
        keep.add(rel)
    inc = os.path.join(root, "include")
    for p in glob.glob(os.path.join(inc, "**", "*.h"), recursive=True):
        keep.add(os.path.relpath(p, root))
    keep.add(LICENCE_FILE)
    units = sorted(os.path.relpath(c, root) for c in compiled
                   if os.path.relpath(c, root) in keep)
    return sorted(keep), units


TESTAMENT = """\
/* testament.h -- written by tools/netsurf_vendor.py, NOT upstream's.
 *
 * Upstream generates this from the build host, and that copy records the
 * builder's user name, host name and checkout path. This one carries the
 * pinned revision and nothing about who vendored it.
 */
#ifndef NETSURF_REVISION_TESTAMENT
#define NETSURF_REVISION_TESTAMENT "{sha}"
#define USERNAME "toy-os"
#define GECOS "toy-os"
#define WT_ROOT "userland/ports/netsurf/"
#define WT_HOSTNAME "toy-os"
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
`tools/netsurf_vendor.py`. Do not edit it**: change the C library, or
our front end in `userland/backends/netsurf/`, or re-run the tool at a
new pin. `docs/browser-design.md` is the plan this serves.

- **Upstream**: {url}
- **Commit**: `{sha}` ({date})
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `{licence}`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.{extra}
- **Licence**: `{licence}`, upstream's own file.
"""

EXTRA = {
    "netsurf": "\n- **Not taken**: `frontends/monkey/main.c` -- its loop waits in\n"
               "  `select()`, which toy-os has not got; `userland/tests/nsmonkey.c`\n"
               "  replaces it, and `content/fs_backing_store.c`, the disk cache only\n"
               "  that main.c named. `generated/testament.h` is the tool's, not upstream's\n"
               "  (upstream's records the build host). The four stylesheets under\n"
               "  `resources/` and `generated/res/Messages` are run-time resources,\n"
               "  staged on the image at `/usr/share/netsurf/`.",
    "libdom": "\n- **Not taken**: `bindings/xml/` -- the XML parser binding, over\n"
              "  expat, which NetSurf does not use in this configuration.",
    "libnsutils": "\n- **Not taken**: `src/unistd.c` -- `nsu_pread()`/`nsu_pwrite()`, whose\n"
                  "  only caller is NetSurf's disk cache (not taken either), over\n"
                  "  `pread()`, which toy-os has not got.",
}


def vendor(work):
    for name, sha in PINS:
        if name == "buildsystem":
            continue
        root = os.path.join(work, "src", name)
        files, units = vendored_set(root, name)
        dest = os.path.join(PORTS, name)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        for rel in files:
            os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
            shutil.copy2(os.path.join(root, rel), os.path.join(dest, rel))
        date = subprocess.run(["git", "-C", root, "log", "-1", "--format=%cs"],
                              capture_output=True, text=True).stdout.strip()
        with open(os.path.join(dest, "README.md"), "w") as f:
            f.write(README.format(name=name, url=UPSTREAM.format(name)[:-4], sha=sha,
                                  date=date, licence=LICENCE_FILE,
                                  extra=EXTRA.get(name, "")))
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
    for tool in ("git", "make", "perl", "gperf", "pkg-config"):
        if not shutil.which(tool):
            raise SystemExit(f"netsurf_vendor: needs `{tool}` on the host")
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    for name, sha in PINS:
        clone(work, name, sha)
    print(f"netsurf_vendor: building on the host (log: {work}/build.log)")
    monkey = host_build(work)
    print(f"netsurf_vendor: host oracle {monkey}")
    if not a.no_copy:
        vendor(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
