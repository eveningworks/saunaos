#!/usr/bin/env bash
# tools/build/build_scummvm.sh -- ScummVM, cross-built for SaunaOS.
#
# docs/sdl-design.md, stage 4: ScummVM 2026.4.0 on its stock SDL2
# backend (sdl2-compat over SDL3, SaunaOS's platform layer), built by
# the x86_64-saunaos cross toolchain (`make toolchain`) against the SDK,
# with zlib, libogg, libvorbis, libpng and FreeType built first and linked
# in STATICALLY (they reach the SDK when a second port needs them). libpng
# and FreeType are the Remastered theme's: without them ScummVM cannot
# load its images or fonts and draws its plain built-in theme. Staged as the
# image's /usr in toolchain/scummvm/usr, which the seed step copies onto
# the image while it exists (not under NOGPL=1); `make scummvm` runs it.
#
#   tools/build/build_scummvm.sh [--cross DIR] [--sysroot DIR] [--jobs N]
#                                [--force] [--keep]
#
# --keep leaves the build tree in toolchain/build-scummvm/.
#
# ONLY FOUR ENGINES: sky, queen, lure, drascula -- the four freeware
# games the card downloads. Another is one name in ENGINES.
#
# OUR CHANGES TO SCUMMVM are userland/third_party/scummvm/patches/*.patch,
# applied -p1 in name order; the first line of each says what it does.
#
# A SECOND RUN IS A NO-OP: the stamp records the pins, this script, the
# patches, the SDK's libraries and the cross toolchain's stamp.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TC=$REPO/toolchain
CROSS=$TC/local
SYSROOT=$REPO/build/sysroot
OUT=$TC/scummvm
WORK=$TC/build-scummvm
PATCHES=$REPO/userland/third_party/scummvm/patches
JOBS=$(nproc)
FORCE=0
KEEP=0
TARGET=x86_64-saunaos
SCUMMVM=scummvm-2026.4.0
ZLIB=zlib-1.3.1
OGG=libogg-1.3.6
VORBIS=libvorbis-1.3.7
PNG=libpng-1.6.59
FT=freetype-2.15.0
ENGINES=sky,queen,lure,drascula

