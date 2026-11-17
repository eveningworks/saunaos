#!/usr/bin/env python3
"""Vendor SDL3 and sdl2-compat into userland/third_party/{sdl3,sdl2compat}/upstream/, at pinned releases.

WHAT IT DOES. Downloads the two release tarballs named in PINS, refuses
one whose SHA-256 differs, unpacks them into a work directory, and copies
into each port's upstream/ exactly the files SaunaOS's configuration
compiles: the translation units below, every header they reach (read off
`gcc -MM` with SaunaOS's own flags and glue/SDL_build_config_private.h,
never listed by hand), the public include/ tree whole, and the licence.
It writes each port's sources.mk, which the Makefile includes, and the
one header sdl2-compat's CMake would generate (SDL_config_unix.h).

WHY THE TRANSLATION UNITS ARE GLOBS. SDL's CMakeLists.txt globs its
generic sources (`src/*.c`, `src/audio/*.c`, ...) for every platform and
adds one directory per driver; SDL3_UNITS is that list for the drivers
SaunaOS picks. Host CMake is not needed: SaunaOS is an SDL "private
platform", whose build configuration is our own header.

ALMOST NO PATCHES. SDL's private-platform hooks (SDL_PLATFORM_PRIVATE,
the *_PRIVATE drivers and modules) let what is SaunaOS-shaped live in
userland/third_party/sdl3/glue/. What they cannot reach is a patch in
userland/third_party/<port>/patches/, applied in name order to the COPY
after it is made (the cached tarball tree stays pristine), each saying
why at its top.

Reaches github.com (release downloads, redirected to
release-assets.githubusercontent.com).

    python3 tools/fetch/sdl_vendor.py               # download, check, vendor
    python3 tools/fetch/sdl_vendor.py --work DIR    # unpack in DIR
                                                    # (default ~/.cache/saunaos/sdl)
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)
THIRD_PARTY = os.path.join(REPO, "userland", "third_party")

DEFAULT_WORK = os.path.join(os.environ.get("XDG_CACHE_HOME") or
                            os.path.expanduser("~/.cache"), "saunaos", "sdl")

# (port directory, top directory in the tarball, URL, SHA-256). The
# newest releases on the day they were vendored (2026-11-17).
PINS = [
    ("sdl3", "SDL3-3.4.20",
     "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz",
     "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f"),
    ("sdl2compat", "sdl2-compat-2.32.74",
     "https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz",
     "ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588"),
]

# SDL3's translation units: CMakeLists.txt's generic globs, then one
# directory per driver this configuration picks. pthread's semaphore is
# left out for the generic one (tolibc has no <semaphore.h>), and the
# Unix time and filesystem modules for glue/'s.
SDL3_UNITS = [
    "src/*.c", "src/atomic/*.c", "src/audio/*.c", "src/camera/*.c",
    "src/core/*.c", "src/cpuinfo/*.c", "src/dynapi/*.c", "src/events/*.c",
    "src/io/*.c", "src/io/generic/*.c", "src/filesystem/*.c", "src/gpu/*.c",
    "src/joystick/*.c", "src/haptic/*.c", "src/hidapi/*.c", "src/locale/*.c",
    "src/main/*.c", "src/misc/*.c", "src/power/*.c", "src/render/*.c",
    "src/render/software/*.c", "src/sensor/*.c", "src/stdlib/*.c",
    "src/storage/*.c", "src/thread/*.c", "src/time/*.c", "src/timer/*.c",
    "src/video/*.c", "src/video/yuv2rgb/*.c", "src/libm/*.c",
    "src/dialog/*.c", "src/tray/*.c", "src/process/*.c",
    # the drivers
    "src/audio/dummy/*.c", "src/audio/disk/*.c", "src/video/dummy/*.c",
    "src/camera/dummy/*.c", "src/joystick/dummy/*.c", "src/haptic/dummy/*.c",
    "src/sensor/dummy/*.c", "src/thread/pthread/*.c",
    "src/thread/generic/SDL_syssem.c", "src/timer/unix/*.c",
    "src/loadso/dlopen/*.c", "src/filesystem/posix/*.c",
    "src/locale/unix/*.c", "src/misc/dummy/*.c", "src/process/dummy/*.c",
    "src/dialog/dummy/*.c", "src/tray/dummy/*.c", "src/storage/generic/*.c",
    "src/main/generic/*.c",
]
SDL3_EXCLUDE = ["src/thread/pthread/SDL_syssem.c"]

# sdl2-compat's, from its CMakeLists.txt's SDL2COMPAT_SRCS.
SDL2COMPAT_UNITS = ["src/sdl2_compat.c", "src/dynapi/SDL_dynapi.c"]

LICENCE = "LICENSE.txt"


def fetch(work, port, top, url, sha):
    """The unpacked tree, downloaded and checked first if it is not there."""
    tree = os.path.join(work, top)
    if os.path.isdir(tree):
        return tree
    os.makedirs(work, exist_ok=True)
    tarball = os.path.join(work, os.path.basename(url))
    if not os.path.exists(tarball):
        print(f"sdl_vendor: downloading {url}")
        urllib.request.urlretrieve(url, tarball)
    with open(tarball, "rb") as f:
        got = hashlib.sha256(f.read()).hexdigest()
    if got != sha:
        os.unlink(tarball)
        sys.exit(f"sdl_vendor: {os.path.basename(tarball)} has SHA-256 {got}, PINS says {sha}")
    with tarfile.open(tarball) as t:
        t.extractall(work, filter="data")
    return tree


def compile_flags(sdl3_tree):
    """SaunaOS's compile, as the Makefile's SDL3_CFLAGS has it -- enough
    for gcc -MM to resolve the same headers."""
    gcc_inc = subprocess.check_output(["gcc", "-print-file-name=include"], text=True).strip()
    return ["-std=gnu11", "-ffreestanding", "-nostdinc", "-isystem", gcc_inc,
            "-I" + os.path.join(REPO, "userland/libs/libc/include"),
            "-I" + os.path.join(REPO, "kernel/include/api"),
            "-I" + os.path.join(REPO, "kernel/include/abi"),
            "-I" + os.path.join(REPO, "build/gen"),
            "-I" + os.path.join(REPO, "userland"), "-I" + os.path.join(REPO, "userland/libs"),
            "-U__linux__", "-U__linux", "-U__gnu_linux__", "-Ulinux",
            "-DSDL_PLATFORM_PRIVATE", '-DSDL_PLATFORM_PRIVATE_NAME="SaunaOS"',
            "-I" + os.path.join(THIRD_PARTY, "sdl3/glue"),
            "-I" + os.path.join(sdl3_tree, "include"),
            "-I" + os.path.join(sdl3_tree, "include/build_config"),
            "-I" + os.path.join(sdl3_tree, "src")]


def units(tree, patterns, exclude=()):
    found = []
    for p in patterns:
        for f in sorted(glob.glob(os.path.join(tree, p))):
            rel = os.path.relpath(f, tree)
            if rel not in exclude and rel not in found:
                found.append(rel)
    return found


def headers(tree, unit_list, flags):
    """Every file under `tree` that one of the units #includes."""
    reached = set()
    for u in unit_list:
        out = subprocess.run(["gcc", "-MM"] + flags + [u], cwd=tree,
                             capture_output=True, text=True)
        if out.returncode != 0:
            sys.exit(f"sdl_vendor: gcc -MM {u} failed:\n{out.stderr}")
        for word in out.stdout.replace("\\\n", " ").split()[1:]:
            path = os.path.realpath(os.path.join(tree, word))
            if path.startswith(os.path.realpath(tree) + os.sep):
                reached.add(os.path.relpath(path, os.path.realpath(tree)))
    return reached


