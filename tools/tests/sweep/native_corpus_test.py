#!/usr/bin/env python3
"""GCC ON SaunaOS against the cross compiler: a corpus built both ways, compared byte for byte.

docs/cc-design.md, stage 5: a corpus compiled ON the guest by /usr/bin/cc
and c++ (`make native-toolchain`, then `make iso`) and the same sources
compiled on the host by x86_64-saunaos-gcc/g++ -- one GCC, built twice --
must agree, the way GCC's own bootstrap compares its stage 2 and stage 3
objects (`make compare`). The corpus is three suites:

  own        tools/tests/sweep/cc_corpus/: the C library and C++ as a
             program uses them, OUTPUT compared;
  torture    GCC's gcc.c-torture/execute -- self-checking programs that
             abort() on a miscompile;
  libstdc++  libstdc++'s `dg-do run` tests, VERIFY()-checked, linked with
             libtestc++.a built from its testsuite/util (on each side).

The last two are read from the GCC tarball the toolchain fetched
(tools/lib/gcc_testsuite.py turns their DejaGnu directives into flags,
skips and expectations).

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - "the program ran and passed its own checks" passes a guest compiler
    that miscompiles in a way the test does not look at: every OBJECT the
    guest wrote is compared BYTE FOR BYTE with the cross compiler's
    (same source path, same flags, same working-directory-relative names);
  - identical objects say nothing about the guest's ld: every linked
    program's SHA-256 is compared too, and one that differs is not
    excused -- the cross-built binary is run on the same guest and its
    result must equal the guest-built one's;
  - "it ran" says nothing about the C library: each self-checking test's
    verdict is compared with the named list in cc_corpus/known.txt, so a
    NEW failure is red and a known one that starts passing is red too
    (the list must stay true); the host's own gcc+glibc build of each
    test runs alongside, as a third, informational column that says
    whether a failure is SaunaOS's or the test's;
  - a run that never reached the guest: the guest's results come back
    off the image after a clean shutdown, and a case with no result is a
    failure, not a pass.

  python3 tools/tests/sweep/native_corpus_test.py [--instance N] [--kvm] [--full]
  python3 tools/tests/sweep/native_corpus_test.py --kvm --full --report out.tsv
      # every test (about 5,100 runnable): an hour or more, KVM only
  python3 tools/tests/sweep/native_corpus_test.py --positive-control objects
      # the guest compiles with -fno-ident (no .comment): the object check must FAIL
  python3 tools/tests/sweep/native_corpus_test.py --positive-control known
      # known.txt is ignored: the verdict check must FAIL (it names failures)

Without --full it runs a fixed sample (every Nth test by name, `--every`),
sized for TCG. A SKIP (exit 77) unless this checkout built the native
toolchain and seeded it (`make native-toolchain`, then `make iso`); the
GCC suites are left out, said so, when the tarball is absent. Boots its
own guest on a COPY of disk.img, twice when a linked program differs.
"""

RUNNER = {"covers": "a corpus (own + GCC torture + libstdc++) compiled on the guest is byte-identical to the cross compiler's and runs as it does",
          "needs": ["native_cc", "no native toolchain on the image -- "
                    "`make native-toolchain`, then `make iso`"]}

import argparse
import concurrent.futures
import fnmatch
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, copy_disk, poll  # noqa: E402
import gcc_testsuite as gts  # noqa: E402

REPO = toolpath.REPO
HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "cc_corpus")
KNOWN = os.path.join(CORPUS, "known.txt")
XBIN = os.path.join(REPO, "toolchain", "local", "bin")
G = "/cc5"                                   # the guest's tree
SUITE_DIR = {"own": "own", "torture": "torture", "libstdc++": "libstdcxx"}
LIBTEST = "l___libtestc++"                   # the pseudo-case that builds libtestc++.a
RUN_LIMIT_S = 120                            # a test run on the machine, before its timeout_factor
SHARD = 400                                  # entries per directory on the machine


# The cross compiler names a system header by its path under toolchain/local;
# the machine's compiler finds the same header under /usr. Where a header
# puts __FILE__ into code (a libstdc++ assertion), the two objects would
# differ by that string alone, so the CROSS side maps its prefixes to the
# machine's -- reproducible builds' -ffile-prefix-map, the guest untouched.
_XL = os.path.join(REPO, "toolchain", "local")
XMAP = [f"-ffile-prefix-map={_XL}/x86_64-saunaos/include/c++=/usr/include/c++",
        f"-ffile-prefix-map={_XL}/x86_64-saunaos/sysroot/usr/include=/usr/include",
        f"-ffile-prefix-map={_XL}/lib/gcc=/usr/lib/gcc",
        # the cross sysroot is a link to the SDK: GCC records the resolved path
        f"-ffile-prefix-map={REPO}/build/sysroot/usr/include=/usr/include"]
# ...and a `-g` case's compile directory, which the debug line table
# carries: the machine compiles from c.srcdir (xmap_dir()).


def xmap_dir(host_dir, machine_dir):
    return f"-ffile-prefix-map={os.path.realpath(host_dir)}={machine_dir}"


def xtool(name):
    return os.path.join(XBIN, f"x86_64-saunaos-{name}")


# -- the corpus -----------------------------------------------------------------

def own_cases():
    cases = []
    for fn in sorted(os.listdir(CORPUS)):
        path = os.path.join(CORPUS, fn)
        if fn == "corpus_run.c" or fn.startswith("."):
            continue
        if os.path.isdir(path):
            srcs = sorted(os.path.join(fn, s) for s in os.listdir(path) if s.endswith((".c", ".cpp")))
            name = fn
        elif fn.endswith((".c", ".cpp")):
            srcs, name = [fn], fn.rsplit(".", 1)[0]
        else:
            continue
        c = gts.Case("own", "o_" + name, srcs, ["-O2"], CORPUS)
        c.libs = ["-lm"]
        c.oracle = "output"
        cases.append(c)
    return cases


def select(cases, every, only):
    """Every `every`th RUNNABLE case by name (skips stay in the report)."""
    if only:
        rx = re.compile(only)
        return [c for c in cases if rx.search(c.cid)]
    if every <= 1:
        return cases
    run = [c for c in cases if not c.skip]
    keep = {c.cid for c in run[::every]}
    return [c for c in cases if c.skip or c.cid in keep]


# -- the host side ----------------------------------------------------------------

