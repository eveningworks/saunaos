#!/bin/sh
# quiet_sh.sh -- make's SHELL for a quiet build (the default; `make V=1`
# or VERBOSE=1 bypasses it): Linux kbuild's short lines, one per command
# that builds something --
#
#     CC      build/kernel/proc/scheduler.o
#     LD      build/kernel.pass2.elf
#     ISO     saunaos.iso
#
# -- and nothing for the rest (mkdir, echo, cmp). Every command still
# runs exactly as written, through /bin/sh, and keeps its own output and
# exit status: a warning or an error prints as it always did. Only the
# ISO step's tool chatter is held back, and shown in full if it fails.
#
# make hands each recipe line over as `quiet_sh.sh -c '<line>'`, and its
# $(shell ...) calls too -- see the label's comment below.
if [ "$1" != "-c" ] || [ $# -ne 2 ]; then exec /bin/sh "$@"; fi
cmd=$2

# The first thing that builds, from the line's words: a compiler with -c
# (CC) or without (LINK), the linker, the assembler, the archiver,
# grub-mkrescue, a generator under tools/gen. The output is the word
# after -o, else the generator's name.
set -f
tag=
out=
prev=
for w in $cmd; do
    case $prev in -o) [ -z "$out" ] && out=$w ;; esac
    case $w in
        *gcc|*g++|cc|c++|*-cc|clang) [ -z "$tag" ] && tag=LINK ;;
        -c) [ "$tag" = LINK ] && tag=CC ;;
        ld|*-ld|ld.*) [ -z "$tag" ] && tag=LD ;;
        nasm) [ -z "$tag" ] && tag=AS ;;
        ar|*-ar) [ -z "$tag" ] && tag=AR && out= ;;
        *grub-mkrescue) [ -z "$tag" ] && tag=ISO ;;
        tools/gen/*.py) [ -z "$tag" ] && tag=GEN && out=${w#tools/gen/} ;;
    esac
    # AR names its archive first, with no -o: the word after `rcs`.
    case $prev in rcs|rc|crs|rcsD) [ "$tag" = AR ] && [ -z "$out" ] && out=$w ;; esac
    prev=$w
done
set +f

# TO STDERR, and only with a real output: make runs its $(shell ...)
# calls through this too, and their STDOUT is a variable's value --
# `$(shell gcc -print-file-name=include)` must not come back with a
# label in it. A probe writing to /dev/null is no build step either.
case $out in ''|/dev/null) tag= ;; esac
if [ -n "$tag" ]; then printf '  %-7s %s\n' "$tag" "$out" >&2; fi

if [ "$tag" = ISO ]; then
    log=$(mktemp) || exec /bin/sh -c "$cmd"
    /bin/sh -c "$cmd" >"$log" 2>&1
    rc=$?
    [ $rc -ne 0 ] && cat "$log"
    rm -f "$log"
    exit $rc
fi
exec /bin/sh -c "$cmd"
