// The /etc config parser, in RING 3.
//
// WHY THIS EXISTS RATHER THAN ONLY A KTEST -- the same reason
// klineedit_test.c does. kernel/lib/etc_config.c has KTESTs, and they
// would go on passing whether or not a single line of it were reachable
// from a ring-3 program. What is new is the LINK: the parser is
// compiled a second time with USERLAND_CFLAGS into libuapp.a, which is
// what /bin/netd, the File Manager and the window manager read /etc
// through, and a KTEST runs inside the kernel and cannot see that build.
//
// So the assertion is not "the parser works". It is "the ring-3 build
// of it produces byte-for-byte what the kernel's build produces", over
// the table both rings read (kernel/include/api/etc_config_cases.h).
//
// And since a document is allocated at its file's size: ring 3's own
// loader (lib/uconf.c) reading files either side of the old 4 KiB
// buffer and far past it, its cache's byte cap, and its frees.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "etc_config.h"
#include "etc_config_cases.h"
#include "lib/utest.h"
#include "lib/uconf.h"
#include "tmppath.h"
#include "heap.h"   // heap_used_bytes(): malloc's large blocks are heap_core's

static char g_got[256];

static const char *scratch(const char *name) {
    static char p[128];
    tmppath(p, sizeof p, TMP_VOLATILE, name);
    return p;
}

// Writes an etc_config_big_doc() of `bytes`; its length, or 0.
static uint32_t write_big(const char *path, uint32_t bytes) {
    uint32_t len = 0;
    char *doc = etc_config_big_doc(bytes, &uconf_mem, &len);
    if (!doc) return 0;
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    int64_t w = fd >= 0 ? sys_write(fd, doc, len) : -1;
    if (fd >= 0) sys_close(fd);
    free(doc);
    return w == (int64_t)len ? len : 0;
}

// The key on the LAST line, which a read cut anywhere would lose.
static void read_big(uint32_t bytes, const char *what) {
    const char *path = scratch("etc_config_big.conf");
    uint32_t len = write_big(path, bytes);
    struct etc_config_buf buf;
    int loaded = len && uconf_load(path, &buf);
    char v[16] = "", w[16] = "";
    int whole = loaded && buf.size == len && etc_config_buf_get(&buf, "last", v, sizeof v)
             && !strcmp(v, "end");
    uint32_t size = loaded ? buf.size : 0;
    etc_config_buf_free(&buf);
    int per_key = uconf_get(path, "last", w, sizeof w) && !strcmp(w, "end");
    sys_unlink(path);
    utest_checkf(whole && per_key, "%s: wrote %u, loaded %d at %u, last=\"%s\"/\"%s\"",
                 what, (unsigned)len, loaded, (unsigned)size, v, w);
}

static void big_files(void) {
    read_big(3 * 1024, "a 3 KiB file is read whole");
    read_big(5 * 1024, "a 5 KiB file is read whole -- over the old 4 KiB buffer");
    read_big(64 * 1024, "a 64 KiB file is read whole, its last key found");

    // A set that grows the file: the rewrite is sized from the value.
    const char *path = scratch("etc_config_grow.conf");
    uint32_t len = write_big(path, 5 * 1024);
    static char value[600], v[600];
    for (unsigned i = 0; i < sizeof value - 1; i++) value[i] = (char)('a' + i % 26);
    int set = uconf_set(path, "grown", value);
    struct sys_stat st;
    uint64_t after = sys_stat(path, &st) == 0 ? st.size : 0;
    int got = uconf_get(path, "grown", v, sizeof v) && !strcmp(v, value);
    char last[16];
    int kept = uconf_get(path, "last", last, sizeof last) && !strcmp(last, "end");
    int mid = uconf_set(path, "pad100", value)
           && uconf_get(path, "pad100", v, sizeof v) && !strcmp(v, value)
           && uconf_get(path, "last", last, sizeof last) && !strcmp(last, "end");
    sys_unlink(path);
    utest_checkf(len && set && after > len && got && kept && mid,
                 "a set grows a 5 KiB file: set %d, %u -> %llu bytes, read back %d, "
                 "last kept %d, middle key replaced %d", set, (unsigned)len,
                 (unsigned long long)after, got, kept, mid);

    // Over the sanity bound: refused, and a set does not clobber it.
    path = scratch("etc_config_huge.conf");
    sys_unlink(path);
    int first = uconf_set(path, "first", "1");
    int grown = sys_truncate(path, (unsigned long long)ETC_CONFIG_FILE_MAX + 1) == 0;
    struct etc_config_buf buf;
    int loaded = uconf_load(path, &buf);
    etc_config_buf_free(&buf);
    int clobbered = uconf_set(path, "second", "2");
    uint64_t size = sys_stat(path, &st) == 0 ? st.size : 0;
    int at = sys_truncate(path, ETC_CONFIG_FILE_MAX) == 0 && uconf_load(path, &buf)
          && etc_config_buf_get(&buf, "first", v, sizeof v) && !strcmp(v, "1");
    etc_config_buf_free(&buf);
    sys_unlink(path);
    utest_checkf(first && grown && !loaded && !clobbered
                 && size == (uint64_t)ETC_CONFIG_FILE_MAX + 1 && at,
                 "a file over the sanity bound is refused (loaded %d, set %d, size %llu), "
                 "one exactly at it read (%d)", loaded, clobbered,
                 (unsigned long long)size, at);
}