def sh(cmd, cwd, timeout=600):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        return "timeout", ""


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def loaded_sha(path):
    """A hash of what the kernel LOADS from an ELF: the entry, the program
    headers and every PT_LOAD/PT_TLS segment's bytes. The ELF header's
    section-table fields (e_shoff, e_shnum, e_shstrndx) are zeroed first:
    the first segment maps the header, and they move whenever an
    unloaded section does -- the debug info the cross toolchain's libgcc
    carries and the image's (`--strip-debug`, build_native_toolchain.sh)
    does not."""
    import struct
    with open(path, "rb") as f:
        d = bytearray(f.read())
    struct.pack_into("<Q", d, 40, 0)
    struct.pack_into("<H", d, 60, 0)
    struct.pack_into("<H", d, 62, 0)
    entry, phoff = struct.unpack_from("<QQ", d, 24)
    phentsize, phnum = struct.unpack_from("<HH", d, 54)
    h = hashlib.sha256(struct.pack("<Q", entry))
    for i in range(phnum):
        p_type, p_flags, p_off, p_vaddr, _pa, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
        h.update(struct.pack("<IIQQQQ", p_type, p_flags, p_vaddr, p_filesz, p_memsz, p_align))
        if p_type in (1, 7):
            h.update(d[p_off:p_off + p_filesz])
    return h.hexdigest()


def link_args(c, libtest_path):
    return [libtest_path if a == "libtestc++.a" else a for a in c.libs]


def driver_for(c, src):
    return "g++" if src.endswith((".cc", ".cpp")) else "gcc"


def host_build(c, work, libtest):
    """The cross compiler's objects and program for case `c`."""
    od = os.path.join(work, "xobj")
    objs = []
    for i, s in enumerate(c.srcs):
        o = os.path.join(od, f"{c.cid}.{i}.o")
        rc, out = sh([xtool(driver_for(c, s)), *c.flags, *XMAP, xmap_dir(c.root, c.srcdir),
                      "-c", s, "-o", o], c.root)
        if rc != 0:
            return {"ok": False, "stage": "compile", "log": out[-2000:]}
        objs.append(o)
    exe = os.path.join(work, "xexe", c.cid)
    rc, out = sh([xtool("g++" if c.cxx else "gcc"), *c.flags, *objs, *link_args(c, libtest), "-o", exe], c.root)
    if rc != 0:
        return {"ok": False, "stage": "link", "log": out[-2000:], "objs": [sha(o) for o in objs]}
    return {"ok": True, "objs": [sha(o) for o in objs], "exe": sha(exe), "exe_path": exe,
            "loaded": loaded_sha(exe)}


def host_native(c, work, libtest_native, slot):
    """The host's OWN gcc+glibc: informational, never gating."""
    exe = os.path.join(work, "nexe", c.cid)
    objs = []
    for i, s in enumerate(c.srcs):
        o = os.path.join(work, "nobj", f"{c.cid}.{i}.o")
        rc, _ = sh(["g++" if s.endswith((".cc", ".cpp")) else "gcc", *c.flags, "-c", s, "-o", o], c.root)
        if rc != 0:
            return {"status": "no-build"}
        objs.append(o)
    rc, _ = sh(["g++" if c.cxx else "gcc", *c.flags, *objs, *link_args(c, libtest_native), "-pthread", "-o", exe], c.root)
    if rc != 0:
        return {"status": "no-build"}
    rundir = os.path.join(work, "nrun", f"{c.suite}.{slot}")
    rc, out = sh([exe], rundir, timeout=int(60 * c.timeout_factor))
    os.unlink(exe)
    return {"status": str(rc), "out": out}


def build_libtest(cache, work, tool_cc, ar, tag):
    """libtestc++.a from testsuite/util, as libstdc++.exp builds it."""
    root = os.path.join(cache, gts.LIBSTDCXX)
    objs, shas, log = [], [], ""
    for i, s in enumerate(gts.libtest_sources()):
        o = os.path.join(work, f"{tag}obj", f"{LIBTEST}.{i}.o")
        xm = [*XMAP, xmap_dir(root, f"{G}/src/{SUITE_DIR['libstdc++']}")] if tag == "x" else []
        rc, out = sh([tool_cc, *gts.LIBTEST_FLAGS, *xm, "-c", s, "-o", o], root)
        if rc == 0:
            objs.append(o)
            shas.append(sha(o))
        else:
            shas.append(None)
            log += f"{s}: {out[-600:]}\n"
    a = os.path.join(work, f"libtestc++-{tag}.a")
    if os.path.exists(a):
        os.unlink(a)
    subprocess.run([ar, "rcD", a, *objs], check=True)
    return a, shas, log


# -- the image --------------------------------------------------------------------

class Image:
    """The disk copy's root volume through tfs3_writer, opened per batch."""

    def __init__(self, path):
        import mkpart_test
        self.path = path
        self.lba, self.sectors = mkpart_test.volume_of(path)

    def open(self, writable):
        import tfs3_writer as tw
        self.tw = tw
        self.img = tw.Tfs3Image(self.path, writable=writable, base_lba=self.lba, sectors=self.sectors)
        self.dirs = set()
        return self

    def close(self):
        self.img.close()

    def mkdirs(self, path):
        parts = path.strip("/").split("/")
        for k in range(1, len(parts) + 1):
            p = "/" + "/".join(parts[:k])
            if p not in self.dirs:
                self.tw.mkdir_path(self.img, self.tw.disk_path(p))
                self.dirs.add(p)

    def write(self, path, data):
        self.mkdirs(os.path.dirname(path))
        self.tw.write_file(self.img, data, self.tw.disk_path(path))

    def read(self, path):
        ino = self.img.lookup(self.tw.disk_path(path))
        return None if ino is None else self.img.read_file_data(ino)

    def listdir(self, path):
        ino = self.img.lookup(self.tw.disk_path(path))
        if ino is None:
            return []
        return [self.tw.host_path(n) for n, _ in self.img.dirents(ino) if n not in (".", "..")]


def stage_sources(img, cases, cache):
    """Every file a selected case reads, at the same path RELATIVE to the
    directory it compiles in as on the host."""
    wanted = {}                       # machine path -> host path
    for c in cases:
        for s_ in c.srcs:
            wanted[f"{c.srcdir}/{s_}"] = os.path.join(c.root, s_)
            d = os.path.dirname(os.path.join(c.root, s_))
            for fn in os.listdir(d):
                if fn.endswith((".h", ".hpp", ".tcc", ".inc", ".def")):
                    rel = os.path.relpath(os.path.join(d, fn), c.root)
                    wanted[f"{c.srcdir}/{rel}"] = os.path.join(d, fn)
    if cache and any(c.suite == "libstdc++" for c in cases):
        # util/ whole, and every header in the tree: a test reaches for
        # one by `../../x.h` as often as beside itself.
        root = os.path.join(cache, gts.LIBSTDCXX)
        for dirpath, _d, files in os.walk(root):
            in_util = os.path.relpath(dirpath, root).split(os.sep)[0] == "util"
            for fn in files:
                if in_util or fn.endswith((".h", ".hpp", ".tcc", ".inc")):
                    rel = os.path.relpath(os.path.join(dirpath, fn), root)
                    wanted[f"{G}/src/libstdcxx/{rel}"] = os.path.join(dirpath, fn)
    for dst in sorted(wanted):
        with open(wanted[dst], "rb") as f:
            img.write(dst, f.read())
    return len(wanted)


