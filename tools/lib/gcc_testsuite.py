#!/usr/bin/env python3
"""GCC's own run tests as a corpus: gcc.c-torture/execute and libstdc++'s `dg-do run` tests.

Read from the pinned GCC tarball the toolchain already fetched
(toolchain/tarballs/gcc-<ver>.tar.xz), extracted once into
build/gcc_testsuite/ -- nothing is vendored. Each test's DejaGnu
directives become what native_corpus_test.py needs: the flags, the
extra sources, whether it runs at all here and why not, and whether its
run is EXPECTED to fail (`dg-xfail-run-if`, `dg-do run { xfail ... }`).

A directive or effective target this file does not know makes the test
SKIPPED WITH THAT NAME as the reason, never guessed at: an unevaluated
selector counted as true would run tests that were never meant to, and
one counted as false would hide them. `python3 tools/lib/gcc_testsuite.py
--summary` prints the counts and the most common reasons.

The shape follows the two harnesses it stands in for: torture's
execute.exp (`-w`, one torture option -- this tool's `-O2` -- and
dg-options replacing the `-w`), and libstdc++.exp (each test once, at
-std=gnu++20 or the newer standard its `target c++NN` needs, linked with
libtestc++.a built from testsuite/util).
"""

import fnmatch
import os
import re
import shutil
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolpath  # noqa: E402,F401

REPO = toolpath.REPO
GCC_VERSION = "16.2.0"
TARBALL = os.path.join(REPO, "toolchain", "tarballs", f"gcc-{GCC_VERSION}.tar.xz")
CACHE = os.path.join(REPO, "build", "gcc_testsuite")
TORTURE = f"gcc-{GCC_VERSION}/gcc/testsuite/gcc.c-torture/execute"
LIBSTDCXX = f"gcc-{GCC_VERSION}/libstdc++-v3/testsuite"

TRIPLET = "x86_64-pc-saunaos"
DEFAULT_STD = 20     # libstdc++.exp's v3_default_std
MAX_STD = 26         # ... and v3_max_std
STDS = (98, 11, 14, 17, 20, 23, 26)

# libstdc++.exp's libtestc++.a, in its order.
LIBTEST_SOURCES = ("testsuite_abi.cc", "testsuite_allocator.cc", "testsuite_character.cc",
                   "testsuite_hooks.cc", "io/verified_cmd_line_input.cc", "io/prog_bar.cc",
                   "performance/time/elapsed_timer.cc")
LIBSTDCXX_DIRS = ("17_intro", "18_support", "19_diagnostics", "20_util", "21_strings", "22_locale",
                  "23_containers", "24_iterators", "25_algorithms", "26_numerics", "27_io",
                  "28_regex", "29_atomics", "30_threads", "std", "ext", "tr1", "tr2",
                  "experimental", "special_functions", "backward", "decimal")