// The cache keeps each document at its size under a byte cap; two 5 KiB
// files under an 8 KiB cap evict each other, and that is said ONCE.
static void cache_cap(void) {
    static char a[128];
    strlcpy(a, scratch("etc_config_cap_a.conf"), sizeof a);
    const char *b = scratch("etc_config_cap_b.conf");
    uint32_t la = write_big(a, 5 * 1024), lb = write_big(b, 5 * 1024);
    uint32_t was = uconf_cache_set_cap(8 * 1024);
    unsigned long long h0 = uconf_cache_cap_hits();
    char v[16];
    int ok = uconf_get(a, "last", v, sizeof v) && uconf_get(b, "last", v, sizeof v);
    unsigned long long h1 = uconf_cache_cap_hits();
    ok = ok && uconf_get(a, "last", v, sizeof v) && uconf_get(b, "last", v, sizeof v);
    unsigned long long h2 = uconf_cache_cap_hits();
    uconf_cache_set_cap(was);
    sys_unlink(a);
    sys_unlink(b);
    utest_checkf(la && lb && ok && h1 > h0 && h2 > h1 && uconf_cache_cap_said() == 1,
                 "the cache's byte cap evicts (hits %llu -> %llu -> %llu) and is said once (%d)",
                 h0, h1, h2, uconf_cache_cap_said());
}

// Every load allocates the document: N loads, per-key reads and sets of
// a 5 KiB file must leave the heap where it was, give or take less than
// a document.
static void no_leak(void) {
    const char *path = scratch("etc_config_leak.conf");
    uint32_t len = write_big(path, 5 * 1024);
    char v[16];
    struct etc_config_buf buf;
    uconf_get(path, "last", v, sizeof v);   // warm: the cache's own copy
    uconf_set(path, "pad7", "warm");
    uconf_get(path, "last", v, sizeof v);
    uint64_t before = heap_used_bytes();
    for (int i = 0; i < 40; i++) {
        if (uconf_load(path, &buf)) etc_config_buf_get(&buf, "pad7", v, sizeof v);
        etc_config_buf_free(&buf);
        uconf_get(path, "last", v, sizeof v);
        uconf_set(path, "pad7", (i & 1) ? "odd" : "even");
    }
    uint64_t after = heap_used_bytes();
    sys_unlink(path);
    utest_checkf(len && after <= before + len,
                 "40 loads, reads and sets of a %u-byte file free what they take: "
                 "heap %llu -> %llu", (unsigned)len,
                 (unsigned long long)before, (unsigned long long)after);
}

int main(void) {
    utest_begin("etc_config_test", "the shared /etc parser, built for ring 3", 0);
    char detail[320];

    for (int i = 0; i < etc_get_case_count; i++) {
        int ok = etc_get_case_run(&etc_get_cases[i], &uconf_mem, g_got, sizeof g_got);
        snprintf(detail, sizeof detail, "got \"%s\" want \"%s\"",
                 g_got, etc_get_cases[i].want ? etc_get_cases[i].want : "(a miss)");
        utest_check_detail(ok, etc_get_cases[i].name, detail);
    }

    for (int i = 0; i < etc_set_case_count; i++) {
        int ok = etc_set_case_run(&etc_set_cases[i], &uconf_mem,
                                  g_got, sizeof g_got);
        snprintf(detail, sizeof detail, "got \"%s\" want \"%s\"",
                 g_got, etc_set_cases[i].want ? etc_set_cases[i].want : "(a refusal)");
        utest_check_detail(ok, etc_set_cases[i].name, detail);
    }

    for (int i = 0; i < etc_sections_case_count; i++) {
        int ok = etc_sections_case_run(&etc_sections_cases[i], &uconf_mem,
                                       g_got, sizeof g_got);
        snprintf(detail, sizeof detail, "got \"%s\" want \"%s\"",
                 g_got, etc_sections_cases[i].want);
        utest_check_detail(ok, etc_sections_cases[i].name, detail);
    }

    // An EMPTY table would print nothing and exit 0, which reads exactly
    // like every case passing.
    utest_checkf(etc_get_case_count >= 10 && etc_set_case_count >= 10
                 && etc_sections_case_count >= 4,
                 "%d/%d/%d cases -- the shared table is not linked",
                 etc_get_case_count, etc_set_case_count, etc_sections_case_count);

    big_files();
    cache_cap();
    no_leak();

    return utest_end();
}