while [ $# -gt 0 ]; do
    case $1 in
        --cross)   CROSS=$2; shift 2 ;;
        --sysroot) SYSROOT=$2; shift 2 ;;
        --jobs)    JOBS=$2; shift 2 ;;
        --force)   FORCE=1; shift ;;
        --keep)    KEEP=1; shift ;;
        -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build_scummvm: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case $CROSS in /*) ;; *) CROSS=$REPO/$CROSS ;; esac
case $SYSROOT in /*) ;; *) SYSROOT=$REPO/$SYSROOT ;; esac

say() { echo "build_scummvm: $*"; }
die() { echo "build_scummvm: $*" >&2; exit 1; }

[ -x "$CROSS/bin/$TARGET-g++" ] ||
    die "no cross toolchain at $CROSS -- run \`make toolchain\` first"
[ -f "$SYSROOT/usr/lib/libSDL2.so" ] || die "no SDK with SDL2 at $SYSROOT -- run \`make sdk\` first"

BUILD_TRIPLE=$(gcc -dumpmachine)
DEPS=$WORK/deps
FPM="-ffile-prefix-map=$WORK/src=/usr/src"

# Everything ScummVM can find but this build leaves out, said once so
# a host library never sneaks in (configure probes by compiling). MP3,
# FLAC, JPEG and the rest are no game's need here; the
# network ones need sockets ScummVM's SDL backend would reach through
# SDL_net or curl, which the SDK has neither of.
SCUMMVM_CONF=(--host="$TARGET" --prefix=/usr --backend=sdl
    --enable-release --disable-debug --enable-optimizations
    --disable-all-engines --enable-engine="$ENGINES"
    --with-sdl-prefix="$SYSROOT/usr"
    --with-zlib-prefix="$DEPS" --with-ogg-prefix="$DEPS" --with-vorbis-prefix="$DEPS"
    --enable-png --with-png-prefix="$DEPS" --enable-freetype2 --with-freetype2-prefix="$DEPS"
    --disable-tremor --disable-mad --disable-flac --disable-jpeg
    --disable-gif --disable-theoradec --disable-vpx --disable-faad --disable-mpeg2
    --disable-a52 --disable-mikmod --disable-openmpt --disable-mpcdec
    --disable-fluidsynth --disable-fluidlite --disable-sonivox --disable-opl2lpt
    --disable-retrowave --disable-nfm --disable-fribidi
    --disable-readline --disable-libunity --disable-gtk --disable-sndio
    --disable-sdlnet --disable-libcurl --disable-enet --disable-discord
    --disable-cloud --disable-tts --disable-alsa --disable-seq-midi
    --disable-timidity --disable-opengl-game --disable-opengl-game-classic
    --disable-opengl-game-shaders --disable-nasm --disable-sparkle
    --disable-eventrecorder --disable-system-dialogs --disable-taskbar)

STAMP=$OUT/.saunaos-scummvm
want=$( {
    python3 "$REPO/tools/fetch/fetch_scummvm.py" --pins
    cat "$0"
    cat "$PATCHES"/*.patch 2>/dev/null || true
    sha256sum "$SYSROOT/usr/lib/libc.so" "$SYSROOT/usr/lib/libSDL2.so"
    echo "${SCUMMVM_CONF[@]}"
    cat "$CROSS/.saunaos-toolchain" 2>/dev/null || true
} | sha256sum | cut -d' ' -f1)
if [ $FORCE = 0 ] && [ -x "$OUT/usr/bin/scummvm" ] && [ -f "$STAMP" ] &&
   [ "$(cat "$STAMP")" = "$want" ]; then
    say "up to date ($OUT/usr/bin/scummvm)"
    exit 0
fi

# Hashing 216 MiB is seconds of work, so only a build that will happen
# pays it: the stamp above holds the pins already.
python3 "$REPO/tools/fetch/fetch_scummvm.py" --verify ||
    die "a ScummVM tarball is missing or wrong. \`make scummvm EXTRAS=1\` fetches them (it shows the licences first)."

start=$(date +%s)
rm -rf "$WORK" "$OUT"
mkdir -p "$WORK/src" "$DEPS"
export PATH=$CROSS/bin:$PATH
log=$WORK/build.log
: > "$log"
run() { "$@" >> "$log" 2>&1 || die "failed: $* -- see ${log#"$REPO"/}"; }

for p in $ZLIB $OGG $VORBIS $PNG $FT $SCUMMVM; do
    say "unpacking $p"
    tar -C "$WORK/src" -xf "$TC"/tarballs/$p.tar.*
done

# The autotools packages' config.sub predates the target's name. libogg's
# is the newer and lists serenity*; it gets saunaos* beside it (the line
# toolchain/patches/gcc/ adds to GCC's copy) and serves libvorbis too.
sub=$WORK/src/$OGG/config.sub
sed -i 's/| serenity\* \\$/| serenity* | saunaos* \\/' "$sub"
grep -q 'saunaos\*' "$sub" || die "$OGG's config.sub has no OS list to add saunaos* to"
cp "$sub" "$WORK/src/$VORBIS/config.sub"
cp "$sub" "$WORK/src/$PNG/config.sub"
cp "$sub" "$WORK/src/$FT/builds/unix/config.sub"
# ScummVM ships its own, as new; without the name its configure reads the
# host OS as empty and treats SaunaOS as a non-POSIX unknown.
sub=$WORK/src/$SCUMMVM/config.sub
sed -i 's/| serenity\* \\$/| serenity* | saunaos* \\/' "$sub"
grep -q 'saunaos\*' "$sub" || die "$SCUMMVM's config.sub has no OS list to add saunaos* to"

CC="$TARGET-gcc"
DEP_CFLAGS="-O2 -fPIC $FPM"

say "zlib, libogg, libvorbis, libpng, FreeType (static; log: ${log#"$REPO"/})"
# zlib's configure is its own, not autoconf: CHOST names the cross tools.
(cd "$WORK/src/$ZLIB" && CHOST=$TARGET CFLAGS="$DEP_CFLAGS" run ./configure --static --prefix="$DEPS" &&
    run make -j"$JOBS" libz.a && run make install)
for p in $OGG $VORBIS; do
    (cd "$WORK/src/$p" && run ./configure --build="$BUILD_TRIPLE" --host="$TARGET" \
        --prefix="$DEPS" --disable-shared --enable-static --disable-docs --disable-examples \
        --with-ogg="$DEPS" CFLAGS="$DEP_CFLAGS" PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" &&
        run make -j"$JOBS" && run make install)
done
(cd "$WORK/src/$PNG" && run ./configure --build="$BUILD_TRIPLE" --host="$TARGET" \
    --prefix="$DEPS" --disable-shared --enable-static --disable-tools \
    CFLAGS="$DEP_CFLAGS" CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" &&
    run make -j"$JOBS" && run make install)
# freetype-config is asked for: ScummVM finds FreeType through it here.
(cd "$WORK/src/$FT" && run ./configure --build="$BUILD_TRIPLE" --host="$TARGET" \
    --prefix="$DEPS" --disable-shared --enable-static --enable-freetype-config \
    --with-zlib=yes --without-bzip2 --without-png --without-harfbuzz --without-brotli \
    CFLAGS="$DEP_CFLAGS" ZLIB_CFLAGS="-I$DEPS/include" ZLIB_LIBS="-L$DEPS/lib -lz" \
    PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" &&
    run make -j"$JOBS" && run make install)
# THEIR .pc FILES GO: ScummVM's configure prefers pkg-config, which runs
# under the SDK's PKG_CONFIG_SYSROOT_DIR and would prefix these paths with
# it. Without them it takes --with-png-prefix and freetype-config instead.
rm -f "$DEPS"/lib/pkgconfig/libpng*.pc "$DEPS"/lib/pkgconfig/freetype2.pc
# ...and FreeType's own freetype-config asks /usr/bin/pkg-config, the
# HOST's, baked in at its configure: it answered with the dev host's
# FreeType. This one knows only the deps (ScummVM passes --prefix,
# --static and --cflags/--libs; it reads stdout alone).
cat > "$DEPS/bin/freetype-config" <<EOS
#!/bin/sh
for a; do
    case \$a in
        --cflags) echo "-I$DEPS/include/freetype2" ;;
        --libs)   echo "-L$DEPS/lib -lfreetype -L$DEPS/lib -lz" ;;
    esac
done
EOS
chmod +x "$DEPS/bin/freetype-config"

src=$WORK/src/$SCUMMVM
for p in "$PATCHES"/*.patch; do
    [ -f "$p" ] || continue
    say "patch $(basename "$p")"
    (cd "$src" && run patch -p1 -i "$p")
done

say "configure + build ScummVM ($ENGINES; -j$JOBS)"
# sdl2-config is the SDK's; SDL's headers are found through the sysroot
# (=/usr/include/SDL2), which the cross compiler reads as the sysroot.
(cd "$src" && SDL_CONFIG="$SYSROOT/usr/bin/sdl2-config" CXX="$TARGET-g++" \
    CXXFLAGS="-O2 $FPM" PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="$SYSROOT" run ./configure "${SCUMMVM_CONF[@]}")
(cd "$src" && run make -j"$JOBS")
(cd "$src" && run make install DESTDIR="$OUT")

# What the image needs of `make install`: the program, its data (themes,
# translations, the engines' data files), the licences. Not the man page,
# the desktop file and icons (SaunaOS has its own), nor the docs.
"$CROSS/bin/$TARGET-strip" "$OUT/usr/bin/scummvm"
rm -rf "$OUT/usr/share/man" "$OUT/usr/share/applications" "$OUT/usr/share/icons" \
       "$OUT/usr/share/pixmaps" "$OUT/usr/share/metainfo" "$OUT/usr/share/doc"
# ...nor what only a left-out part reads: the OpenGL shaders (no OpenGL
# here), the 3D engines' theme, and the Mac GUI's fonts and widgets --
# nor fonts-cjk.dat (37 MB), which only the launcher's Chinese, Japanese
# and Korean translations read; fonts.dat (6 MB) is the theme's own.
rm -f "$OUT"/usr/share/scummvm/{shaders.dat,residualvm.zip,classicmacfonts.dat,macgui.dat,fonts-cjk.dat}
mkdir -p "$OUT/usr/share/licenses"
cp "$src/COPYING" "$OUT/usr/share/licenses/scummvm.txt"
cp "$WORK/src/$OGG/COPYING" "$OUT/usr/share/licenses/libogg.txt"
cp "$WORK/src/$VORBIS/COPYING" "$OUT/usr/share/licenses/libvorbis.txt"
cp "$WORK/src/$PNG/LICENSE" "$OUT/usr/share/licenses/libpng.txt"
cp "$WORK/src/$FT/docs/FTL.TXT" "$OUT/usr/share/licenses/freetype.txt"
cp "$src/LICENSES/COPYING.OFL" "$OUT/usr/share/licenses/liberation-fonts.txt"
sed -n '/^ Copyright/,/^ *Mark Adler/p' "$WORK/src/$ZLIB/README" > "$OUT/usr/share/licenses/zlib.txt"
{
    echo "/usr/bin/scummvm is ScummVM ${SCUMMVM#scummvm-}, built for SaunaOS with the engines"
    echo "$ENGINES. GPL-3.0-or-later (scummvm.txt); linked statically with"
    echo "$ZLIB (zlib.txt), $OGG (libogg.txt) and $VORBIS (libvorbis.txt)."
    echo
    echo "The source is ScummVM's release tarball and those three, pinned in"
    echo "tools/fetch/fetch_scummvm.py, plus userland/third_party/scummvm/patches/"
    echo "and tools/build/build_scummvm.sh in SaunaOS's repository (SOURCE)."
} > "$OUT/usr/share/licenses/scummvm-build.txt"

if [ $KEEP = 0 ]; then
    for d in "$WORK"/*; do [ "$d" = "$log" ] || rm -rf "$d"; done
fi
echo "$want" > "$STAMP"
say "built $OUT/usr/bin/scummvm in $(( $(date +%s) - start )) s ($(du -h "$OUT/usr/bin/scummvm" | cut -f1))"
