#!/usr/bin/env bash
# tools/build/build_native_toolchain.sh -- GCC and binutils that RUN ON SaunaOS.
#
# docs/cc-design.md, stage 3: a Canadian cross -- built on this Linux host,
# by the x86_64-saunaos cross toolchain (`make toolchain`), into programs
# whose host AND target are x86_64-saunaos. `make native-toolchain` is the
# way in; the result is a staged tree, toolchain/native/usr/..., that the
# image's seed step copies to /usr when it exists (and NOGPL=1 never does).
#
#   tools/build/build_native_toolchain.sh [--cross DIR] [--sysroot DIR] [--jobs N]
#                                         [--force] [--keep] [--resume]
#
# --keep leaves the build tree in toolchain/build-native/; --resume takes
# up a kept or failed one at the stage that did not finish.
#
# WHAT IS BUILT HERE AND WHAT IS COPIED. GMP, MPFR and MPC (static, for
# SaunaOS), binutils, and GCC's compilers and driver are built. The TARGET
# runtime -- libgcc, libstdc++, libsupc++ and their headers -- is COPIED
# from the cross prefix: it was built by the same GCC for the same target
# against the same SDK, and a second build would be the same bytes.
#
# THE SPECS ARE THE CROSS GCC'S (gcc/config/saunaos.h, the one patch); the
# native compiler is --prefix=/usr --with-sysroot=/, so %R/usr/lib is
# /usr/lib on the machine. `as` and `ld` are named absolutely
# (--with-as/--with-ld), so the driver does not depend on PATH.
#
# A SECOND RUN IS A NO-OP: the stamp records the pins, the patches, this
# script, the configure lines and the cross toolchain's own stamp.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TC=$REPO/toolchain
CROSS=$TC/local
SYSROOT=$REPO/build/sysroot
OUT=$TC/native
WORK=$TC/build-native
JOBS=$(nproc)
FORCE=0
KEEP=0
RESUME=0
TARGET=x86_64-saunaos
BINUTILS=binutils-2.47
GCC=gcc-16.2.0
GMP=gmp-6.3.0
MPFR=mpfr-4.2.2
MPC=mpc-1.3.1
GCCVER=16.2.0

