#!/usr/bin/env bash
# tools/build/build_native_make.sh -- GNU make that RUNS ON SaunaOS.
#
# docs/cc-design.md, stage 4: GNU make, cross-built by the x86_64-saunaos
# cross toolchain (`make toolchain`) against the SDK, into the SAME staged
# tree the compiler is in (toolchain/native/usr/bin/make), so the seed
# step that carries the compiler to /usr carries make with it. `make
# native-toolchain` runs this after tools/build/build_native_toolchain.sh;
# NOGPL=1 never seeds either.
#
#   tools/build/build_native_make.sh [--cross DIR] [--sysroot DIR] [--jobs N]
#                                    [--force] [--keep]
#
# --keep leaves the build tree in toolchain/build-native-make/.
#
# MAKE IS NOT PATCHED. What this system lacks is answered at configure
# time, each answer below with its reason; a gap in the C library is
# filled in tolibc, not worked around here.
#
# A SECOND RUN IS A NO-OP: the stamp records the pin, this script, the
# configure line and the cross toolchain's own stamp. The compiler's
# build deletes the staged tree when it rebuilds, stamp and all, so a
# rebuilt compiler rebuilds make after it.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TC=$REPO/toolchain
CROSS=$TC/local
SYSROOT=$REPO/build/sysroot
OUT=$TC/native
WORK=$TC/build-native-make
JOBS=$(nproc)
FORCE=0
KEEP=0
TARGET=x86_64-saunaos
MAKEPKG=make-4.4.1