def stage_run_dirs(img, cases, cache):
    for suite in sorted({c.suite for c in cases}):
        img.mkdirs(f"{G}/run/{SUITE_DIR[suite]}")
    if cache and any(c.suite == "libstdc++" for c in cases):
        root = os.path.join(cache, gts.LIBSTDCXX)
        for rel in gts.libstdcxx_data(cache):
            p = os.path.join(root, rel)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    img.write(f"{G}/run/libstdcxx/{os.path.basename(rel)}", f.read())
    for c in cases:
        for rel in c.data:
            p = os.path.join(c.root, rel)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    img.write(f"{G}/run/{SUITE_DIR[c.suite]}/{os.path.basename(rel)}", f.read())


# -- the guest's job list ---------------------------------------------------------

def q(args):
    return " ".join(shlex.quote(a) for a in args)


def guest_jobs(cases, extra, scale, libtest_needed):
    """`id TAB seconds TAB command` lines for corpus_run."""
    lines = []
    if libtest_needed:
        cmds = []
        for i, s in enumerate(gts.libtest_sources()):
            o = f"{G}/obj/{LIBTEST}.{i}.o"
            cmds.append(f"{{ c++ {q(gts.LIBTEST_FLAGS + extra)} -c {q([s])} -o {o} || echo FAILED {s}; }}")
        # Named, not `$(ls ...)`: the machine's ls lists ONE directory and
        # refuses several names, which left the archive empty.
        members = " ".join(f"{G}/obj/{LIBTEST}.{i}.o" for i in range(len(gts.libtest_sources())))
        cmds.append(f"ar rcD {G}/obj/libtestc++.a {members}")
        lines.append((f"b.{LIBTEST}", int(900 * scale),
                      f"cd {G}/src/libstdcxx && " + "; ".join(cmds)))
    for c in cases:
        src_dir = c.srcdir
        # The last case's program goes unless it differs from the cross
        # build's (its want file is the cross build's hash): a differing
        # one stays for the host to read back.
        # No /dev/null on SaunaOS: the check's chatter goes to a file.
        objs, steps = [], [f"for f in $(ls {G}/exe); do sum -a sha256 -c {G}/want/${{f%%_*}}/$f > {G}/null 2>&1 "
                           f"&& rm -f {G}/exe/$f; done; true"]
        for i, s in enumerate(c.srcs):
            o = f"{G}/obj/{c.shard}/{c.gid}.{i}.o"
            drv = "c++" if s.endswith((".cc", ".cpp")) else "cc"
            steps.append(f"{drv} {q(c.flags + extra)} -c {q([s])} -o {o}")
            objs.append(o)
        libs = [f"{G}/obj/libtestc++.a" if a == "libtestc++.a" else a for a in c.libs]
        exe = exe_path(c)
        steps.append(f"{'c++' if c.cxx else 'cc'} {q(c.flags + extra)} {' '.join(objs)} {q(libs)} -o {exe}")
        steps.append(f"sum -a sha256 {exe}")
        lines.append((f"{c.shard}/b.{c.gid}", int(600 * scale * max(1.0, c.timeout_factor)),
                      f"cd {src_dir} && " + " && ".join(steps)))
        # RUN_LIMIT_S, not 30: the ASUS took longer than 30 s over one
        # libstdc++ random-number test the KVM guest passed, which is
        # speed, not correctness (DejaGnu's own default is 300 s).
        lines.append((f"{c.shard}/r.{c.gid}", int(RUN_LIMIT_S * scale * c.timeout_factor),
                      f"cd {G}/run/{SUITE_DIR[c.suite]} && [ -x {exe} ] || exit 125; exec {exe}"))
    return lines


def exe_path(c):
    # Named <shard>_<id>: the clean-up loop finds its want file by the prefix.
    return f"{G}/exe/{c.shard}_{c.gid}"


def guest_rerun_jobs(cases, scale):
    # chmod: tfs3_writer gives a file outside its EXEC_DIRS no exec bit.
    return [(f"x.{c.gid}", int(30 * scale * c.timeout_factor),
             f"chmod 755 {G}/xexe/{c.gid}; cd {G}/run/{SUITE_DIR[c.suite]} && exec {G}/xexe/{c.gid}")
            for c in cases]


def bundle_job(name):
    """The last job: every output in one file (each behind a \\001NAME\\001
    marker, verbatim) and every object's SHA-256 in another -- two files
    to fetch from a --host machine instead of thousands. One `sum` per
    file, not `sum *.o`: thousands of names would be one huge argv."""
    return ("c.bundle", 3600,
            f"cd {G}/res-{name} && for d in . $(ls); do [ -d \"$d\" ] || continue; "
            f"for f in $(ls \"$d\" | grep '[.]out$'); do printf '\\001%s\\001' \"${{d#./}}/$f\"; "
            f"cat \"$d/$f\"; done; done > {G}/bundle-{name}.txt; cd {G}/obj && "
            f"for f in $(ls | grep '[.]o$'); do sum -a sha256 \"$f\"; done > {G}/objsums-{name}.txt; "
            f"for d in $(ls); do [ -d \"$d\" ] || continue; for f in $(ls \"$d\" | grep '[.]o$'); do "
            f"sum -a sha256 \"$d/$f\"; done; done >> {G}/objsums-{name}.txt; true")


# A machine whose kernel predates SPAWN_ARGS_MAX's growth to 256 KiB
# caps a spawn's whole argument vector at 1 KiB, so a job longer than
# this goes in a script file and the job line sources it -- the corpus
# then runs on any machine it is pointed at.
LONG_JOB = 900


