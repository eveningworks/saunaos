#!/usr/bin/env python3
"""Stage the SDK -- the headers and libraries a program for SaunaOS is built against.

    python3 tools/build/stage_sdk.py --build build --out build/sysroot
    python3 tools/build/stage_sdk.py --build build --list     # what would be staged, from where

ONE TREE, TWO USES. `build/sysroot/usr/{include,lib}` is what the Makefile's
seed step copies to the disk's `/usr/include` and `/usr/lib`, AND what the
host's `x86_64-saunaos-gcc` is configured with as its sysroot
(docs/cc-design.md, stages 0 and 1). A program cross-built on the host and one
compiled on the machine see the same bytes.

THE INCLUDE ORDER IS THE TREE'S, AND IT IS LOAD-BEARING. The build compiles
ring 3 with `-I` libc, then kernel/include/api, abi, build/gen, then
userland/libs; names collide across them (libc's string.h and errno.h vs the
kernel's). So:

    /usr/include/             userland/libs/libc/include/, verbatim
    /usr/include/rt/sys.h     the syscall layer the POSIX headers are inline over
    /usr/include/{base,net,media,ui}/   the toolkit's headers ("base/x.h")
    /usr/include/uixx/        the C++ layer over them, header-only ("uixx/app.hpp")
    /usr/include/saunaos/     every kernel/include/{api,abi} and build/gen
                              header the above reach -- searched AFTER
                              /usr/include (`-idirafter`), as -I put them after
                              libc in the tree

api/, abi/ and gen/ share no file name, so they go flat into one directory;
`main()` refuses the day two of them do. Which kernel headers ship is NOT a
list: it is the compiler's own `-M` closure over the headers above, so a new
#include in a libc header ships its target with no edit here, and a kernel
header nothing public reaches stays off the disk.

DYNAMIC LINKING IS THE DEFAULT (docs/cc-design.md, "Dynamic by default").
The shared libraries live in /lib, where ld-toy finds them; /usr/lib holds a
LINKER SCRIPT per library a program may name -- libc.so is `INPUT(/lib/libc.so)`,
libSDL3.so is `INPUT(/lib/libSDL3.so.0)` -- the role a Linux distribution's
`-dev` symlink plays, without needing a symlink on the disk. ld reads a path
in a script found inside the sysroot as sysroot-relative, so the cross
linker needs the libraries themselves too: the sysroot's lib/ has a copy,
which the seed step does NOT put on the disk (the disk's /lib already has
the same files). -static never sees a script: it takes only the archives.

SDL's headers ship as <SDL3/...> and <SDL2/...>, with pkg-config files
(usr/lib/pkgconfig) and sdl2-config (usr/bin) that name them with `=`
paths, which GCC reads as sysroot-relative -- one answer for the cross
compiler and for the one on the machine.

/usr/lib holds the static archives (libc.a, libc_nonshared.a, the four toolkit
archives, and empty libm.a/libpthread.a/libdl.a/librt.a), the five runtime objects every program links (crt0.o first), and
the two linker scripts as saunaos.ld / saunaos-dyn.ld -- the shared libraries
stay in /lib, where the loader finds them. Archives and objects are copied
`objcopy --strip-debug` (deterministic: the same input gives the same bytes),
which takes them from ~17 MiB to ~6.

The output is replaced whole (written beside, then renamed), so a header
deleted from the tree leaves the SDK too. The disk's copy is kept exact the same
way: the seed step lists /usr/include and /usr/lib in `seed/exact`, which
tfs3_writer.py's `sync` prunes. tools/tests/gate/sdk_test.py checks both.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
REPO = toolpath.REPO

LIBC_INC = "userland/libs/libc/include"
RT_SYS = "userland/libs/rt/sys.h"
TOOLKIT = ("base", "net", "media", "ui")
# The C++ layer (docs/uixx-design.md): headers only, and outside the -M
# closure below, which compiles as C -- everything it includes is ui/'s.
UIXX = "userland/libs/uixx"
KERNEL_DIRS = ("kernel/include/api", "kernel/include/abi")   # + <build>/gen

ARCHIVES = ("libc.a", "libc_nonshared.a", "libbase.a", "libnet.a", "libmedia.a", "libui.a")
RT_OBJS = ("crt0.o", "sys.o", "stack_chk.o", "sigtramp.o", "tls.o")
# EMPTY ON PURPOSE: libc.a holds the maths, the threads and dlopen, but a
# build system says -lm / -lpthread / -ldl / -lrt, and g++ adds -lm to
# every link. musl ships the same names as empty archives for the same
# reason. An archive with no members is just its 8-byte magic.
EMPTY_ARCHIVES = ("libm.a", "libpthread.a", "libdl.a", "librt.a")
EMPTY_ARCHIVE = b"!<arch>\n"
LINK_SCRIPTS = (("userland/libs/rt/link.ld", "saunaos.ld"),
                ("userland/libs/rt/link-dyn.ld", "saunaos-dyn.ld"))
# The shared libraries a program may link by -l<name>: (the -l name, the
# file in /lib).
SHARED = (("c", "libc.so"), ("base", "libbase.so"), ("net", "libnet.so"),
          ("media", "libmedia.so"), ("ui", "libui.so"),
          ("SDL3", "libSDL3.so.0"), ("SDL2", "libSDL2-2.0.so.0"))
SDL3_INC = "userland/third_party/sdl3/upstream/include/SDL3"
SDL2_INC = "userland/third_party/sdl2compat/upstream/include/SDL2"


def sdl_version(header, names):
    vals = {}
    for line in open(os.path.join(REPO, header), encoding="utf-8"):
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "#define" and parts[1] in names:
            vals[parts[1]] = parts[2]
    return ".".join(vals[n] for n in names)


def sdl_files():
    """SDL's headers, and the pkg-config files and sdl2-config that name them."""
    out = {}
    for inc, sub in ((SDL3_INC, "SDL3"), (SDL2_INC, "SDL2")):
        for f in sorted(os.listdir(os.path.join(REPO, inc))):
            if f.endswith(".h"):
                out[f"usr/include/{sub}/{f}"] = (f"{inc}/{f}", False)
    v3 = sdl_version(f"{SDL3_INC}/SDL_version.h",
                     ("SDL_MAJOR_VERSION", "SDL_MINOR_VERSION", "SDL_MICRO_VERSION"))
    v2 = sdl_version(f"{SDL2_INC}/SDL_version.h",
                     ("SDL_MAJOR_VERSION", "SDL_MINOR_VERSION", "SDL_PATCHLEVEL"))
    pc = ("prefix=/usr\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n"
          "Name: {name}\nDescription: Simple DirectMedia Layer, SaunaOS's platform layer\n"
          "Version: {ver}\nLibs: -l{lib}\nCflags:{cflags}\n")
    out["usr/lib/pkgconfig/sdl3.pc"] = (pc.format(name="sdl3", ver=v3, lib="SDL3", cflags="").encode(), False)
    out["usr/lib/pkgconfig/sdl2.pc"] = (pc.format(name="sdl2", ver=v2, lib="SDL2",
                                                  cflags=" -I=/usr/include/SDL2 -D_REENTRANT").encode(), False)
    config = f"""#!/bin/sh
# sdl2-config for SaunaOS: SDL2 through sdl2-compat over SDL3. The -I path is
# `=`-prefixed, which GCC reads as sysroot-relative, so the cross compiler and
# the one on the machine take the same answer.
usage() {{ echo "Usage: sdl2-config [--prefix] [--exec-prefix] [--version] [--cflags] [--libs] [--static-libs]" >&2; exit 1; }}
[ $# -eq 0 ] && usage
for a in "$@"; do
    case "$a" in
    --prefix|--exec-prefix) echo /usr ;;
    --version) echo {v2} ;;
    --cflags) echo -I=/usr/include/SDL2 -D_REENTRANT ;;
    --libs) echo -lSDL2 ;;
    --static-libs) echo "sdl2-config: SDL2 is a shared library on SaunaOS" >&2; exit 1 ;;
    *) usage ;;
    esac
done
"""
    out["usr/bin/sdl2-config"] = (config.encode(), False)
    return out