# What this target IS, for an effective-target name or an old-style
# dg-require-<name>. True and False are claims about SaunaOS on x86-64
# (LP64, static, ELF, hosted, POSIX threads); a name missing from both
# is not evaluated (the test is skipped naming it).
TRUE = {
    # the C side (gcc/testsuite/lib/target-supports.exp)
    "int32plus", "int32", "lp64", "longlong64", "double64", "double64plus", "large_double",
    "large_long_double", "int128", "stdint_types", "stdint_types_mbig_endian_disabled",
    "inttypes_types", "label_values", "indirect_jumps", "indirect_calls", "nonlocal_goto",
    "trampolines", "untyped_assembly", "return_address", "fileio", "c99_runtime", "unwrapped",
    "mmap", "alloca", "ptr32plus", "size32plus", "sync_int_long", "sync_char_short",
    "sync_long_long", "sync_long_long_runtime", "x86", "ieee", "ieee_floats", "le", "little_endian",
    "freorder", "nonpic", "tls", "tls_runtime", "tls_native", "pthread", "signal", "setjmp",
    "clocks", "int_eq_float", "ptr_eq_long", "size_t_eq_long", "wchar", "exceptions", "rtti",
    "builtin_eh_return", "struct_musttail", "musttail", "nonnull_ptr", "scalar_all_fma",
    "lax_strtofp", "stdint", "alias", "weak", "fpic", "size20plus", "static",
    # the C++ library side (libstdc++-v3/testsuite/lib/libstdc++.exp)
    "hosted", "cxx11_abi", "gthreads", "gthreads_timed", "cstdint", "string-conversions",
    "string_conversions", "c-std", "cmath", "atomic-builtins", "atomic-cmpxchg-word",
    "thread-fence", "time", "sleep", "sched-yield", "little-endian", "filesystem-ts",
    "filesystem", "random_device", "std_allocator_new", "c++", "fileio",
    "cxx11_atomic", "libatomic_available", "binary_io", "binary-io", "normal-mode",
    "gthreads-timed", "static-libstdcxx", "exceptions_enabled", "target-fs-lwt",
}
FALSE = {
    "run_expensive_tests",            # the harness's GCC_TEST_RUN_EXPENSIVE, off by default
    "ia32", "ilp32", "x32", "lp64_false", "avr", "pcc_bitfield_type_matters",
    "dfp", "dfprt", "dfp_bid", "dfp_hw",   # not probed: decimal float is not this corpus' question
    "lto", "lto_incremental",          # one torture option, no -flto
    "debug-mode", "debug_mode",        # the testsuite's -D_GLIBCXX_DEBUG mode, not the normal run
    "parallel-mode", "profile-mode",
    "namedlocale", "iconv",           # no named locales, no iconv in tolibc
    "tzdb",                            # no tzdata on the image
    "tbb_backend",                     # no TBB
    "freestanding",
    "net_ts_ip",                       # not built against tolibc's sockets
    "simulator", "ptx", "pthread_h_false",
    "ifunc", "weak_undefined",
    "newlib", "newlib_nano_io", "newlib_broken_long_double_io",
    "sharedlib",                       # every program here links statically
    "futex",                           # libstdc++'s futex path is Linux's syscall
    "swprintf", "dummy_wcsftime",      # tolibc has no wide printf or wcsftime
    "broken_cplxf_arg",
    "target-fs-symlinks", "target-fs-space",   # not claimed for TFS3 here: unmeasured
}
# dg-add-options names that change nothing here, and those that mean a library.
ADD_OPTIONS = {"ieee": [], "c99_runtime": [], "no_pch": [], "tls": [], "bind_pic_locally": [],
               "stack_size": [], "float16": None, "float32": [], "float64": [], "float128": None,
               "float32x": [], "float64x": [], "bfloat16": None, "libatomic": [],
               "strict_std": [], "net_ts": None, "pthread": ["-pthread"], "gthreads": [],
               "cstdint": [], "c99_math": [], "using-deprecated": [], "riscv_zfa": None, "arm_neon": None}


class Case:
    """One test: what to compile, how, and what its run should do."""

    def __init__(self, suite, cid, srcs, flags, root):
        self.suite = suite          # "own" | "torture" | "libstdc++"
        self.cid = cid              # unique, safe as a file name
        self.srcs = srcs            # paths relative to `root`, compiled with cwd = root
        self.flags = flags
        self.root = root            # host directory the relative paths resolve in
        self.libs = []              # link-only arguments
        self.expect = "pass"        # "pass" | "fail" (an xfail'd run)
        self.skip = None            # why it does not run here, or None
        self.timeout_factor = 1.0
        self.data = []              # files copied into the run directory, relative to root
        self.oracle = "exit"        # "exit": its exit status is its verdict; "output": compared

    @property
    def cxx(self):
        return any(s.endswith((".cc", ".cpp", ".C", ".cxx")) for s in self.srcs)

    def __repr__(self):
        return f"<Case {self.cid} {self.srcs} {self.flags} skip={self.skip}>"


# -- extraction ---------------------------------------------------------------