def write_jobs(img, name, lines, runner, shards=()):
    lines = lines + [bundle_job(name)]
    short = []
    for jid, t, cmd in lines:
        if len(cmd) > LONG_JOB:
            img.write(f"{G}/jobs-{name}/{jid}.sh", (cmd + "\n").encode())
            cmd = f". {G}/jobs-{name}/{jid}.sh"
        short.append((jid, t, cmd))
    lines = short
    img.write(f"{G}/{name}.txt", "".join(f"{i}\t{t}\t{cmd}\n" for i, t, cmd in lines).encode())
    dirs = " ".join(f"{G}/res-{name}/{sh} {G}/obj/{sh}" for sh in shards)
    img.write(f"{G}/{name}.sh", (f"export PATH=/bin:/usr/bin\nrm -r -f {G}/res-{name}\n"
                                 f"mkdir -p {G}/res-{name} {G}/obj {G}/exe {dirs}\n"
                                 f"exec {G}/bin/corpus_run {G}/{name}.txt {G}/res-{name}\n").encode())
    with open(runner, "rb") as f:
        img.write(f"{G}/bin/corpus_run", f.read())
    return len(lines)


# -- the machine the corpus runs on ---------------------------------------------

def watch(ex, name, total, stall):
    """Run one job list to its DONE marker; `ex(cmd)` runs a shell command there."""
    ex(f"spawn /bin/dash {G}/{name}.sh")
    t0, last, last_n = time.time(), time.time(), -1
    while True:
        if re.search(r"^done$", ex(f"cat {G}/res-{name}/DONE"), re.M):
            break
        m = re.search(r"^\s*(\d+)\s+\S*results\.txt", ex(f"wc -l {G}/res-{name}/results.txt"), re.M)
        n = int(m.group(1)) if m else 0
        if n != last_n:
            last, last_n = time.time(), n
            print(f"native_corpus_test: {name}: {n}/{total} jobs, {time.time() - t0:.0f} s", flush=True)
        if time.time() - last > stall:
            return False, f"no job finished for {stall} s (at {n}/{total})"
        time.sleep(10)  # sleep-ok: a progress poll of an hour-long run, not a settle
    print(f"native_corpus_test: {name}: all {total} jobs in {time.time() - t0:.0f} s", flush=True)
    return True, ""


def parse_results(raw):
    res = {}
    for line in (raw or b"").decode(errors="replace").splitlines():
        f = line.split()
        if len(f) == 3:
            res[f[0]] = (f[1], int(f[2]))
    return res


def make_vm(args, disk):
    def vm(*argv, timeout=180):
        r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
                            "--disk", disk, *(["--kvm"] if args.kvm else []), "--mem", str(args.mem), *argv],
                           cwd=REPO, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr
    return vm


class VmTarget:
    """A QEMU guest booted from a copy of disk.img; files go in and come
    back out through the image, while the guest is powered off."""
    where = "the guest"

    def __init__(self, args, work, reuse=False):
        self.disk = os.path.join(work, "disk.img")
        if not reuse:
            copy_disk(args.disk, self.disk, cwd=REPO)
        self.vm = make_vm(args, self.disk)

    def stage(self, name):
        return Image(self.disk).open(writable=True)

    def deliver(self, st, name):
        st.close()

    def run(self, name, total, stall):
        vm = self.vm
        vm("stop")
        if "ready" not in vm("start", timeout=300):
            return False, "the guest did not come up"
        try:
            vm("exec", f"chmod 755 {G}/bin/corpus_run")
            return watch(lambda cmd: vm("exec", cmd, timeout=60), name, total, stall)
        finally:
            # The host reads the image and writes into it next, so the guest
            # must shut itself down: `vm.py stop`'s ACPI request goes unheard
            # (no power-button handler), and an image whose VM was SIGTERMed
            # has had offline writes clobbered at the next boot (docs/bugs.md).
            vm("exec", "sync", timeout=300)
            vm("exec", "spawn /bin/reboot --poweroff", timeout=60)
            if not poll(lambda: "not running" in vm("status"), 120, step=1):
                print("native_corpus_test: the guest did not power off -- SIGTERM; its last writes may be missing")
            vm("stop", timeout=120)

    def reader(self, name):
        return ImageReader(Image(self.disk).open(writable=False), name)

    def stop(self):
        self.vm("stop")


class ImageReader:
    def __init__(self, img, name):
        self.img, self.name = img, name

    def results(self):
        return parse_results(self.img.read(f"{G}/res-{self.name}/results.txt"))

    def out(self, jid):
        data = self.img.read(f"{G}/res-{self.name}/{jid}.out")
        return None if data is None else data.decode(errors="replace")

    def sha(self, path):
        data = self.img.read(path)
        return None if data is None else hashlib.sha256(data).hexdigest()

    def read(self, path):
        return self.img.read(path)

    def close(self):
        self.img.close()


class LocalStage:
    """What goes under /cc5 on a --host machine, mirrored locally first."""

    def __init__(self, root):
        self.root = root
        shutil.rmtree(root, ignore_errors=True)

    def mkdirs(self, path):
        os.makedirs(self.root + path, exist_ok=True)

    def write(self, path, data):
        self.mkdirs(os.path.dirname(path))
        with open(self.root + path, "wb") as f:
            f.write(data)


class HostTarget:
    """A SaunaOS machine on the network (tools/dev/remote.py): files go in
    by `sync`, the run is watched over telnet, results come back by `get`."""
    where = "the machine"

    def __init__(self, args, work):
        self.host, self.work = args.host, work

    def remote(self, *argv, timeout=600):
        r = subprocess.run([sys.executable, toolpath.tool("remote.py"), "--host", self.host, *argv],
                           cwd=REPO, capture_output=True, text=True, timeout=timeout, errors="replace")
        return r.returncode, r.stdout + r.stderr

    def ex(self, cmd, timeout=120):
        return self.remote("exec", cmd, timeout=timeout)[1]

    def stage(self, name):
        return LocalStage(os.path.join(self.work, f"stage-{name}"))

    def deliver(self, st, name):
        if name == "pass1":
            self.ex(f"rm -r -f {G}", timeout=1800)
        # --force: send everything rather than trust the machine's `sum`
        # to say what it already has (docs/bugs.md).
        rc, out = self.remote("sync", "--force", st.root + G, G, timeout=4 * 3600)
        if rc != 0:
            raise RuntimeError(f"remote.py sync failed:\n{out[-1500:]}")
        self.ex(f"chmod 755 {G}/bin/corpus_run")

    def run(self, name, total, stall):
        return watch(self.ex, name, total, stall)

    def reader(self, name):
        return RemoteReader(self, name)

    def stop(self):
        pass