while [ $# -gt 0 ]; do
    case $1 in
        --cross)   CROSS=$2; shift 2 ;;
        --sysroot) SYSROOT=$2; shift 2 ;;
        --jobs)    JOBS=$2; shift 2 ;;
        --force)   FORCE=1; shift ;;
        --keep)    KEEP=1; shift ;;
        -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build_native_make: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case $CROSS in /*) ;; *) CROSS=$REPO/$CROSS ;; esac
case $SYSROOT in /*) ;; *) SYSROOT=$REPO/$SYSROOT ;; esac

say() { echo "build_native_make: $*"; }
die() { echo "build_native_make: $*" >&2; exit 1; }

[ -x "$CROSS/bin/$TARGET-gcc" ] ||
    die "no cross toolchain at $CROSS -- run \`make toolchain\` first"
[ -f "$SYSROOT/usr/lib/libc.a" ] || die "no SDK at $SYSROOT -- run \`make sdk\` first"
python3 "$REPO/tools/fetch/fetch_toolchain.py" --set make --verify ||
    die "the make tarball is missing or wrong. \`make native-toolchain EXTRAS=1\` fetches it (it shows the licence first)."

# What /usr/bin gains. tools/checks/check_docs.py reads THIS LIST, as it
# reads build_native_toolchain.sh's, for the commands that need a
# docs/commands/ page.
KEEP_BIN=(make)

BUILD_TRIPLE=$(gcc -dumpmachine)
FPM="-ffile-prefix-map=$WORK/src=/usr/src"
# --disable-load: `load` opens a shared object into make, and this make
# is static (as every compiled program here is). --without-guile: no
# Guile for SaunaOS. --disable-nls: no gettext catalogues on the disk.
# -std=gnu17: 4.4.1's lib/fnmatch.c declares `extern char *getenv ();`,
# which GCC 16's default C23 reads as taking no arguments (the GMP
# reason in build_native_toolchain.sh; make's git has since fixed it).
# -DPOSIX: make decides "this is a POSIX system" from <unistd.h>'s
# _POSIX_VERSION, and without it declares getcwd() and friends K&R-style
# itself and wants a d_ino in every dirent. tolibc does not define
# _POSIX_VERSION -- it would claim all of POSIX.1-2008 -- and makeint.h
# names a predefined POSIX as the other way to say so.
# CXX=g++: make compiles no C++; configure's C++ compiler becomes only
# the built-in default of $(CXX), and a cross build would bake in
# x86_64-saunaos-g++, a name the machine does not have. g++ is GNU make's
# default on a GNU system, and /usr/bin has it. (configure checks it
# compiles: the HOST's g++, which every host building the toolchain has.)
MAKE_CONF=(--build="$BUILD_TRIPLE" --host="$TARGET" --prefix=/usr
    --disable-nls --disable-load --without-guile --without-customs
    --without-dmalloc
    CPPFLAGS="-DPOSIX=1" CFLAGS="-O2 -std=gnu17 $FPM" CXX=g++)

STAMP=$OUT/.saunaos-native-make
want=$( {
    python3 "$REPO/tools/fetch/fetch_toolchain.py" --set make --pins
    cat "$0"
    # make links tolibc statically, so a libc it would link differently
    # (fcntl's locks, 2026-11-09) is a new make -- the compiler's stamp
    # carries the SDK through the cross toolchain's; make's says so here.
    sha256sum "$SYSROOT/usr/lib/libc.a"
    echo "${MAKE_CONF[@]}"
    cat "$CROSS/.saunaos-toolchain" 2>/dev/null || true
} | sha256sum | cut -d' ' -f1)
if [ $FORCE = 0 ] && [ -x "$OUT/usr/bin/make" ] && [ -f "$STAMP" ] &&
   [ "$(cat "$STAMP")" = "$want" ]; then
    say "up to date ($OUT/usr/bin/make)"
    exit 0
fi

start=$(date +%s)
rm -rf "$WORK"
mkdir -p "$WORK/src" "$WORK/build"
export PATH=$CROSS/bin:$PATH
log=$WORK/build.log
: > "$log"
run() { "$@" >> "$log" 2>&1 || die "failed: $* -- see ${log#"$REPO"/}"; }

say "unpacking $MAKEPKG"
tar -C "$WORK/src" -xf "$TC/tarballs/$MAKEPKG".tar.*
# make 4.4.1's config.sub predates the target's name. It gets the one
# line toolchain/patches/gcc/ adds to GCC's copy (the GMP rule in
# build_native_toolchain.sh copies that file whole; GCC's source is not
# unpacked here, and the patch's other hunk names GCC's copyright line).
sub=$WORK/src/$MAKEPKG/build-aux/config.sub
sed -i 's/| serenity\* \\$/| serenity* | saunaos* \\/' "$sub"
grep -q 'saunaos\*' "$sub" || die "$MAKEPKG's config.sub has no OS list to add saunaos* to"

say "configure + build (-j$JOBS; log: ${log#"$REPO"/})"
(cd "$WORK/build" && run "$WORK/src/$MAKEPKG/configure" "${MAKE_CONF[@]}")
(cd "$WORK/build" && run make -j"$JOBS")

mkdir -p "$OUT/usr/bin" "$OUT/usr/share/licenses"
for b in "${KEEP_BIN[@]}"; do
    cp "$WORK/build/$b" "$OUT/usr/bin/$b"
    "$CROSS/bin/$TARGET-strip" "$OUT/usr/bin/$b"
done

# THE LICENCE TRAVELS WITH THE BINARY, as it does for the compiler.
cp "$WORK/src/$MAKEPKG/COPYING" "$OUT/usr/share/licenses/make.txt"
{
    echo "/usr/bin/make is GNU make ${MAKEPKG#make-}, built for SaunaOS, unmodified."
    echo "GPL-3.0-or-later (make.txt)."
    echo
    echo "The source is the GNU release tarball, pinned in"
    echo "tools/fetch/fetch_toolchain.py (--set make), plus"
    echo "tools/build/build_native_make.sh in SaunaOS's repository (SOURCE)."
} > "$OUT/usr/share/licenses/native-make.txt"

if [ $KEEP = 0 ]; then
    for d in "$WORK"/*; do [ "$d" = "$log" ] || rm -rf "$d"; done
fi
echo "$want" > "$STAMP"
say "built $OUT/usr/bin/make in $(( $(date +%s) - start )) s ($(du -h "$OUT/usr/bin/make" | cut -f1))"
