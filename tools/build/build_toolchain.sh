#!/usr/bin/env bash
# tools/build/build_toolchain.sh -- the x86_64-saunaos cross toolchain.
#
# GCC and binutils, taught the x86_64-saunaos target by
# toolchain/patches/<pkg>/*.patch, built OFFLINE from the pinned tarballs
# in toolchain/tarballs/ (fetched by `make toolchain EXTRAS=1`;
# tools/fetch/fetch_toolchain.py) into toolchain/local/. docs/cc-design.md,
# stages 1 and 2, is the plan; `make toolchain` is the way in. C and C++:
# libstdc++ is built static, against tolibc, and installs in the prefix.
#
#   tools/build/build_toolchain.sh [--prefix DIR] [--sysroot DIR] [--jobs N] [--force]
#
# THE SYSROOT IS A SYMLINK INSIDE THE PREFIX. GCC and ld are configured
# --with-sysroot=<prefix>/x86_64-saunaos/sysroot, a path under the
# prefix, which both treat as RELOCATABLE (found relative to the
# binaries). The symlink points at the checkout's build/sysroot, which
# `make sdk` writes and `make clean` deletes -- so the compiler survives a
# clean and sees the next `make sdk` without a rebuild. An absolute
# --with-sysroot would bake one checkout's path into the binaries.
# Stage 3's compiler on the machine is configured with `/` instead; the
# specs name every path through %R, so they carry over unchanged.
#
# A SECOND RUN IS A NO-OP: the stamp in the prefix records the pins, the
# patches, this script and the configure lines; any change rebuilds.
#
# GMP, MPFR and MPC are the HOST's libraries (the compiler runs on the
# host); docs/development-setup.md names the packages.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TC=$REPO/toolchain
PREFIX=$TC/local
SYSROOT=$REPO/build/sysroot
JOBS=$(nproc)
FORCE=0
TARGET=x86_64-saunaos
BINUTILS=binutils-2.47
GCC=gcc-16.2.0