def rel(path):
    return os.path.relpath(path, REPO)


def public_headers():
    """{sysroot path: repo path} for every header shipped by name."""
    out = {}
    root = os.path.join(REPO, LIBC_INC)
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith(".h"):
                src = os.path.join(d, f)
                out["usr/include/" + os.path.relpath(src, root)] = rel(src)
    out["usr/include/rt/sys.h"] = RT_SYS
    for lib in TOOLKIT:
        d = os.path.join(REPO, "userland/libs", lib)
        for f in sorted(os.listdir(d)):
            if f.endswith(".h"):
                out[f"usr/include/{lib}/{f}"] = f"userland/libs/{lib}/{f}"
    return out


def kernel_closure(build, cc, public):
    """{sysroot path: repo path} for the kernel headers `public` reaches.

    The tree's own -I order and the compiler's own -M, so this answers what
    the build would actually open -- not what a grep of #include lines
    guesses. -ffreestanding because that is how the tree compiles them; the
    hosted cross compiler sees the same files.
    """
    gen = os.path.join(build, "gen")
    roots = {os.path.join(REPO, d): d for d in KERNEL_DIRS}
    roots[os.path.abspath(gen)] = rel(gen)
    src = "".join(f'#include "{p}"\n' for p in sorted(public.values()))
    # The headers are named by their repo path, so the trailing `-I .` finds
    # them; the roots before it decide what everything THEY include opens.
    flags = ["-std=gnu11", "-ffreestanding", "-M",
             "-I", LIBC_INC, *[a for d in KERNEL_DIRS for a in ("-I", d)],
             "-I", gen, "-I", "userland", "-I", "userland/libs", "-I", ".", "-x", "c", "-"]
    r = subprocess.run([cc, *flags], input=src, capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        sys.exit(f"stage_sdk: the public headers do not compile on their own:\n{r.stderr}")
    deps = r.stdout.replace("\\\n", " ").split(":", 1)[1].split()
    out, mine = {}, set(public.values())
    for dep in deps:
        path = os.path.abspath(os.path.join(REPO, dep))
        if not path.startswith(REPO + os.sep):
            continue                     # the compiler's own stddef.h, stdint.h ...
        if rel(path) in mine:
            continue
        for root, name in roots.items():
            if os.path.dirname(path) == root:
                dst = "usr/include/saunaos/" + os.path.basename(path)
                if dst in out and out[dst] != rel(path):
                    sys.exit(f"stage_sdk: {out[dst]} and {rel(path)} would both be {dst} "
                             f"-- /usr/include/saunaos/ is flat, so the names must differ")
                out[dst] = rel(path)
                break
        else:
            sys.exit(f"stage_sdk: a public header includes {rel(path)}, which has no "
                     f"place in the SDK -- move it under one of the staged roots or "
                     f"add its directory here")
    return out


def plan(build, cc):
    """{sysroot path: (repo path, strip?)} -- everything the SDK holds."""
    public = public_headers()
    files = {k: (v, False) for k, v in public.items()}
    files.update({k: (v, False) for k, v in kernel_closure(build, cc, public).items()})
    for f in sorted(os.listdir(os.path.join(REPO, UIXX))):
        if f.endswith(".hpp"):
            files[f"usr/include/uixx/{f}"] = (f"{UIXX}/{f}", False)
    ub = rel(os.path.join(build, "userland"))
    for a in ARCHIVES:
        files["usr/lib/" + a] = (f"{ub}/{a}", True)
    for o in RT_OBJS:
        files["usr/lib/" + o] = (f"{ub}/libs/rt/{o}", True)
    for src, name in LINK_SCRIPTS:
        files["usr/lib/" + name] = (src, False)
    for a in EMPTY_ARCHIVES:
        files["usr/lib/" + a] = (None, False)
    bl = rel(os.path.join(build, "lib"))
    for name, so in SHARED:
        files["lib/" + so] = (f"{bl}/{so}", True)   # stripped as the seed step strips /lib's
        files[f"usr/lib/lib{name}.so"] = (f"INPUT(/lib/{so})\n".encode(), False)
    files.update(sdl_files())
    return files


def stage(files, out, objcopy):
    tmp = out + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    for dst, (src, strip) in sorted(files.items()):
        target = os.path.join(tmp, dst)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if src is None or isinstance(src, bytes):   # an empty archive, or text made here
            with open(target, "wb") as f:
                f.write(EMPTY_ARCHIVE if src is None else src)
            if target.endswith("/sdl2-config"):
                os.chmod(target, 0o755)
            continue
        source = os.path.join(REPO, src)
        if not os.path.exists(source):
            sys.exit(f"stage_sdk: {src} is missing -- `make all` builds it")
        if strip:
            subprocess.run([objcopy, "--strip-debug", source, target], check=True)
        else:
            shutil.copyfile(source, target)
    shutil.rmtree(out, ignore_errors=True)
    os.rename(tmp, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--build", default="build", help="the build directory (default: build)")
    ap.add_argument("--out", help="the sysroot to write (replaced whole)")
    ap.add_argument("--list", action="store_true", help="print what would be staged and from where")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--objcopy", default="objcopy")
    args = ap.parse_args()
    build = os.path.abspath(os.path.join(REPO, args.build))
    files = plan(build, args.cc)
    if args.list:
        for dst, (src, strip) in sorted(files.items()):
            what = "(generated)" if isinstance(src, bytes) else src or "(empty archive)"
            print(f"/{dst}\t{what}{'  (strip-debug)' if strip else ''}")
        return
    if not args.out:
        ap.error("--out or --list")
    stage(files, os.path.abspath(os.path.join(REPO, args.out)), args.objcopy)
    print(f"stage_sdk: {len(files)} files -> {args.out}")


if __name__ == "__main__":
    main()
