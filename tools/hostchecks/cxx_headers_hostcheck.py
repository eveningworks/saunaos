#!/usr/bin/env python3
"""Every header the SDK ships outside libc -- the toolkit's base/ net/
media/ ui/, rt/sys.h and the kernel headers they reach -- is usable from
C++: it COMPILES as C++ on its own, and every function it declares has C
LINKAGE.

TWO CHECKS, BECAUSE A COMPILE SEES ONLY ONE OF THE TWO FAULTS. A header
with no `extern "C"` compiles as C++ without a word, and the program then
fails to LINK, every call mangled. So:

  1. LINKAGE. gcc's -aux-info lists every function a header declares, by
     line; each non-static one must sit inside the header's
     `#ifdef __cplusplus / extern "C" {` block, and no #include may (a C
     header pulled in from inside one may be libstdc++'s wrapper, whose
     templates cannot have C linkage -- docs/conventions/build.md).
  2. SYNTAX. The host g++ compiles `#include <the header>` as C++17 with
     -Wall, against tolibc's headers as the tree includes them: no C-only
     keyword (`_Static_assert` needs the fallback base/uchan_page.h has),
     no C++ keyword as a name (`private`, `new`, `class`).

The set is not a list: it is tools/build/stage_sdk.py's own -- the
toolkit's directories, rt/sys.h, and the compiler's -M closure over them
into kernel/include. libc's headers are tools/tests/sweep/cross_gxx_test.py's,
against the cross compiler; this one needs only the host's.

`--positive-control` runs both checks over two broken copies -- ui/uapp.h
with its extern "C" block taken out, base/uchan_page.h given a field
named `class` -- and both must FAIL.

    python3 tools/hostchecks/cxx_headers_hostcheck.py
    python3 tools/hostchecks/cxx_headers_hostcheck.py --positive-control
"""

RUNNER = {"covers": "every SDK header outside libc compiles as C++ and declares its functions with C linkage",
          "parallel": True, "gate": True,
          "needs": ["host_cxx", "needs gcc and g++ on PATH"]}

import argparse
import atexit
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import stage_sdk  # noqa: E402
from harness import Results  # noqa: E402

ROOT = toolpath.REPO
# ABSOLUTE, and every compile runs in an EMPTY directory: a quoted
# #include read from stdin searches the working directory before any -I,
# which made the repo's own copy shadow the positive control's.
INC = ["-I" + os.path.join(ROOT, d) for d in ("userland/libs/libc/include", "kernel/include/api",
       "kernel/include/abi", "build/gen", "userland", "userland/libs", ".")]
EMPTY = tempfile.mkdtemp(prefix="cxx_headers.")
atexit.register(shutil.rmtree, EMPTY, ignore_errors=True)
OPEN = re.compile(r'^\s*extern\s+"C"\s*\{')
CLOSE = re.compile(r"^\s*\}\s*(//.*)?$")
CPP_IF = re.compile(r"^\s*#\s*ifdef\s+__cplusplus\b")
INCLUDE = re.compile(r"^\s*#\s*include\b")


def shipped():
    """Repo paths of every header the SDK ships, less libc's."""
    pub = stage_sdk.public_headers()
    ker = stage_sdk.kernel_closure("build", "gcc", pub)
    return sorted(p for p in set(pub.values()) | set(ker.values())
                  if not p.startswith("userland/libs/libc/") and not p.startswith("build/"))


def overlay_flags(overlay):
    """An overlay holds copies at their REPO paths; searched first both as
    the repo root and as userland/libs, so "ui/x.h" opens the copy too."""
    return ["-I" + overlay, "-I" + os.path.join(overlay, "userland/libs")] if overlay else []


def declarations(paths, overlay=None):
    """{repo path: [line, ...]} of every non-static function declared, from
    one C compile of them all with -aux-info."""
    pre = overlay_flags(overlay)
    with tempfile.TemporaryDirectory() as tmp:
        aux = os.path.join(tmp, "aux")
        src = "".join(f'#include "{p}"\n' for p in paths)
        r = subprocess.run(["gcc", "-std=gnu11", "-ffreestanding", "-fsyntax-only", "-aux-info", aux,
                            *pre, *INC, "-x", "c", "-"], input=src, capture_output=True, text=True, cwd=EMPTY)
        if r.returncode:
            sys.exit(f"cxx_headers: the headers do not compile as C:\n{r.stderr[-2000:]}")
        out = {}
        for ln in open(aux):
            m = re.match(r"/\* (\S+?):(\d+):\w+ \*/ (.*)", ln)
            if not m or m.group(3).startswith("static "):
                continue
            f = os.path.normpath(m.group(1))
            for base in ([overlay] if overlay else []) + [ROOT]:
                if f.startswith(os.path.normpath(base) + os.sep):
                    f = os.path.relpath(f, base)
                    break
            out.setdefault(f, []).append(int(m.group(2)))
    return out