def norm_key(name):
    """A bundle or sums name as the reader looks it up: the machine's `ls`
    prints a directory as `s0/`, so the bundle job's `$d/$f` reads `s0//f`,
    and the top directory's files come out as `./f`."""
    name = re.sub(r"/+", "/", name)
    return name[2:] if name.startswith("./") else name


class RemoteReader:
    """Results from a --host machine: the bundle and the object sums
    fetched once, any other file on demand."""

    def __init__(self, target, name):
        self.t, self.name = target, name
        self.dir = os.path.join(target.work, f"fetched-{name}")
        os.makedirs(self.dir, exist_ok=True)
        raw = (self.read(f"{G}/bundle-{name}.txt") or b"").decode(errors="replace")
        parts = raw.split("\x01")
        self.outs = {norm_key(parts[k]): parts[k + 1] for k in range(1, len(parts) - 1, 2)}
        self.sums = {}
        for line in (self.read(f"{G}/objsums-{name}.txt") or b"").decode(errors="replace").splitlines():
            m = re.match(r"([0-9a-f]{64})  (\S+)$", line)
            if m:
                self.sums[f"{G}/obj/{norm_key(m.group(2))}"] = m.group(1)

    def read(self, path):
        local = os.path.join(self.dir, path.strip("/").replace("/", "__"))
        rc, _ = self.t.remote("get", path, local, timeout=600)
        if rc != 0 or not os.path.exists(local):
            return None
        with open(local, "rb") as f:
            return f.read()

    def results(self):
        return parse_results(self.read(f"{G}/res-{self.name}/results.txt"))

    def out(self, jid):
        return self.outs.get(f"{jid}.out")

    def sha(self, path):
        return self.sums.get(path)

    def close(self):
        pass


# -- known failures -----------------------------------------------------------------

def load_known(path):
    """cc_corpus/known.txt: `<case-id glob>  <reason>`, # comments."""
    known = []
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#", 1)[0].rstrip() if not line.lstrip().startswith("#") else ""
            if line.strip():
                pat, _, why = line.strip().partition(" ")
                known.append((pat, why.strip()))
    return known


def known_reason(known, cid):
    for pat, why in known:
        if fnmatch.fnmatchcase(cid, pat):
            return why or pat
    return None


# -- main -------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--instance", type=int, default=6, metavar="N", help="the VM slot (default 6)")
    ap.add_argument("--disk", default="disk.img", help="the image to boot a COPY of")
    ap.add_argument("--kvm", action="store_true", help="KVM: the only way --full is practical in QEMU")
    ap.add_argument("--host", metavar="IP", help="run on this SaunaOS machine instead (tools/dev/remote.py), "
                    "not a QEMU guest; its /cc5 is replaced")
    ap.add_argument("--mem", type=int, default=2048, metavar="MIB", help="guest RAM (default 2048)")
    ap.add_argument("--suites", default="own,torture,libstdc++", help="which suites (comma-separated)")
    ap.add_argument("--full", action="store_true", help="every runnable test, not the sample")
    ap.add_argument("--every", default="torture=10,libstdc++=40", metavar="SUITE=N,...",
                    help="the sample: every Nth runnable test of a suite (default torture=10,libstdc++=40)")
    ap.add_argument("--only", metavar="REGEX", help="just the cases whose id matches (o_, t_, l_ prefixes)")
    ap.add_argument("--opt", default="-O2", help="the optimisation level every case gets (default -O2)")
    ap.add_argument("--no-host-run", action="store_true", help="skip the informational host gcc+glibc column")
    ap.add_argument("--report", metavar="TSV", help="write one line per case")
    ap.add_argument("--work", default=os.path.join(REPO, "build", "native_corpus"),
                    help="scratch directory for the host's builds (default build/native_corpus)")
    ap.add_argument("--collect", action="store_true",
                    help="judge what an earlier run with the same --work and cases left there, even a stopped one: "
                         "the host builds are redone, nothing runs on the machine")
    ap.add_argument("--skip-done", metavar="TSV",
                    help="leave out the cases this --report already has a guest build for (finish a stopped run)")
    ap.add_argument("--positive-control", choices=("objects", "known"),
                    help="objects: the guest compiles with -fno-ident; known: known.txt is ignored -- must FAIL")
    args = ap.parse_args()

    if args.host:
        out = subprocess.run([sys.executable, toolpath.tool("remote.py"), "--host", args.host, "exec",
                              "sum -a sha256 /usr/libexec/gcc/x86_64-saunaos/16.2.0/cc1 "
                              "/usr/libexec/gcc/x86_64-saunaos/16.2.0/cc1plus"],
                             cwd=REPO, capture_output=True, text=True, timeout=300).stdout
        theirs = dict((os.path.basename(p), h) for h, p in re.findall(r"^([0-9a-f]{64})  (\S+)$", out, re.M))
        if not theirs:
            print(f"native_corpus_test: SKIP -- {args.host} has no /usr/libexec/gcc compiler (or is not answering)")
            return 77
        libexec = os.path.join(REPO, "toolchain", "native", "usr", "libexec", "gcc", "x86_64-saunaos", "16.2.0")
        for prog in ("cc1", "cc1plus"):
            mine = sha(os.path.join(libexec, prog)) if os.path.exists(os.path.join(libexec, prog)) else None
            print(f"native_corpus_test: {args.host}'s {prog} is "
                  f"{'this checkout' + chr(39) + 's' if mine == theirs.get(prog) else 'NOT this checkout' + chr(39) + 's'} "
                  f"({(theirs.get(prog) or '?')[:16]})")
    elif not os.access(os.path.join(REPO, "seed", "sync", "usr", "bin", "cc"), os.X_OK):
        print("native_corpus_test: SKIP -- the seed tree has no /usr/bin/cc; "
              "`make native-toolchain`, then `make iso`")
        return 77
    if not os.access(xtool("gcc"), os.X_OK):
        print("native_corpus_test: SKIP -- no cross compiler; `make toolchain EXTRAS=1`")
        return 77
    suites = set(args.suites.split(","))
    cache = gts.extract() if suites & {"torture", "libstdc++"} else None
    if suites & {"torture", "libstdc++"} and not cache:
        print(f"native_corpus_test: no {gts.TARBALL} -- the GCC suites are left out "
              "(`make toolchain EXTRAS=1` fetches it)")
        suites -= {"torture", "libstdc++"}
    every = dict((k, int(v)) for k, v in (p.split("=") for p in args.every.split(",") if p))
    cases = []
    if "own" in suites:
        cases += own_cases()
    if "torture" in suites:
        cases += select(gts.torture_cases(cache, args.opt), 1 if args.full else every.get("torture", 1), args.only)
    if "libstdc++" in suites:
        cases += select(gts.libstdcxx_cases(cache, args.opt), 1 if args.full else every.get("libstdc++", 1), args.only)
    if args.only and "own" in suites:
        cases = [c for c in cases if c.suite != "own" or re.search(args.only, c.cid)]
    if args.skip_done:
        done = set()
        with open(args.skip_done) as f:
            for line in f:
                col = line.rstrip("\n").split("\t")
                if len(col) > 5 and col[5] not in ("", "guest_build"):
                    done.add(col[1])
        cases = [c for c in cases if c.cid not in done]
        print(f"native_corpus_test: --skip-done: {len(done)} cases already have a guest build in {args.skip_done}")
    run = [c for c in cases if not c.skip]
    print(f"native_corpus_test: {len(cases)} cases, {len(run)} to run "
          f"({', '.join(f'{s} {sum(c.suite == s for c in run)}' for s in ('own', 'torture', 'libstdc++') if s in suites)})"
          f"; {('on ' + args.host) if args.host else 'KVM' if args.kvm else 'TCG -- times are not speeds'}")
    return drive(args, cases, run, cache)