def copy_port(tree, port, files, public_include):
    dest = os.path.join(THIRD_PARTY, port, "upstream")
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    wanted = set(files) | {LICENCE}
    for root, _, names in os.walk(os.path.join(tree, public_include)):
        for n in names:
            if n.endswith(".h"):
                wanted.add(os.path.relpath(os.path.join(root, n), tree))
    for rel in sorted(wanted):
        os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
        shutil.copy2(os.path.join(tree, rel), os.path.join(dest, rel))
    return dest, len(wanted)


def configure_sdl2_config(tree, dest):
    """include/SDL2/SDL_config_unix.h, as sdl2-compat's CMake writes it
    from the template (configure_file): the header SDL_config.h picks on
    any __unix__ system, so every program built against SDL2 reads it.
    Its one switch is X11, which SaunaOS has not got."""
    name = "include/SDL2/SDL_config_unix.h"
    with open(os.path.join(tree, name + ".cmake")) as f:
        text = f.read()
    text = text.replace("#cmakedefine SDL2COMPAT_DISABLE_X11", "#define SDL2COMPAT_DISABLE_X11")
    if "#cmakedefine" in text or "@" in text.split("*/", 1)[1]:
        sys.exit(f"sdl_vendor: {name}.cmake has a substitution this tool does not know")
    with open(os.path.join(dest, name), "w") as f:
        f.write(text)