def extract(tarball=TARBALL, cache=CACHE):
    """The two testsuite trees, extracted once; returns the cache root or None.

    Two runs may start together (a corpus run and its positive control),
    so the check and the extraction happen under a lock, into a scratch
    directory renamed into place: no reader ever sees a half-written or
    deleted tree."""
    import fcntl
    if not os.path.exists(tarball):
        return None
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    key = f"{os.path.getsize(tarball)} {int(os.path.getmtime(tarball))}\n"
    stamp = os.path.join(cache, ".stamp")
    with open(cache + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if os.path.exists(stamp) and open(stamp).read() == key:
            return cache
        scratch = cache + ".new"
        shutil.rmtree(scratch, ignore_errors=True)
        os.makedirs(scratch)
        with tarfile.open(tarball) as tf:
            members = [m for m in tf.getmembers()
                       if (m.name.startswith(TORTURE + "/") or m.name.startswith(LIBSTDCXX + "/"))
                       and (m.isfile() or m.isdir())]
            tf.extractall(scratch, members=members, filter="data")
        with open(os.path.join(scratch, ".stamp"), "w") as f:
            f.write(key)
        shutil.rmtree(cache, ignore_errors=True)
        os.rename(scratch, cache)
    return cache


# -- DejaGnu directives ------------------------------------------------------

def _directives(text):
    """Every `{ dg-name args... }` in a file, as (name, [word, ...]); a
    word is a plain token, a "quoted" string or a {braced} group (kept
    as its inner text)."""
    out = []
    for m in re.finditer(r"\{\s*(dg-[\w-]+)", text):
        i, depth = m.start(), 0
        j = i
        while j < len(text):
            c = text[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            elif c == "\n" and depth == 1:
                break
            j += 1
        body = text[m.end():j]
        out.append((m.group(1), _words(body)))
    return out


def _words(s):
    words, i = [], 0
    while i < len(s):
        c = s[i]
        if c.isspace():
            i += 1
        elif c == '"':
            j = i + 1
            while j < len(s) and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            words.append(s[i + 1:j])
            i = j + 1
        elif c == "{":
            depth, j = 0, i
            while j < len(s):
                if s[j] == "{":
                    depth += 1
                elif s[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            words.append(s[i + 1:j].strip())
            i = j + 1
        else:
            j = i
            while j < len(s) and not s[j].isspace():
                j += 1
            words.append(s[i:j])
            i = j
    return words


class Unknown(Exception):
    pass


def _target(name, std):
    """One effective-target name, at language standard `std` (None for C)."""
    m = re.fullmatch(r"c\+\+(\d\d)(_only|_down)?", name)
    if m:
        if std is None:
            return False
        n = int(m.group(1))
        n = n if n != 98 else 3
        s = std if std != 98 else 3
        return s == n if m.group(2) == "_only" else s <= n if m.group(2) == "_down" else s >= n
    if name in ("c",):
        return std is None
    if name in TRUE:
        return True
    if name in FALSE:
        return False
    if re.fullmatch(r"[\w?*.\[\]]+-[\w?*.\[\]]+-[\w?*.\[\]-]+", name) and re.search(r"[*?[]", name):
        return fnmatch.fnmatch(TRIPLET, name) or fnmatch.fnmatch("x86_64-unknown-saunaos", name)
    raise Unknown(name)


def _selector(sel, std):
    """A DejaGnu target selector: a triplet glob, a name, `! x`, `x && y`, `x || y`."""
    toks = _words(sel) if isinstance(sel, str) else sel
    if not toks:
        return True
    if toks[0] in ("target", "xfail") and len(toks) >= 2:
        toks = toks[1:]
    # Operators bind left to right; DejaGnu's own grammar is no richer in practice.
    def atom(t):
        if " " in t.strip():
            return _selector(t, std)
        return _target(t, std)
    if toks[0] == "!":
        val = not atom(toks[1])
        rest = toks[2:]
    else:
        val = atom(toks[0])
        rest = toks[1:]
    while rest:
        if rest[0] in ("&&", "||") and len(rest) >= 2:
            if rest[1] == "!":
                rhs = not atom(rest[2])
                nxt = rest[3:]
            else:
                rhs = atom(rest[1])
                nxt = rest[2:]
            val = (val and rhs) if rest[0] == "&&" else (val or rhs)
            rest = nxt
        else:
            # A list of targets (`{ x86_64-*-* i?86-*-* }`) is an OR.
            val = val or atom(rest[0])
            rest = rest[1:]
    return val


def _split_target_xfail(words):
    """dg-do's second word: `target SEL`, `xfail SEL`, or both."""
    target = xfail = None
    toks = _words(words if isinstance(words, str) else " ".join(words))
    k = 0
    while k < len(toks):
        if toks[k] == "target" and k + 1 < len(toks):
            target = toks[k + 1]
            k += 2
            # `target c++11 xfail *-*-*` arrives flat
            while k < len(toks) and toks[k] not in ("xfail",):
                target += " " + toks[k]
                k += 1
        elif toks[k] == "xfail" and k + 1 < len(toks):
            xfail = " ".join(toks[k + 1:])
            break
        else:
            k += 1
    return target, xfail


def apply_directives(case, text, std, base_flags, torture_opts):
    """Fill `case` from the test's directives; `std` is None for C."""
    opts = list(base_flags)
    extra = []
    try:
        for name, w in _directives(text):
            if name == "dg-do":
                what = w[0] if w else "run"
                if what != "run":
                    case.skip = f"dg-do {what}"
                    return
                if len(w) > 1:
                    target, xfail = _split_target_xfail(w[1:])
                    if target and not _selector(target, std):
                        case.skip = f"not for this standard/target ({target})"
                        return
                    if xfail and _selector(xfail, std):
                        case.expect = "fail"
            elif name in ("dg-options", "dg-additional-options"):
                if len(w) > 1 and not _selector(w[1], std):
                    continue
                flags = w[0].split() if w else []
                if name == "dg-options":
                    opts = flags
                else:
                    extra += flags
            elif name == "dg-add-options":
                for o in w:
                    if o not in ADD_OPTIONS:
                        raise Unknown(f"dg-add-options {o}")
                    if ADD_OPTIONS[o] is None:
                        case.skip = f"dg-add-options {o}"
                        return
                    extra += ADD_OPTIONS[o]
            elif name == "dg-require-effective-target":
                if not w:
                    continue
                if len(w) > 1 and not _selector(w[1], std):
                    continue
                if not _target(w[0], std):
                    case.skip = f"needs {w[0]}"
                    return
            elif name == "dg-require-stack-size" or name == "dg-require-cpp-feature-test":
                continue
            elif name.startswith("dg-require-"):
                what = name[len("dg-require-"):]
                if not _target(what, std):
                    case.skip = f"needs {what}"
                    return
            elif name == "dg-skip-if":
                sel = w[1] if len(w) > 1 else "*-*-*"
                inc = w[2].split() if len(w) > 2 else []
                exc = w[3].split() if len(w) > 3 else []
                if _selector(sel, std):
                    applies = (not inc or "*" in inc or any(o in torture_opts for o in inc)) \
                        and not any(o in torture_opts for o in exc if o)
                    if applies:
                        case.skip = f"dg-skip-if {w[0]!r}" if w and w[0] else "dg-skip-if"
                        return
            elif name == "dg-xfail-run-if":
                sel = w[1] if len(w) > 1 else "*-*-*"
                inc = w[2].split() if len(w) > 2 else []
                if _selector(sel, std) and (not inc or "*" in inc or any(o in torture_opts for o in inc)):
                    case.expect = "fail"
            elif name == "dg-timeout-factor":
                try:
                    case.timeout_factor = float(w[0])
                except (ValueError, IndexError):
                    pass
            elif name == "dg-additional-sources":
                d = os.path.dirname(case.srcs[0])
                case.srcs += [os.path.join(d, s) for s in (w[0].split() if w else [])]
            elif name == "dg-additional-files":
                d = os.path.dirname(case.srcs[0])
                case.data += [os.path.join(d, s) for s in (w[0].split() if w else [])]
            # The rest -- dg-final, dg-output, dg-warning, dg-error,
            # dg-message, dg-prune-output, dg-bogus, dg-excess-errors,
            # dg-xfail-if, dg-shouldfail -- check compiler messages or an
            # output pattern, which this corpus' oracle replaces.
            elif name == "dg-shouldfail":
                case.expect = "fail"
    except Unknown as e:
        case.skip = f"not evaluated: {e}"
        return
    case.flags = torture_opts + opts + extra


# -- the two suites -----------------------------------------------------------

def _safe(rel):
    return re.sub(r"[^A-Za-z0-9_.+-]", "_", rel.replace("/", "__"))


def torture_cases(cache, opt="-O2"):
    root = os.path.join(cache, TORTURE)
    cases = []
    for fn in sorted(os.listdir(root)):
        if not fn.endswith(".c"):
            continue
        c = Case("torture", "t_" + _safe(fn[:-2]), [fn], [], root)
        text = open(os.path.join(root, fn), encoding="latin-1").read()
        apply_directives(c, text, None, ["-w"], [opt])
        if not c.skip and re.search(r'^\s*#\s*include\s+"\.\./', text, re.M):
            c.skip = "includes a file outside gcc.c-torture/execute (not extracted)"
        c.libs = ["-lm"]
        cases.append(c)
    return cases


def _min_std(text):
    """libstdc++.exp's v3-minimum-std: the lowest `target c++NN` named."""
    best = None
    for m in re.finditer(r"dg-(?:do|require-effective-target|options|additional-options)[^\n]*?"
                         r"(?:target\s+\{?\s*)c\+\+(\d\d)\b(?!_)", text):
        n = int(m.group(1))
        n = n if n != 98 else 3
        best = n if best is None else min(best, n)
    return best


def libstdcxx_cases(cache, opt="-O2"):
    root = os.path.join(cache, LIBSTDCXX)
    cases = []
    for top in LIBSTDCXX_DIRS:
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if not fn.endswith(".cc"):
                    continue
                path = os.path.join(dirpath, fn)
                text = open(path, encoding="latin-1").read()
                if not re.search(r"\{\s*dg-do\s+run\b", text):
                    continue
                rel = os.path.relpath(path, root)
                c = Case("libstdc++", "l_" + _safe(rel[:-3]), [rel], [], root)
                explicit = re.search(r"dg-(?:additional-)?options[^\n]*-std=(?:gnu|c)\+\+(\d\d|2a|2b|2c|1z|0x)", text)
                if explicit:
                    std = {"0x": 11, "1z": 17, "2a": 20, "2b": 23, "2c": 26}.get(explicit.group(1))
                    std = std or int(explicit.group(1))
                    stdflag = []
                else:
                    mn = _min_std(text)
                    std = mn if mn and mn > DEFAULT_STD else DEFAULT_STD
                    prefix = "-std=c++" if "strict_std" in text else "-std=gnu++"
                    stdflag = [f"{prefix}{std}"]
                std = 98 if std == 3 else std
                base_flags = stdflag + ["-fmessage-length=0", "-fno-show-column", "-ffunction-sections",
                                        "-fdata-sections", "-D_GNU_SOURCE", '-DLOCALEDIR="."',
                                        "-Iutil"]
                apply_directives(c, text, std, [], [opt] + base_flags)
                c.libs = ["libtestc++.a", "-lm"]
                c.timeout_factor = max(c.timeout_factor, 1.0)
                cases.append(c)
    return sorted(cases, key=lambda c: c.cid)


def libtest_sources():
    return [os.path.join("util", s) for s in LIBTEST_SOURCES]


LIBTEST_FLAGS = ["-O2", f"-std=gnu++{DEFAULT_STD}", "-w", "-fno-lto", "-ffunction-sections",
                 "-fdata-sections", "-D_GNU_SOURCE", "-Iutil", "-I."]


def libstdcxx_data(cache):
    """testsuite/data/*: what libstdc++.exp copies into every run directory."""
    d = os.path.join(cache, LIBSTDCXX, "data")
    return sorted(os.path.join("data", f) for f in os.listdir(d)) if os.path.isdir(d) else []


def main():
    import argparse
    import collections
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--summary", action="store_true", help="count cases and skip reasons")
    ap.add_argument("--show", metavar="CID", help="print one case")
    args = ap.parse_args()
    cache = extract()
    if not cache:
        print(f"gcc_testsuite: no {TARBALL} -- `make toolchain EXTRAS=1` fetches it")
        return 1
    every = torture_cases(cache) + libstdcxx_cases(cache)
    if args.show:
        for c in every:
            if c.cid == args.show:
                print(vars(c))
        return 0
    for suite in ("torture", "libstdc++"):
        cs = [c for c in every if c.suite == suite]
        run = [c for c in cs if not c.skip]
        print(f"{suite}: {len(cs)} tests, {len(run)} to run ({sum(c.expect == 'fail' for c in run)} expected to fail)")
        why = collections.Counter(c.skip for c in cs if c.skip)
        for r, n in why.most_common(25):
            print(f"  {n:5d}  {r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