def linkage_faults(path, lines, text):
    """What is wrong with `path`'s C linkage, or [] when nothing is."""
    L = text.split("\n")
    opens = [i + 1 for i, l in enumerate(L) if OPEN.match(l) and i and CPP_IF.match(L[i - 1])]
    if not lines:
        return []
    if not opens:
        return [f"{path}: declares {len(lines)} function(s) (first at line {min(lines)}) "
                f"with no `#ifdef __cplusplus / extern \"C\" {{` block"]
    start = opens[0]
    closes = [i + 1 for i, l in enumerate(L) if CLOSE.match(l) and i > start and CPP_IF.match(L[i - 1])]
    if not closes:
        return [f"{path}: its extern \"C\" block at line {start} is never closed under #ifdef __cplusplus"]
    end = closes[-1]
    faults = [f"{path}:{n}: declared outside the extern \"C\" block (lines {start}-{end})"
              for n in lines if not start < n < end]
    faults += [f"{path}:{i + 1}: an #include inside the extern \"C\" block"
               for i in range(start, end - 1) if INCLUDE.match(L[i])]
    return faults


def syntax_fault(path, overlay=None):
    pre = overlay_flags(overlay)
    r = subprocess.run(["g++", "-std=c++17", "-Wall", "-ffreestanding", "-fsyntax-only", *pre, *INC,
                        "-x", "c++", "-"], input=f'#include "{path}"\nint main() {{ return 0; }}\n',
                       capture_output=True, text=True, cwd=EMPTY)
    msg = (r.stderr or "").strip()
    if r.returncode or msg:
        first = next((l for l in msg.splitlines() if "error" in l or "warning" in l), msg[:200])
        return f"{path}: {first}"
    return None


def check(paths, overlay=None):
    """(linkage faults, syntax faults) over `paths`, reading each from
    `overlay` when a copy is there."""
    decl = declarations(paths, overlay)

    def text(p):
        o = os.path.join(overlay, p) if overlay else None
        return open(o if o and os.path.exists(o) else os.path.join(ROOT, p)).read()

    link = [f for p in paths for f in linkage_faults(p, decl.get(p, []), text(p))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        syn = [f for f in ex.map(lambda p: syntax_fault(p, overlay), paths) if f]
    return link, syn


def positive_control(res):
    """The ordinary checks over two broken copies: both must go red."""
    no_block, keyword = "userland/libs/ui/uapp.h", "userland/libs/base/uchan_page.h"
    with tempfile.TemporaryDirectory() as ov:
        for p in (no_block, keyword):
            os.makedirs(os.path.join(ov, os.path.dirname(p)), exist_ok=True)
        t = open(os.path.join(ROOT, no_block)).read()
        t = re.sub(r'#ifdef __cplusplus\nextern "C" \{\n#endif\n', "", t)
        t = re.sub(r"#ifdef __cplusplus\n\}\n#endif\n", "", t)
        open(os.path.join(ov, no_block), "w").write(t)
        t = open(os.path.join(ROOT, keyword)).read()
        t = t.replace("#include <stdint.h>\n", "#include <stdint.h>\nstruct cxx_control { int class; };\n", 1)
        open(os.path.join(ov, keyword), "w").write(t)
        link, syn = check([no_block, keyword], overlay=ov)
    res.check("uapp.h's functions have C linkage", not any(f.startswith(no_block) for f in link),
              "\n".join(link))
    res.check("uchan_page.h compiles as C++", not any(f.startswith(keyword) for f in syn), "\n".join(syn))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break two copies (no extern \"C\" block, a keyword as a name): both must be reported")
    args = ap.parse_args()
    if not (shutil.which("gcc") and shutil.which("g++")):
        print("cxx_headers: SKIP -- needs gcc and g++")
        return 77
    res = Results()
    if args.positive_control:
        positive_control(res)
        print(f"\ncxx_headers --positive-control: {len(res.passes)} passed, {len(res.fails)} failed"
              " (both must FAIL)")
        return 1 if res.fails else 0
    paths = shipped()
    link, syn = check(paths)
    res.check(f"every function in the {len(paths)} shipped headers has C linkage", not link, "\n".join(link[:40]))
    res.check(f"each of the {len(paths)} shipped headers compiles as C++ on its own", not syn, "\n".join(syn[:40]))
    print(f"\ncxx_headers: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
