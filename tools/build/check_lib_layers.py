#!/usr/bin/env python3
"""Refuse a shared library that uses a symbol from a library to its RIGHT.

    tools/build/check_lib_layers.py build/lib/libnet.so \\
        --below build/lib/libbase.so build/lib/libc.so \\
        --exe build/userland/libs/rt/sys.o ... build/userland/libc_nonshared.a \\
        --search build/userland-pic/libs

The toolkit is four libraries in an ORDER, base < net < media < ui
(docs/tree-design.md, "Named libraries"): each may use the ones to its
left and never one to its right. The Makefile runs this after linking
each one, and deletes the library when it fails, so an edge pointing
right is a BUILD failure, not a review comment.

WHY NOT `ld -z defs`, which is the linker's own form of this check:
every library here leaves the runtime's symbols undefined ON PURPOSE.
`sys_*`, `__errno_location`, `environ`, the stack canary and pthread
live in each EXECUTABLE (rt/sys.o, libc_nonshared.a -- libc.so has the
same shape), so `-z defs` fails every library on symbols that are
right. This is `-z defs` with the executable's side named: every
undefined, non-weak dynamic symbol of the library must be defined by a
`--below` library or an `--exe` object, or it is a finding.

`--search` names the directory of the other libraries' objects, so a
finding says which module the symbol belongs to. Exits 1 on a finding.
"""
import argparse
import os
import subprocess
import sys


def nm(path, *flags):
    r = subprocess.run(["nm", *flags, path], capture_output=True, text=True, check=False)
    if r.returncode != 0:
        sys.exit(f"check_lib_layers: nm {path} failed:\n{r.stderr}")
    return r.stdout.splitlines()


def undefined(so):
    """Non-weak undefined dynamic symbols (`U`; a weak one is `w`/`v`)."""
    out = set()
    for line in nm(so, "-D", "--undefined-only"):
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U":
            out.add(parts[1].split("@")[0])
    return out


def defined(path):
    flags = ("-D", "--defined-only") if path.endswith(".so") else ("-g", "--defined-only")
    out = set()
    for line in nm(path, *flags):
        parts = line.split()
        if len(parts) == 3:
            out.add(parts[2].split("@")[0])
    return out


def owners(search, names):
    """Which object under `search` defines each of `names`."""
    found = {}
    for d, _dirs, files in os.walk(search):
        for f in sorted(files):
            if f.endswith(".o"):
                p = os.path.join(d, f)
                for n in defined(p) & names:
                    found.setdefault(n, os.path.relpath(p, search))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("so", help="the library just linked")
    ap.add_argument("--below", nargs="*", default=[], help="the libraries it may use")
    ap.add_argument("--exe", nargs="*", default=[], help="what every executable provides")
    ap.add_argument("--search", default=None, help="objects to name a finding's owner from")
    a = ap.parse_args()
    have = set()
    for p in a.below + a.exe:
        have |= defined(p)
    missing = undefined(a.so) - have
    if not missing:
        return 0
    who = owners(a.search, missing) if a.search else {}
    name = os.path.basename(a.so)
    print(f"check_lib_layers: {name} uses {len(missing)} symbol(s) no library to its left defines "
          "-- an edge pointing right (base < net < media < ui):", file=sys.stderr)
    for n in sorted(missing):
        print(f"  {n}" + (f"  (defined in {who[n]})" if n in who else ""), file=sys.stderr)
    print("Move the caller up a library, or the callee down -- docs/tree-design.md.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