while [ $# -gt 0 ]; do
    case $1 in
        --cross)   CROSS=$2; shift 2 ;;
        --sysroot) SYSROOT=$2; shift 2 ;;
        --jobs)    JOBS=$2; shift 2 ;;
        --force)   FORCE=1; shift ;;
        --keep)    KEEP=1; shift ;;
        --resume)  RESUME=1; KEEP=1; FORCE=1; shift ;;
        -h|--help) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build_native_toolchain: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case $CROSS in /*) ;; *) CROSS=$REPO/$CROSS ;; esac
case $SYSROOT in /*) ;; *) SYSROOT=$REPO/$SYSROOT ;; esac

say() { echo "build_native_toolchain: $*"; }
die() { echo "build_native_toolchain: $*" >&2; exit 1; }

[ -x "$CROSS/bin/$TARGET-g++" ] ||
    die "no cross toolchain at $CROSS -- run \`make toolchain\` first"
[ -f "$SYSROOT/usr/lib/libc.a" ] || die "no SDK at $SYSROOT -- run \`make sdk\` first"
python3 "$REPO/tools/fetch/fetch_toolchain.py" --set native --verify ||
    die "the source tarballs are missing or wrong. \`make native-toolchain EXTRAS=1\` fetches them (it shows their licences first)."

# What /usr/bin keeps. tools/checks/check_docs.py reads THIS LIST for the
# commands that need a docs/commands/ page, so a program added here and
# not documented fails the build. Everything else binutils and GCC
# install (ld.bfd, gprof, elfedit, addr2line, c++filt, the
# target-prefixed copies, the LTO wrappers, gcov) is left out.
KEEP_BIN=(gcc g++ cc c++ cpp as ld ar ranlib nm objdump objcopy readelf strip size strings)

BUILD_TRIPLE=$(gcc -dumpmachine)
HOSTLIBS=$WORK/hostlibs
# The build tree's path is not the image's business: GCC's assertions
# name their source files, and this keeps this host's directories out of
# every binary (and the bytes the same from any checkout).
FPM="-ffile-prefix-map=$WORK/src=/usr/src"

# -std=gnu17: GMP 6.3.0's configure probes declare `void g(){}` and call
# it with arguments, an error in GCC 16's default C23 -- so every compiler
# it tries is "not working". The flag is what distributions pass.
LIB_CONF=(--build="$BUILD_TRIPLE" --host="$TARGET" --prefix="$HOSTLIBS"
    --disable-shared --enable-static CFLAGS="-O2 -std=gnu17 $FPM")
BINUTILS_CONF=(--build="$BUILD_TRIPLE" --host="$TARGET" --target="$TARGET"
    --prefix=/usr --with-sysroot=/ --disable-nls --disable-werror
    --disable-gprofng --disable-gdb --disable-gdbserver --disable-sim
    --disable-plugins --disable-install-libbfd --disable-libctf
    --without-zstd --without-debuginfod --without-msgpack
    CFLAGS="-O2 $FPM" CXXFLAGS="-O2 $FPM")
GCC_CONF=(--build="$BUILD_TRIPLE" --host="$TARGET" --target="$TARGET"
    --prefix=/usr --with-sysroot=/ --with-build-sysroot="$SYSROOT"
    --with-as=/usr/bin/as --with-ld=/usr/bin/ld
    --with-gmp="$HOSTLIBS" --with-mpfr="$HOSTLIBS" --with-mpc="$HOSTLIBS"
    --without-isl --without-zstd --disable-fixincludes
    --enable-languages=c,c++ --disable-nls --disable-werror --disable-multilib
    --disable-shared --disable-lto --disable-plugin --enable-threads=posix
    --enable-initfini-array --disable-libstdcxx-pch --enable-libstdcxx-time=yes
    --disable-libssp --disable-libquadmath --disable-libgomp
    --disable-libatomic --disable-libsanitizer --disable-libvtv
    CFLAGS="-O2 $FPM" CXXFLAGS="-O2 -DCODY_NETWORKING=0 $FPM")
# --disable-fixincludes: the target's headers are never fixed (the GCC
# patch's mkfixinc.sh), so the host copy of the tool would be dead
# weight -- and it times its server with alarm(), which this system has
# no SIGALRM for. include-fixed/ comes from the cross prefix instead.
# CODY_NETWORKING=0: libcody, the C++ modules mapper's protocol, speaks
# it over a socket as well as a pipe and a file, and its socket half
# wants <netdb.h>'s getaddrinfo(), which tolibc does not have (name
# resolution is libnet's, in ring 3 above libc). Without it, `-fmodules`
# still works: the mapper is in-process, as it is by default.

STAMP=$OUT/.saunaos-native-toolchain
want=$( {
    python3 "$REPO/tools/fetch/fetch_toolchain.py" --set native --pins
    cat "$0" "$TC"/patches/binutils/*.patch "$TC"/patches/gcc/*.patch
    echo "${LIB_CONF[@]}" "${BINUTILS_CONF[@]}" "${GCC_CONF[@]}"
    cat "$CROSS/.saunaos-toolchain" 2>/dev/null || true
} | sha256sum | cut -d' ' -f1)
if [ $FORCE = 0 ] && [ -x "$OUT/usr/bin/gcc" ] && [ -f "$STAMP" ] &&
   [ "$(cat "$STAMP")" = "$want" ]; then
    say "up to date ($OUT)"
    exit 0
fi

start=$(date +%s)
if [ $RESUME = 0 ] || [ ! -f "$WORK/src.done" ]; then rm -rf "$WORK"; fi
rm -rf "$OUT"
mkdir -p "$WORK/src" "$OUT"
export PATH=$CROSS/bin:$PATH
log=$WORK/build.log
: > "$log"
run() { "$@" >> "$log" 2>&1 || die "failed: $* -- see ${log#"$REPO"/}"; }

# One stage: skipped when a previous run finished it (--resume), its
# build directory started afresh otherwise.
stage() {
    local name=$1; shift
    if [ -f "$WORK/$name.done" ]; then say "$name: done before, skipped"; return; fi
    say "$name: configure + build (-j$JOBS; log: ${log#"$REPO"/})"
    rm -rf "${WORK:?}/$name"
    mkdir -p "$WORK/$name"
    (cd "$WORK/$name" && "$@")
    touch "$WORK/$name.done"
}

if [ ! -f "$WORK/src.done" ]; then
    for pkg in $BINUTILS $GCC $GMP $MPFR $MPC; do
        say "unpacking $pkg"
        tar -C "$WORK/src" -xf "$TC/tarballs/$pkg".tar.*
    done
    for pkg in $BINUTILS $GCC; do
        for p in "$TC/patches/${pkg%%-*}"/*.patch; do
            say "  applying ${p#"$REPO"/}"
            patch -d "$WORK/src/$pkg" -p1 --forward --quiet < "$p" ||
                die "${p#"$REPO"/} does not apply to $pkg"
        done
    done
    # GMP, MPFR and MPC predate the target's name. The config.sub the GCC
    # patch already taught it to is copied over theirs -- the same lines,
    # not a patch of their own (GMP's config.sub wraps configfsf.sub).
    cp "$WORK/src/$GCC/config.sub" "$WORK/src/$GMP/configfsf.sub"
    cp "$WORK/src/$GCC/config.sub" "$WORK/src/$MPFR/config.sub"
    cp "$WORK/src/$GCC/config.sub" "$WORK/src/$MPC/build-aux/config.sub"
    touch "$WORK/src.done"
fi

# GMP's assembly is off: its x86-64 mpn routines assume a host ABI
# (direct references to its own tables) that -mno-direct-extern-access
# and a base above 4 GiB do not promise, and the C fallback is what
# every unknown CPU gets. GCC's constant folding is not where a compile
# spends its time.
build_gmp() {
    run "$WORK/src/$GMP/configure" "${LIB_CONF[@]}" --disable-assembly
    run make -j"$JOBS"
    run make install
}
build_mpfr() {
    run "$WORK/src/$MPFR/configure" "${LIB_CONF[@]}" --with-gmp="$HOSTLIBS"
    run make -j"$JOBS"
    run make install
}
build_mpc() {
    run "$WORK/src/$MPC/configure" "${LIB_CONF[@]}" --with-gmp="$HOSTLIBS" --with-mpfr="$HOSTLIBS"
    run make -j"$JOBS"
    run make install
}
build_binutils() {
    run "$WORK/src/$BINUTILS/configure" "${BINUTILS_CONF[@]}"
    run make -j"$JOBS"
    run make install-strip DESTDIR="$WORK/root"
}
# NO -fno-PIE: gcc/ builds its programs -fno-PIE when the compiler takes
# the flag, which at a base above 4 GiB leaves 32-bit absolute
# relocations ld cannot fit -- and --enable-host-pie would link -pie, an
# ET_DYN the kernel will not load. Answering gcc/configure's two probes
# "no" leaves the specs' own -fpie and a static ET_EXEC link, which is
# what every program here is. Exported: gcc/ is configured by `make`.
build_gcc() {
    export gcc_cv_c_no_fpie=no gcc_cv_no_pie=no
    run "$WORK/src/$GCC/configure" "${GCC_CONF[@]}"
    run make -j"$JOBS" all-gcc
    run make install-strip-gcc DESTDIR="$WORK/root"
}
stage gmp build_gmp
stage mpfr build_mpfr
stage mpc build_mpc
stage binutils build_binutils
stage gcc build_gcc

# --- the staged tree: what the image gets -------------------------------
R=$WORK/root/usr
U=$OUT/usr
mkdir -p "$U/bin" "$U/lib" "$U/include" "$U/libexec/gcc/$TARGET/$GCCVER"
# cc and c++ are HARD LINKS of gcc and g++ (after them in KEEP_BIN): one
# file each on the image too, since tfs3_writer.py's sync makes a link of
# every name the staging tree links. A copy cost 2.4 MB apiece.
for b in "${KEEP_BIN[@]}"; do
    case $b in
        cc)  ln "$U/bin/gcc" "$U/bin/cc"; continue ;;
        c++) ln "$U/bin/g++" "$U/bin/c++"; continue ;;
    esac
    [ -f "$R/bin/$b" ] || die "the install has no bin/$b"
    cp "$R/bin/$b" "$U/bin/$b"
done
# cc1, cc1plus, collect2 -- and nothing of the plugin or LTO machinery,
# which is configured off.
for p in cc1 cc1plus collect2; do
    cp "$R/libexec/gcc/$TARGET/$GCCVER/$p" "$U/libexec/gcc/$TARGET/$GCCVER/"
done
# GCC's own headers (stddef.h, stdarg.h, the x86 intrinsics) and the
# target runtime from the cross prefix, where `make toolchain` put it.
mkdir -p "$U/lib/gcc/$TARGET/$GCCVER"
cp -r "$CROSS/lib/gcc/$TARGET/$GCCVER/include" "$CROSS/lib/gcc/$TARGET/$GCCVER/include-fixed" \
    "$U/lib/gcc/$TARGET/$GCCVER/"
for f in "$CROSS/lib/gcc/$TARGET/$GCCVER"/*.a "$CROSS/lib/gcc/$TARGET/$GCCVER"/*.o; do
    [ -e "$f" ] && cp "$f" "$U/lib/gcc/$TARGET/$GCCVER/"
done
[ -f "$U/lib/gcc/$TARGET/$GCCVER/libgcc.a" ] || die "the cross prefix has no libgcc.a"
for f in "$CROSS/$TARGET/lib"/*.a "$CROSS/$TARGET/lib"/*.modules.json; do
    [ -e "$f" ] && cp "$f" "$U/lib/"
done
[ -f "$U/lib/libstdc++.a" ] || die "the cross prefix has no libstdc++.a"
cp -r "$CROSS/$TARGET/include/c++" "$U/include/"

# The static archives keep their symbols and lose their DWARF, as the
# SDK's do; the programs were stripped by install-strip.
find "$U/lib" -name '*.a' -exec "$CROSS/bin/$TARGET-strip" --strip-debug {} +
find "$U/bin" "$U/libexec" -type f -exec "$CROSS/bin/$TARGET-strip" {} +

# THE LICENCES TRAVEL WITH THE BINARIES, as the seed step does for every
# port: GPL-3 for GCC and binutils with GCC's runtime exception, the LGPL
# for GMP, MPFR and MPC, and where the source is.
L=$U/share/licenses
mkdir -p "$L"
cp "$WORK/src/$GCC/COPYING3" "$L/gcc.txt"
cp "$WORK/src/$GCC/COPYING.RUNTIME" "$L/gcc-runtime-exception.txt"
cp "$WORK/src/$BINUTILS/COPYING3" "$L/binutils.txt"
cp "$WORK/src/$GMP/COPYING.LESSERv3" "$L/gmp.txt"
cp "$WORK/src/$MPFR/COPYING.LESSER" "$L/mpfr.txt"
cp "$WORK/src/$MPC/COPYING.LESSER" "$L/mpc.txt"
{
    echo "The compiler in /usr (cc, c++, as, ld and the rest) is GCC $GCCVER and"
    echo "${BINUTILS/-/ }, built for SaunaOS with GMP, MPFR and MPC linked in."
    echo "GPL-3.0-or-later (gcc.txt, binutils.txt); libgcc and libstdc++ carry the"
    echo "GCC Runtime Library Exception (gcc-runtime-exception.txt), so a program"
    echo "it compiles is its author's. GMP, MPFR, MPC: LGPL-3.0-or-later."
    echo
    echo "The source is the GNU release tarballs, pinned in"
    echo "tools/fetch/fetch_toolchain.py, plus toolchain/patches/ and"
    echo "tools/build/build_native_toolchain.sh in SaunaOS's repository (SOURCE)."
} > "$L/native-toolchain.txt"

# The build tree is gigabytes and an ordinary rebuild starts from
# scratch; the log stays.
if [ $KEEP = 0 ]; then
    for d in "$WORK"/*; do [ "$d" = "$log" ] || rm -rf "$d"; done
    rm -f "$WORK"/*.done
fi
echo "$want" > "$STAMP"
say "built $OUT/usr in $(( $(date +%s) - start )) s ($(du -sh "$OUT" | cut -f1))"