def drive(args, cases, run, cache):
    res = Results()
    work = os.path.abspath(args.work)
    # On the machine every file is named by a short id, never the case's
    # own name: it chose that while readdir() cut names at 63 bytes, and
    # it keeps every path there short (the debug console takes a line).
    #
    # And no directory holds more than SHARD of them: tfs3_writer cannot
    # grow one past its 12 direct blocks, and the full torture suite is
    # 1,681 files side by side. A torture test still compiles as `x.c`
    # from its own directory, so the path the compiler sees is unchanged.
    tk = 0
    for k, c in enumerate(run):
        c.gid = f"c{k}"
        c.shard = f"s{k // SHARD}"
        if c.suite == "torture":
            c.srcdir = f"{G}/src/torture/t{tk // SHARD}"
            tk += 1
        else:
            c.srcdir = f"{G}/src/{SUITE_DIR[c.suite]}"
    if args.collect:
        # The machine's results are in here (disk.img, fetched-*); the
        # host's side is rebuilt over itself, and is deterministic.
        if not os.path.isdir(work):
            print(f"native_corpus_test: --collect: no {work}")
            return 2
        for d in ("xobj", "xexe", "nobj", "nexe", "gexe", "gobj"):
            shutil.rmtree(os.path.join(work, d), ignore_errors=True)
    else:
        shutil.rmtree(work, ignore_errors=True)
    for d in ("xobj", "xexe", "nobj", "nexe", "gobj"):
        os.makedirs(os.path.join(work, d))
    need_lib = any(c.suite == "libstdc++" for c in run)
    for s in ("own", "torture", "libstdc++"):
        for slot in range(os.cpu_count() or 4):
            os.makedirs(os.path.join(work, "nrun", f"{s}.{slot}"), exist_ok=True)
    if need_lib:
        for rel in gts.libstdcxx_data(cache):
            p = os.path.join(cache, gts.LIBSTDCXX, rel)
            for slot in range(os.cpu_count() or 4):
                if os.path.isfile(p):
                    shutil.copy(p, os.path.join(work, "nrun", f"libstdc++.{slot}"))

    # 1. The cross compiler's build of everything, and the host's own.
    t0 = time.time()
    libtest = libtest_native = None
    lib_shas = []
    if need_lib:
        libtest, lib_shas, lib_log = build_libtest(cache, work, xtool("g++"), xtool("ar"), "x")
        libtest_native, _, _ = build_libtest(cache, work, "g++", "ar", "n")
        if lib_log:
            print("native_corpus_test: libtestc++ (cross) members that did not build:\n" + lib_log)
    runner = os.path.join(work, "corpus_run")
    subprocess.run([xtool("gcc"), "-O2", os.path.join(CORPUS, "corpus_run.c"), "-o", runner], check=True)
    host = {}
    native = {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        futs = {ex.submit(host_build, c, work, libtest): c for c in run}
        for f in concurrent.futures.as_completed(futs):
            host[futs[f].cid] = f.result()
    print(f"native_corpus_test: cross builds: {sum(h['ok'] for h in host.values())}/{len(run)} "
          f"in {time.time() - t0:.0f} s")
    if not args.no_host_run:
        t1 = time.time()
        slots = list(range(os.cpu_count() or 4))
        import queue
        free = queue.Queue()
        for s in slots:
            free.put(s)

        def one(c):
            s = free.get()
            try:
                return host_native(c, work, libtest_native, s)
            finally:
                free.put(s)
        with concurrent.futures.ThreadPoolExecutor(len(slots)) as ex:
            futs = {ex.submit(one, c): c for c in run}
            for f in concurrent.futures.as_completed(futs):
                native[futs[f].cid] = f.result()
        print(f"native_corpus_test: host gcc+glibc runs in {time.time() - t1:.0f} s")

    # 2. The guest's pass: sources in, everything built and run there.
    target = HostTarget(args, work) if args.host else VmTarget(args, work, reuse=args.collect)
    if args.collect:
        return collect(args, res, cases, run, host, native, need_lib, lib_shas, target, work)
    st = target.stage("pass1")
    n = stage_sources(st, run, cache)
    stage_run_dirs(st, run, cache)
    for c in run:
        if host[c.cid]["ok"]:
            st.write(f"{G}/want/{c.shard}/{c.shard}_{c.gid}", f"{host[c.cid]['exe']}  {exe_path(c)}\n".encode())
    extra = ["-fno-ident"] if args.positive_control == "objects" else []
    if extra:
        print("native_corpus_test: POSITIVE CONTROL -- the guest compiles with -fno-ident; the object check must FAIL")
    scale = 1 if (args.kvm or args.host) else 4
    jobs = guest_jobs(run, extra, scale, need_lib)
    shards = sorted({c.shard for c in run}, key=lambda x: int(x[1:]))
    njobs = write_jobs(st, "pass1", jobs, runner, shards)
    t2 = time.time()
    target.deliver(st, "pass1")
    print(f"native_corpus_test: {n} source files and {njobs} jobs delivered to {target.where} "
          f"in {time.time() - t2:.0f} s")
    ok, why = target.run("pass1", njobs, stall=max(t for _, t, _ in jobs) + 120)
    if not res.check(f"{target.where} ran the whole job list", ok, why):
        target.stop()
        return res.finish("native_corpus_test")

    return read_back(args, res, cases, run, host, native, need_lib, lib_shas, target, runner, scale, work)


def read_back(args, res, cases, run, host, native, need_lib, lib_shas, target, runner, scale, work):
    """Pass 1's results off the machine; pass 2 where linked programs differ; the verdicts."""
    rd = target.reader("pass1")

    def verified(path, want, save):
        """The target's own hash is a HINT: a mismatch is confirmed from
        the bytes, so a wrong `sum` there can cost a fetch but never fake
        a pass or a failure. Returns (sha, local copy or None)."""
        h = rd.sha(path)
        if h is None or h == want:
            return h, None
        data = rd.read(path)
        if data is None:
            return None, None
        os.makedirs(os.path.dirname(save), exist_ok=True)
        with open(save, "wb") as f:
            f.write(data)
        return hashlib.sha256(data).hexdigest(), save

    results = rd.results()
    guest = {}
    fetch_bad = []
    for c in run:
        b = results.get(f"{c.shard}/b.{c.gid}")
        r = results.get(f"{c.shard}/r.{c.gid}")
        blog = rd.out(f"{c.shard}/b.{c.gid}") or ""
        m = re.search(r"^([0-9a-f]{64})  " + re.escape(exe_path(c)) + r"$", blog, re.M)
        hobjs = host[c.cid].get("objs") or []
        objs = [verified(f"{G}/obj/{c.shard}/{c.gid}.{i}.o", hobjs[i] if i < len(hobjs) else None,
                         os.path.join(work, "gobj", f"{c.cid}.{i}.o"))[0] for i in range(len(c.srcs))]
        guest[c.cid] = {"build": b[0] if b else None, "build_us": b[1] if b else 0,
                        "run": r[0] if r else None, "run_us": r[1] if r else 0,
                        "exe": m.group(1) if m else None, "log": blog, "objs": objs,
                        "out": rd.out(f"{c.shard}/r.{c.gid}") or ""}
        if guest[c.cid]["exe"] and host[c.cid]["ok"] and guest[c.cid]["exe"] != host[c.cid]["exe"]:
            # A copy that matches the machine's own sum, or a second fetch
            # (its `sum` has been wrong before, docs/bugs.md), is the file;
            # one that agrees with neither came back damaged and is never judged.
            data, seen = None, set()
            for _ in range(3):
                data = rd.read(exe_path(c))
                h = data and hashlib.sha256(data).hexdigest()
                if data is None or h == guest[c.cid]["exe"] or h in seen:
                    break
                seen.add(h)
            else:
                fetch_bad.append(c.cid)
                data = None
            if data is not None:
                gpath = os.path.join(work, "gexe", c.cid)
                os.makedirs(os.path.dirname(gpath), exist_ok=True)
                with open(gpath, "wb") as f:
                    f.write(data)
                guest[c.cid]["exe"] = hashlib.sha256(data).hexdigest()
                guest[c.cid]["loaded"] = loaded_sha(gpath)
    res.check("every program fetched from the machine matches its own sum there", not fetch_bad,
              ", ".join(fetch_bad[:20]))
    glib = [verified(f"{G}/obj/{LIBTEST}.{i}.o", lib_shas[i], os.path.join(work, "gobj", f"{LIBTEST}.{i}.o"))[0]
            for i in range(len(lib_shas))] if need_lib else []
    rd.close()

    # 3. A linked program that differs: the cross-built one, run on the same machine.
    differ = [c for c in run if host[c.cid]["ok"] and guest[c.cid]["exe"]
              and guest[c.cid]["exe"] != host[c.cid]["exe"]
              and guest[c.cid].get("loaded") != host[c.cid]["loaded"]]
    rerun = {}
    if differ:
        st = target.stage("pass2")
        for c in differ:
            with open(host[c.cid]["exe_path"], "rb") as f:
                st.write(f"{G}/xexe/{c.gid}", f.read())
        jobs2 = guest_rerun_jobs(differ, scale)
        n2 = write_jobs(st, "pass2", jobs2, runner)
        target.deliver(st, "pass2")
        print(f"native_corpus_test: {len(differ)} linked programs differ -- running the cross builds too")
        ok2, why2 = target.run("pass2", n2, stall=max(t for _, t, _ in jobs2) + 120)
        res.check(f"{target.where} ran the cross-built programs that differ", ok2, why2)
        rd = target.reader("pass2")
        r2 = rd.results()
        for c in differ:
            st2 = r2.get(f"x.{c.gid}")
            rerun[c.cid] = {"run": st2[0] if st2 else None, "out": rd.out(f"x.{c.gid}") or ""}
        rd.close()

    return judge(args, res, cases, run, host, native, guest, rerun, lib_shas, glib, work)


def collect(args, res, cases, run, host, native, need_lib, lib_shas, target, work):
    """--collect: the machine already ran pass 1 (all of it, or until it was
    stopped). A case with no build result there is reported as not run and
    left out of every check; pass 2 still runs, so the machine must be up."""
    runner = os.path.join(work, "corpus_run")
    scale = 1 if (args.kvm or args.host) else 4
    rd = target.reader("pass1")
    results = rd.results()
    rd.close()
    if not results:
        print(f"native_corpus_test: --collect: no pass 1 results in {work}")
        return 2
    done = [c for c in run if f"{c.shard}/b.{c.gid}" in results and f"{c.shard}/r.{c.gid}" in results]
    for c in run:
        if c not in done:
            c.skip = "not run (the run was stopped)"
    print(f"native_corpus_test: --collect: {len(done)} of {len(run)} cases finished on {target.where}"
          f"{'' if len(done) == len(run) else '; the rest are reported as not run'}")
    return read_back(args, res, cases, done, host, native, need_lib, lib_shas, target, runner, scale, work)


def verdict_ok(c, status):
    return (status == "0") if c.expect == "pass" else (status not in ("0", None))


def judge(args, res, cases, run, host, native, guest, rerun, lib_shas, glib, work):
    known = [] if args.positive_control == "known" else load_known(KNOWN)
    if args.positive_control == "known":
        print("native_corpus_test: POSITIVE CONTROL -- known.txt ignored; the verdict check must FAIL")
    rows = []
    no_build, accepts, obj_diff, exe_bad, unexpected, stale, glibc_diff = [], [], [], [], [], [], []
    for c in run:
        h, g = host[c.cid], guest[c.cid]
        kr = known_reason(known, c.cid)
        # A listed case that the machine cannot BUILD is excused like a
        # listed run failure, and a listed one that builds and runs is
        # stale below -- the list covers both halves.
        if h["ok"] and g["build"] != "0" and not kr:
            no_build.append(f"{c.cid}: guest build {g['build']}: {g['log'][-300:].strip()!r}")
        if not h["ok"] and g["build"] == "0":
            accepts.append(f"{c.cid}: the cross compiler failed at {h['stage']}, the guest built it")
        diffs = [i for i, (a, b) in enumerate(zip(h.get("objs", []), g["objs"])) if b is not None and a != b]
        if diffs:
            obj_diff.append(f"{c.cid}: object(s) {diffs} ({', '.join(c.srcs[i] for i in diffs)})")
        same_exe = h["ok"] and g["exe"] == h.get("exe")
        same_loaded = same_exe or (h["ok"] and g.get("loaded") == h.get("loaded"))
        if h["ok"] and g["build"] == "0" and not same_loaded:
            rr = rerun.get(c.cid)
            if not rr or rr["run"] != g["run"] or (c.oracle == "output" and rr["out"] != g["out"]):
                exe_bad.append(f"{c.cid}: linked programs differ, and the cross build ran "
                               f"{rr['run'] if rr else 'not at all'} where the guest's ran {g['run']}")
        status = g["run"]
        if g["build"] == "0":
            if c.oracle == "output":
                nat = native.get(c.cid, {})
                if status != "0" and not kr:
                    unexpected.append(f"{c.cid}: exit {status}: {g['out'][-300:]!r}")
                if status == "0" and kr:
                    stale.append(f"{c.cid}: exits 0, but known.txt says: {kr}")
                if nat.get("status") == "0" and nat.get("out") != g["out"]:
                    gl, nl = g["out"].splitlines(), nat["out"].splitlines()
                    d = [f"    glibc: {a!r}\n    guest: {b!r}" for a, b in zip(nl + [""] * len(gl), gl + [""] * len(nl))
                         if a != b]
                    glibc_diff.append(f"{c.cid}: {len(d)} line(s) differ from the host's glibc build\n" + "\n".join(d[:6]))
            else:
                good = verdict_ok(c, status)
                if not good and not kr:
                    unexpected.append(f"{c.cid}: run {status} (expected {c.expect}); "
                                      f"host glibc {native.get(c.cid, {}).get('status', '?')}: "
                                      f"{g['out'][-200:].strip()!r}")
                if good and kr:
                    stale.append(f"{c.cid}: passes, but known.txt says: {kr}")
        rows.append((c.suite, c.cid, "", c.expect, "ok" if h["ok"] else h["stage"], g["build"],
                     "same" if not diffs and g["objs"] and None not in g["objs"] else ("differ" if diffs else "missing"),
                     "same" if same_exe else "loaded-same" if same_loaded else "differ", status, native.get(c.cid, {}).get("status", ""),
                     f"{g['build_us'] / 1e6:.2f}", kr or ""))
    for c in cases:
        if c.skip:
            rows.append((c.suite, c.cid, c.skip, c.expect, "", "", "", "", "", "", "", ""))

    lib_bad = [f"util/{gts.LIBTEST_SOURCES[i]}" for i, (a, b) in enumerate(zip(lib_shas, glib)) if a and b != a]
    built = sum(1 for c in run if host[c.cid]["ok"])
    res.check(f"every case the cross compiler builds, the guest builds ({built})", not no_build,
              "\n".join(no_build[:20]))
    res.check("nothing the cross compiler rejects, the guest accepts", not accepts, "\n".join(accepts[:20]))
    nobj = sum(len(guest[c.cid]["objs"]) for c in run)
    res.check(f"every object the guest wrote is byte-identical to the cross compiler's ({nobj} objects"
              f"{' + libtestc++' if lib_shas else ''})", not obj_diff and not lib_bad,
              "\n".join((lib_bad and [f"libtestc++: {', '.join(lib_bad)}"] or []) + obj_diff[:30]) +
              (f"\n... {len(obj_diff)} in all; the guest's copies are in {work}/gobj" if obj_diff else ""))
    ndiff = sum(1 for c in run if host[c.cid]["ok"] and guest[c.cid]["exe"]
                and guest[c.cid]["exe"] != host[c.cid]["exe"])
    nload = sum(1 for c in run if host[c.cid]["ok"] and guest[c.cid].get("loaded") not in (None, host[c.cid]["loaded"]))
    nsame = sum(1 for c in run if host[c.cid]["ok"] and guest[c.cid]["exe"] == host[c.cid]["exe"])
    res.check(f"every program the guest linked loads the cross build's bytes, or runs as it does "
              f"({nsame} identical files, {ndiff - nload} identical once loaded, {nload} differ)",
              not exe_bad, "\n".join(exe_bad[:20]))
    if ndiff:
        print(f"native_corpus_test: the {ndiff} program file(s) that differ are in {work}/gexe "
              f"(the cross ones in {work}/xexe)")
    res.check("each test's run matches known.txt -- no new failure", not unexpected, "\n".join(unexpected[:40]) +
              (f"\n... {len(unexpected)} in all" if len(unexpected) > 40 else ""))
    res.check("no known.txt entry is passing now (the list stays true)", not stale, "\n".join(stale[:40]))
    if glibc_diff:
        print("native_corpus_test: INFORMATIONAL -- own programs whose output differs from the host's gcc+glibc build "
              "(tolibc's behaviour, not the compiler's):\n" + "\n".join(glibc_diff))

    by = {}
    for c in run:
        s = by.setdefault(c.suite, [0, 0, 0, 0.0])
        s[0] += 1
        s[1] += guest[c.cid]["build"] == "0"
        s[2] += verdict_ok(c, guest[c.cid]["run"]) if c.oracle == "exit" else guest[c.cid]["run"] == "0"
        s[3] += guest[c.cid]["build_us"] / 1e6
    for suite, (n, b, p, secs) in by.items():
        skipped = sum(1 for c in cases if c.suite == suite and c.skip)
        print(f"native_corpus_test: {suite}: {n} run, {b} built on the guest, {p} passed their own checks, "
              f"{skipped} skipped; guest build time {secs:.0f} s ({secs / max(n, 1):.2f} s each, "
              f"{args.host or ('KVM' if args.kvm else 'TCG')})")
    if args.report:
        with open(args.report, "w") as f:
            f.write("suite\tcase\tskip\texpect\tcross\tguest_build\tobjects\tprogram\tguest_run\thost_glibc_run"
                    "\tguest_build_s\tknown\n")
            for r in sorted(rows):
                f.write("\t".join(str(x) for x in r) + "\n")
        print(f"native_corpus_test: report in {args.report}")
    return res.finish("native_corpus_test")


if __name__ == "__main__":
    sys.exit(main())
