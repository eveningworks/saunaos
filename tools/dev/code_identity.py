#!/usr/bin/env python3
r"""Do two builds contain the same code? The check after a source-tree move.

    tools/dev/code_identity.py OLD_BUILD_DIR [NEW_BUILD_DIR]   # default: build/
    tools/dev/code_identity.py OLD --rename 'userland/gui/(.*)/(\w+)\.elf=userland/apps/\1/\2/\2.elf'

docs/tree-design.md's stages move files without changing them, so every
binary must carry the same code. A byte compare is too strict: a moved
file's `__FILE__` string grows, which shifts `.rodata` and every
RIP-relative `lea` into it, and a changed link order (`find | sort` over
moved files) moves functions and the alignment padding between them.

For every ELF in both trees (`*.elf`, `*.so`, `kernel.bin`):
  1. identical `.text` bytes -> same;
  2. otherwise, every function's disassembly with addresses, immediates,
     symbol names in `<...>` and trailing padding (`nop`, `int3`) masked,
     compared as a MULTISET of (name, body) -- static functions share
     names across files, so a name-keyed map would compare the wrong
     copies -> same when equal, a finding naming the functions otherwise.

`--rename REGEX=REPLACEMENT` (repeatable) maps an old tree's relative
path onto the new one's, for a stage that moved the ELFs themselves.

Exits 1 if any binary differs, or is in one tree only. A green run is
only worth something once a red one has been seen: point it at a tree
where one ELF was swapped for another program's.
"""
import argparse
import collections
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAD = re.compile(r"^(cs )*(nop|xchg\s+%ax,%ax|int3)")
ADDR = re.compile(r"^\s+[0-9a-f]+:")
LABEL = re.compile(r"^[0-9a-f]+ <([^>]+)>:")


def elves(top):
    found = {}
    for d, _dirs, files in os.walk(top):
        for f in files:
            if f.endswith((".elf", ".so")) or f == "kernel.bin":
                p = os.path.join(d, f)
                found[os.path.relpath(p, top)] = p
    return found


def text_bytes(path):
    r = subprocess.run(["objcopy", "-O", "binary", "--only-section=.text", path, "/dev/stdout"],
                       capture_output=True, check=False)
    return hashlib.sha256(r.stdout).hexdigest()


def bodies(path):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True, check=False).stdout
    res, cur, name = [], None, None

    def close():
        while cur and PAD.match(cur[-1]):
            cur.pop()
        res.append((name, hashlib.md5("\n".join(cur).encode()).hexdigest()))

    for line in out.splitlines():
        m = LABEL.match(line)
        if m:
            if cur is not None:
                close()
            name, cur = m.group(1), []
        elif cur is not None and ADDR.match(line):
            ins = line.split(":", 1)[1]
            ins = re.sub(r"#.*", "", ins)
            ins = re.sub(r"-?0x[0-9a-f]+", "N", ins)   # a moved target can flip the sign
            ins = re.sub(r"\b[0-9a-f]{5,}\b", "A", ins)
            ins = re.sub(r"<[^>]*>", "<>", ins)
            cur.append(ins.strip())
    if cur is not None:
        close()
    return collections.Counter(res)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("old", help="the build directory from before the move")
    ap.add_argument("new", nargs="?", default=os.path.join(ROOT, "build"))
    ap.add_argument("--rename", action="append", default=[], metavar="REGEX=REPLACEMENT",
                    help="map an old relative path to its new one (re.fullmatch)")
    a = ap.parse_args()
    old, new = elves(a.old), elves(a.new)
    for rule in a.rename:
        pat, rep = rule.split("=", 1)
        rx = re.compile(pat)
        old = {(rx.sub(rep, k) if rx.fullmatch(k) else k): v for k, v in old.items()}
    bad = [f"  only in {a.old}: {k}" for k in sorted(old.keys() - new.keys())]
    bad += [f"  only in {a.new}: {k}" for k in sorted(new.keys() - old.keys())]
    same = masked = 0
    for k in sorted(old.keys() & new.keys()):
        if text_bytes(old[k]) == text_bytes(new[k]):
            same += 1
            continue
        x, y = bodies(old[k]), bodies(new[k])
        if x == y:
            masked += 1
        else:
            names = sorted({n for n, _ in (x - y) + (y - x)})
            bad.append(f"  DIFFERS {k}: {len(names)} function(s): {', '.join(names[:8])}"
                       + (" ..." if len(names) > 8 else ""))
    print(f"code_identity: {same} identical, {masked} identical once addresses and padding "
          f"are masked, {len(bad)} problem(s)")
    print("\n".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