while [ $# -gt 0 ]; do
    case $1 in
        --prefix)  PREFIX=$2; shift 2 ;;
        --sysroot) SYSROOT=$2; shift 2 ;;
        --jobs)    JOBS=$2; shift 2 ;;
        --force)   FORCE=1; shift ;;
        -h|--help) sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build_toolchain: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case $PREFIX in /*) ;; *) PREFIX=$REPO/$PREFIX ;; esac
case $SYSROOT in /*) ;; *) SYSROOT=$REPO/$SYSROOT ;; esac

say() { echo "build_toolchain: $*"; }
die() { echo "build_toolchain: $*" >&2; exit 1; }

# A prefix that is not this checkout's own is somebody else's build:
# use it, never rebuild it from here.
if [ "$PREFIX" != "$TC/local" ]; then
    [ -x "$PREFIX/bin/$TARGET-gcc" ] ||
        die "TOOLCHAIN=$PREFIX has no bin/$TARGET-gcc -- build it in its own checkout"
    say "using $PREFIX (TOOLCHAIN=), not building"
    exit 0
fi

python3 "$REPO/tools/fetch/fetch_toolchain.py" --verify ||
    die "the source tarballs are missing or wrong. \`make toolchain EXTRAS=1\` fetches them (it shows their licence first)."

BINUTILS_CONF=(--target="$TARGET" --prefix="$PREFIX"
    --with-sysroot="$PREFIX/$TARGET/sysroot"
    --disable-nls --disable-werror --disable-gprofng --disable-gdb)
GCC_CONF=(--target="$TARGET" --prefix="$PREFIX"
    --with-sysroot="$PREFIX/$TARGET/sysroot"
    --enable-languages=c,c++ --disable-nls --disable-werror --disable-multilib
    --disable-shared --disable-lto --enable-threads=posix --enable-initfini-array
    --disable-libstdcxx-pch --enable-libstdcxx-time=yes
    --disable-libssp --disable-libquadmath --disable-libgomp
    --disable-libatomic --disable-libsanitizer --disable-libvtv)

# THE SDK IS PART OF THE KEY. libgcc's and libstdc++'s configure probe
# the sysroot -- compile tests against its headers, link tests against
# libc.a -- and bake the answers into the install (c++config.h's
# _GLIBCXX_USE_C99_MATH_FUNCS, say). Keyed on the patches alone, a libc
# that gained <math.h>'s C99 half left libstdc++ configured without it
# and "up to date". The headers count by content; libc.a by the NAMES it
# defines, which is all a link test asks, so an ordinary libc edit does
# not cost a rebuild and a new function does.
sdk_key() {
    [ -d "$SYSROOT/usr/include" ] || return 0
    (cd "$SYSROOT/usr/include" && find . -type f -print0 | sort -z | xargs -0 sha256sum)
    [ -f "$SYSROOT/usr/lib/libc.a" ] &&
        nm --defined-only -g "$SYSROOT/usr/lib/libc.a" 2>/dev/null | awk 'NF == 3 { print $3 }' | sort -u
}
STAMP=$PREFIX/.saunaos-toolchain
want=$( {
    cat "$REPO/tools/fetch/fetch_toolchain.py" "$0"
    cat "$TC"/patches/binutils/*.patch "$TC"/patches/gcc/*.patch
    echo "${BINUTILS_CONF[@]}" "${GCC_CONF[@]}"
    sdk_key
} | sha256sum | cut -d' ' -f1)
if [ $FORCE = 0 ] && [ -x "$PREFIX/bin/$TARGET-gcc" ] && [ -f "$STAMP" ] &&
   [ "$(cat "$STAMP")" = "$want" ]; then
    say "up to date ($PREFIX)"
    exit 0
fi

# libgcc is compiled against the C library's headers, so they must exist.
[ -f "$SYSROOT/usr/include/stdio.h" ] && [ -f "$SYSROOT/usr/lib/libc.a" ] ||
    die "no SDK at $SYSROOT -- run \`make sdk\` first (libgcc compiles against its headers)"

start=$(date +%s)
rm -rf "$TC/build" "$PREFIX"
mkdir -p "$TC/build/src" "$PREFIX/$TARGET"
ln -s "$(realpath --relative-to="$PREFIX/$TARGET" "$SYSROOT")" "$PREFIX/$TARGET/sysroot"

for pkg in $BINUTILS $GCC; do
    say "unpacking $pkg"
    tar -C "$TC/build/src" -xf "$TC/tarballs/$pkg.tar.xz"
    for p in "$TC/patches/${pkg%%-*}"/*.patch; do
        say "  applying ${p#"$REPO"/}"
        patch -d "$TC/build/src/$pkg" -p1 --forward --quiet < "$p" ||
            die "${p#"$REPO"/} does not apply to $pkg"
    done
done

export PATH=$PREFIX/bin:$PATH
log=$TC/build/build.log
: > "$log"
run() { "$@" >> "$log" 2>&1 || die "failed: $* -- see ${log#"$REPO"/}"; }

say "binutils: configure + build (-j$JOBS; log: ${log#"$REPO"/})"
mkdir -p "$TC/build/binutils"
(cd "$TC/build/binutils" && run "$TC/build/src/$BINUTILS/configure" "${BINUTILS_CONF[@]}" &&
    run make -j"$JOBS" && run make install-strip)

say "gcc: configure + build"
mkdir -p "$TC/build/gcc"
(cd "$TC/build/gcc" && run "$TC/build/src/$GCC/configure" "${GCC_CONF[@]}" &&
    run make -j"$JOBS" all-gcc all-target-libgcc all-target-libstdc++-v3 &&
    run make install-strip-gcc install-target-libgcc install-strip-target-libstdc++-v3)

# The build trees are ~3 GB and every rebuild starts from scratch, so
# nothing reuses them; the log stays.
rm -rf "$TC/build/src" "$TC/build/binutils" "$TC/build/gcc"
echo "$want" > "$STAMP"
say "built $PREFIX/bin/$TARGET-gcc in $(( $(date +%s) - start )) s ($(du -sh "$PREFIX" | cut -f1))"
