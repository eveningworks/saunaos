#!/usr/bin/env python3
"""Strip the DWARF from every program and shared library staged for an image.

    python3 tools/strip_staged.py seed/sync

The build compiles userland with -g, and the copies under build/ keep it
for gdb (tools/gdb/saunaos.py), panic_resolve.py and crash reports resolved
on the host. Nothing on SaunaOS reads DWARF, and it was most of the staged
tree: 131 of 186 MiB, every byte of it read off a CD by GRUB on a live
boot. `objcopy --strip-debug` keeps .symtab, so a crash report still names
functions on the machine itself.

Only executables and shared objects (ELF e_type 2 and 3): a kernel module
(.ko, a relocatable object) is linked by the kernel's loader and is left
exactly as built. The Makefile's `seed` runs this unless STRIP=0.
"""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ET_EXEC, ET_DYN = 2, 3


def is_program(path):
    try:
        with open(path, "rb") as f:
            head = f.read(18)
    except OSError:
        return False
    return (len(head) == 18 and head[:4] == b"\x7fELF"
            and int.from_bytes(head[16:18], "little") in (ET_EXEC, ET_DYN))


def strip(path):
    r = subprocess.run(["objcopy", "--strip-debug", path],
                       capture_output=True, text=True)
    return path, r.returncode, r.stderr.strip()


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: strip_staged.py DIR...")
    files = [os.path.join(d, n) for top in sys.argv[1:]
             for d, _, names in os.walk(top) for n in names]
    files = [f for f in files if not os.path.islink(f) and is_program(f)]
    before = sum(os.path.getsize(f) for f in files)
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        failed = [(p, e) for p, rc, e in ex.map(strip, files) if rc]
    for p, e in failed:
        print(f"strip_staged: {p}: {e}", file=sys.stderr)
    after = sum(os.path.getsize(f) for f in files)
    print(f"  strip_staged: {len(files)} programs, {before >> 20} MiB -> {after >> 20} MiB")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
