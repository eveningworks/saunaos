#!/usr/bin/env python3
"""Fuzz kernel/lib/heap_core.c on the HOST, with its own invariants checked.

WHAT IT CHECKS. heap_core.c is malloc for both rings, built here with the
host gcc and four hooks (mmap for memory, a pthread mutex for the lock).
A seeded random mix of kmalloc / krealloc / kfree over a few thousand
live blocks -- most small, some page-sized, a few hundreds of KiB -- with
every block filled with a pattern its slot owns:

  - every byte of every live block is still its pattern before it is
    touched again (an overlap or a lost realloc copy shows here),
  - krealloc keeps the first min(old, new) bytes,
  - every pointer is 16-aligned and kmalloc_size() covers the request,
  - heap_validate() -- the size-class free lists against the block list
    -- is 0 every 20 000 steps,
  - after freeing everything, nothing is counted as used.

It runs twice: plain, and with `heap debug` on (red-zones and poison,
the second layout every block can have). Then ring 3's slab front end,
userland/libc/malloc.c, over the same heap: the same fuzz through
malloc/calloc/realloc/free, and four threads freeing each other's
blocks (the browser allocates on its fetch and decode threads).

WHY ON THE HOST. A KTEST runs a few thousand steps (kernel/mm/mm_test.c);
the bins took two wrong merges to get right, both found in the first
20 000 steps here and neither by a short run. Millions of steps take
seconds natively and minutes in a guest.

--netsurf PAGE builds the same allocator as an LD_PRELOAD malloc and
times the host's NetSurf (tools/netsurf_vendor.py's build) laying PAGE
out with it and with glibc: the measurement that found the old
first-fit walk quadratic (a Wikipedia article: 0.1 s against minutes).

  python3 tools/heap_hostcheck.py
  python3 tools/heap_hostcheck.py --steps 5000000 --seed 7
  python3 tools/heap_hostcheck.py --positive-control
  python3 tools/heap_hostcheck.py --netsurf data/tests/web/net.html

--positive-control builds a copy with release() back to merging a freed
block into its left neighbour BEFORE listing it -- the first bug the
bins had -- and passes only if the validation catches it.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CORE = os.path.join(REPO, "kernel", "lib", "heap_core.c")
SLAB = os.path.join(REPO, "userland", "libc", "malloc.c")
API = os.path.join(REPO, "kernel", "include", "api")

HOOKS = r"""
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
void heap_os_lock(void) { pthread_mutex_lock(&mu); }
void heap_os_unlock(void) { pthread_mutex_unlock(&mu); }
int heap_os_should_fail_alloc(void) { return 0; }
void heap_os_report(const char *m) { fputs(m, stderr); }
void *heap_os_alloc(uint64_t b) {
    void *p = mmap(0, b, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
void k_memset(void *d, uint8_t c, size_t n) { memset(d, c, n); }
void k_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
int k_snprintf(char *b, size_t n, const char *f, ...) {
    va_list a; va_start(a, f); int r = vsnprintf(b, n, f, a); va_end(a); return r;
}
"""

# The preload: ring 3's names over the same calls (userland/include/stdlib.h).
PRELOAD = r"""
#include <stddef.h>
#include <stdint.h>
#include <string.h>
void *kmalloc(size_t); void *kzalloc(size_t); void kfree(void *); void *krealloc(void *, size_t);
uint64_t kmalloc_size(const void *);
void *malloc(size_t n) { return kmalloc(n ? n : 1); }
void free(void *p) { if (p) kfree(p); }
void *calloc(size_t n, size_t s) { return kzalloc(n * s ? n * s : 1); }
void *realloc(void *p, size_t n) { return krealloc(p, n); }
void *memalign(size_t a, size_t n) { return a <= 16 ? kmalloc(n ? n : 1) : 0; }
void *aligned_alloc(size_t a, size_t n) { return memalign(a, n); }
int posix_memalign(void **r, size_t a, size_t n) { *r = memalign(a, n); return *r ? 0 : 12; }
size_t malloc_usable_size(void *p) { return kmalloc_size(p); }
"""

FUZZ = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
void *kmalloc(size_t); void kfree(void *); void *krealloc(void *, size_t);
uint64_t kmalloc_size(const void *); uint64_t heap_used_bytes(void);
uint64_t heap_validate(void); uint64_t heap_violations(void); void heap_set_debug(int);
#define N 4096
static unsigned char *ptr[N]; static size_t len[N]; static unsigned char tag[N];
static uint64_t rng;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static size_t rsize(void) {
    uint64_t r = rnd() % 100;
    return r < 70 ? 1 + rnd() % 256 : r < 95 ? 1 + rnd() % 8192 : 1 + rnd() % 300000;
}
static int intact(int i) {
    for (size_t k = 0; k < len[i]; k++) if (ptr[i][k] != (unsigned char)(tag[i] + k)) return 0;
    return 1;
}
static void fill(int i) { for (size_t k = 0; k < len[i]; k++) ptr[i][k] = (unsigned char)(tag[i] + k); }
int main(int argc, char **argv) {
    long steps = atol(argv[1]); rng = strtoull(argv[2], 0, 0) | 1; heap_set_debug(atoi(argv[3]));
    for (long it = 0; it < steps; it++) {
        if (it % 20000 == 0 && heap_validate()) { printf("FAIL validate at step %ld\n", it); return 1; }
        int i = rnd() % N, op = rnd() % 3;
        if (ptr[i] && !intact(i)) { printf("FAIL block %d overwritten at step %ld\n", i, it); return 1; }
        if (!ptr[i] || op == 0) {
            if (ptr[i]) kfree(ptr[i]);
            len[i] = rsize(); tag[i] = (unsigned char)rnd(); ptr[i] = kmalloc(len[i]);
            if (!ptr[i]) { printf("FAIL out of memory at step %ld\n", it); return 1; }
            if (((uintptr_t)ptr[i] & 15) || kmalloc_size(ptr[i]) < len[i]) {
                printf("FAIL alignment or size at step %ld\n", it); return 1;
            }
            fill(i);
        } else if (op == 1) {
            size_t n = rsize(); unsigned char *q = krealloc(ptr[i], n);
            if (!q) { printf("FAIL realloc out of memory at step %ld\n", it); return 1; }
            for (size_t k = 0; k < (n < len[i] ? n : len[i]); k++)
                if (q[k] != (unsigned char)(tag[i] + k)) { printf("FAIL realloc lost data at step %ld\n", it); return 1; }
            ptr[i] = q; len[i] = n; fill(i);
        } else { kfree(ptr[i]); ptr[i] = 0; }
    }
    for (int i = 0; i < N; i++) if (ptr[i]) kfree(ptr[i]);
    unsigned long v = (unsigned long)heap_validate(), u = (unsigned long)heap_used_bytes();
    printf("%s %ld steps, validate %lu, used after freeing all %lu, violations %lu\n",
           v || u ? "FAIL" : "ok", steps, v, u, (unsigned long)heap_violations());
    return v || u;
}
"""

SLAB_FUZZ = FUZZ.replace("kmalloc(len[i])", "malloc(len[i])") \
    .replace("krealloc(ptr[i], n)", "realloc(ptr[i], n)") \
    .replace("kfree(", "free(") \
    .replace("|| kmalloc_size(ptr[i]) < len[i]", "") \
    .replace("void *kmalloc(size_t); void free(void *); void *realloc(void *, size_t);",
             "void *malloc(size_t); void free(void *); void *realloc(void *, size_t);") \
    .replace("u = (unsigned long)heap_used_bytes()", "u = 0")

THREADS = r"""
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint64_t heap_validate(void);
#define Q 1024
static void *volatile handoff[Q];
static void *run(void *arg) {
    uint64_t x = (uintptr_t)arg * 0x9e3779b97f4a7c15ull + 1;
    void *mine[256] = {0}; unsigned char tag[256];
    for (long it = 0; it < 300000; it++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int i = x % 256; size_t n = 1 + (x >> 20) % ((x & 0x300) ? 200 : 3000);
        if (mine[i]) {
            if (((unsigned char *)mine[i])[0] != tag[i]) { printf("FAIL a block changed under its owner\n"); exit(1); }
            free(__atomic_exchange_n(&handoff[(x >> 40) % Q], mine[i], __ATOMIC_ACQ_REL));
            mine[i] = 0;
        } else {
            mine[i] = (x & 1) ? malloc(n) : calloc(1, n);
            if (!mine[i]) { printf("FAIL out of memory\n"); exit(1); }
            tag[i] = (unsigned char)x; memset(mine[i], tag[i], n);
            if (x & 0x8000) { void *q = realloc(mine[i], n * 2); if (!q) exit(1); mine[i] = q; }
        }
    }
    for (int i = 0; i < 256; i++) free(mine[i]);
    return 0;
}
int main(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, run, (void *)(i + 1));
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    for (int i = 0; i < Q; i++) free(handoff[i]);
    unsigned long v = (unsigned long)heap_validate();
    printf("%s 4 threads x 300000 steps, validate %lu\n", v ? "FAIL" : "ok", v);
    return v != 0;
}
"""

# The first bug the bins had: a block merged into its left neighbour
# before it was ever listed, so the merge unlinked a stranger.
BROKEN = ("""    bin_insert(b);   // before the merge below, which takes `b` out again
    struct heap_block *p = b->prev;
    if (p && p->free == HEAP_FREE && (uint8_t *)(p + 1) + p->size == (uint8_t *)b) {
        bin_remove(p);
        try_merge_next(p);
        bin_insert(p);
    }
}""", """    struct heap_block *p = b->prev;
    if (p && p->free == HEAP_FREE && (uint8_t *)(p + 1) + p->size == (uint8_t *)b) {
        bin_remove(p);
        try_merge_next(p);
        b = p;
    }
    bin_insert(b);
}""")


def gcc(*args):
    r = subprocess.run(["gcc", "-O2", "-fPIC", *args], capture_output=True, text=True)
    if r.returncode:
        sys.exit("heap_hostcheck: gcc failed:\n" + r.stderr)


def build(tmp, broken=False):
    core = CORE
    if broken:
        text = open(CORE).read()
        assert BROKEN[0] in text, "positive control could not find release()'s merge"
        core = os.path.join(tmp, "heap_core_broken.c")
        open(core, "w").write(text.replace(BROKEN[0], BROKEN[1]))
    for name, src in (("hooks", HOOKS), ("preload", PRELOAD), ("fuzz", FUZZ)):
        open(os.path.join(tmp, name + ".c"), "w").write(src)
    obj = lambda n: os.path.join(tmp, n + ".o")
    gcc("-ffreestanding", "-I" + API, "-c", core, "-o", obj("heap_core"))
    gcc("-c", os.path.join(tmp, "hooks.c"), "-o", obj("hooks"))
    gcc("-c", os.path.join(tmp, "preload.c"), "-o", obj("preload"))
    fuzz = os.path.join(tmp, "fuzz")
    gcc(os.path.join(tmp, "fuzz.c"), obj("heap_core"), obj("hooks"), "-o", fuzz)
    lib = os.path.join(tmp, "libtoyheap.so")
    gcc("-shared", obj("heap_core"), obj("hooks"), obj("preload"), "-o", lib)
    # The slab, over the same heap: its includes stand in for ring 3's.
    inc = os.path.join(tmp, "inc")
    os.makedirs(os.path.join(inc, "rt"), exist_ok=True)
    shutil.copy(os.path.join(API, "heap.h"), inc)
    open(os.path.join(inc, "rt", "sys.h"), "w").write(
        "#include <sched.h>\nstatic inline void sys_yield(void) { sched_yield(); }\n")
    gcc("-I" + inc, "-c", SLAB, "-o", obj("slab"))
    slab = os.path.join(tmp, "libslab.so")
    gcc("-shared", obj("slab"), obj("heap_core"), obj("hooks"), "-o", slab)
    for name, src in (("slab_fuzz", SLAB_FUZZ), ("threads", THREADS)):
        open(os.path.join(tmp, name + ".c"), "w").write(src)
        gcc(os.path.join(tmp, name + ".c"), "-L" + tmp, "-lslab", "-Wl,-rpath," + tmp,
            "-lpthread", "-o", os.path.join(tmp, name))
    return fuzz, lib


def fuzz_runs(fuzz, steps, seed):
    ok = True
    for debug, n in ((0, steps), (1, max(steps // 5, 20000))):
        r = subprocess.run([fuzz, str(n), str(seed), str(debug)], capture_output=True, text=True)
        line = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
        print(f"  {'debug' if debug else 'plain'}: {line}")
        ok &= r.returncode == 0
    return ok


def netsurf(lib, page):
    sys.path.insert(0, HERE)
    from netsurf_vendor import DEFAULT_WORK
    mdir = os.path.join(DEFAULT_WORK, "src", "netsurf")
    if not os.access(os.path.join(mdir, "nsmonkey"), os.X_OK):
        sys.exit("heap_hostcheck: no host NetSurf -- run `python3 tools/netsurf_vendor.py --no-copy`")
    url = "file://" + os.path.abspath(page)
    for name, pre in (("glibc", None), ("heap_core", lib)):
        env = dict(os.environ, LANGUAGE="en", LANG="C")
        if pre:
            env["LD_PRELOAD"] = pre
        p = subprocess.Popen(["./nsmonkey"], cwd=mdir, env=env, text=True,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        t0 = time.time()
        p.stdin.write(f"WINDOW NEW {url}\n")
        p.stdin.flush()
        extent, done = "-", None
        for line in p.stdout:
            if "UPDATE_EXTENT" in line:
                extent = " ".join(line.split()[-3::2])
            if "SET_STATUS" in line and "Done" in line:
                done = time.time() - t0
                break
            if time.time() - t0 > 300:
                break
        try:
            p.stdin.write("QUIT\n")
            p.stdin.close()
        except BrokenPipeError:
            pass
        p.wait(timeout=10)
        print(f"  {name:9s} {'%.2f s' % done if done is not None else 'not done in 300 s'}"
              f"  extent {extent}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--steps", type=int, default=2000000)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x2545F4914F6CDD1D)
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("--netsurf", metavar="PAGE")
    a = ap.parse_args()
    if not shutil.which("gcc"):
        sys.exit("heap_hostcheck: needs gcc")
    with tempfile.TemporaryDirectory(prefix="heap_hostcheck.") as tmp:
        if a.positive_control:
            fuzz, _ = build(tmp, broken=True)
            caught = not fuzz_runs(fuzz, a.steps, a.seed)
            print("positive control:", "CAUGHT (good)" if caught else "MISSED -- the check is blind")
            return 0 if caught else 1
        fuzz, lib = build(tmp)
        print(f"heap_core.c, {a.steps} steps, seed {a.seed:#x}")
        ok = fuzz_runs(fuzz, a.steps, a.seed)
        print("userland/libc/malloc.c (the slab) over it")
        for name, argv in (("slab_fuzz", [str(a.steps), str(a.seed), "0"]), ("threads", [])):
            r = subprocess.run([os.path.join(tmp, name)] + argv, capture_output=True, text=True)
            print(f"  {name}: " + (r.stdout.strip().splitlines() or ["(no output)"])[-1])
            ok &= r.returncode == 0
        if a.netsurf:
            netsurf(lib, a.netsurf)
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