README = """\
# {port} -- vendored, not written here

**This directory is third-party source, copied by
`tools/fetch/sdl_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/third_party/{port}/patches/`, which the tool applies to the copy.
SaunaOS's own SDL code -- the build configuration, the time and
filesystem modules, the drivers -- is in `userland/third_party/sdl3/glue/`.

- **Upstream**: {url}
- **SHA-256 of the tarball**: `{sha}`
- **Files**: {files}
- **Licence**: `LICENSE.txt`, upstream's own file (zlib).{patches}
"""
FILES = {
    "sdl3": "the translation units in `sources.mk` (SDL's CMake globs, and one "
            "directory per driver SaunaOS picks), the headers they reach (`gcc -MM`), "
            "and the public `include/` tree whole.",
    "sdl2compat": "`src/sdl2_compat.c` and `src/dynapi/SDL_dynapi.c`, the headers they "
                  "reach, the public `include/` tree whole, and `include/SDL2/SDL_config_unix.h` "
                  "as its CMake would write it (X11 off).",
}


def write_readme(port, dest, url, sha):
    notes = []
    for p in sorted(glob.glob(os.path.join(THIRD_PARTY, port, "patches", "*.patch"))):
        with open(p) as f:
            notes.append(f"  - `{os.path.basename(p)}` -- {f.readline().strip()}")
    patches = ("\n- **Patched, by SaunaOS**, from `userland/third_party/" + port +
               "/patches/`:\n" + "\n".join(notes)) if notes else ""
    with open(os.path.join(dest, "README.md"), "w") as f:
        f.write(README.format(port=port, url=url, sha=sha, files=FILES[port], patches=patches))


def apply_patches(port, dest):
    pdir = os.path.join(THIRD_PARTY, port, "patches")
    for p in sorted(glob.glob(os.path.join(pdir, "*.patch"))):
        with open(p) as f:
            r = subprocess.run(["patch", "-p1", "--no-backup-if-mismatch", "-d", dest],
                               stdin=f, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"sdl_vendor: {os.path.basename(p)} does not apply:\n{r.stdout}{r.stderr}")
        print(f"sdl_vendor: applied {os.path.basename(p)}")


def write_sources(dest, port, var, unit_list):
    with open(os.path.join(dest, "sources.mk"), "w") as f:
        f.write(f"# Written by tools/fetch/sdl_vendor.py: {port}'s translation units for\n"
                "# SaunaOS's configuration, which is not every .c in it.\n")
        f.write(f"{var} = \\\n")
        f.write(" \\\n".join(f"\tuserland/third_party/{port}/upstream/{u}" for u in unit_list))
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", default=DEFAULT_WORK, help="where the tarballs are unpacked")
    args = ap.parse_args()

    trees = {port: fetch(args.work, port, top, url, sha) for port, top, url, sha in PINS}
    pins = {port: (url, sha) for port, _, url, sha in PINS}
    flags = compile_flags(trees["sdl3"])

    u3 = units(trees["sdl3"], SDL3_UNITS, SDL3_EXCLUDE)
    dest, n = copy_port(trees["sdl3"], "sdl3", set(u3) | headers(trees["sdl3"], u3, flags),
                        "include")
    apply_patches("sdl3", dest)
    write_sources(dest, "sdl3", "SDL3_SRCS", u3)
    write_readme("sdl3", dest, *pins["sdl3"])
    print(f"sdl_vendor: sdl3 -- {len(u3)} units, {n} files")

    flags2 = flags + ["-I" + os.path.join(trees["sdl2compat"], "include"),
                      "-I" + os.path.join(trees["sdl2compat"], "include/SDL2")]
    u2 = units(trees["sdl2compat"], SDL2COMPAT_UNITS)
    dest, n = copy_port(trees["sdl2compat"], "sdl2compat",
                        set(u2) | headers(trees["sdl2compat"], u2, flags2), "include")
    apply_patches("sdl2compat", dest)
    configure_sdl2_config(trees["sdl2compat"], dest)
    write_sources(dest, "sdl2compat", "SDL2COMPAT_SRCS", u2)
    write_readme("sdl2compat", dest, *pins["sdl2compat"])
    print(f"sdl_vendor: sdl2compat -- {len(u2)} units, {n} files")


if __name__ == "__main__":
    main()
