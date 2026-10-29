// Tests for the /etc config reader/writer and, more to the point, for
// the thing that turned out to be missing around it: a setting that
// fails to persist must SAY so.
//
// The bug these come from: `timezone Helsinki` on a filesystem with no
// /etc printed "Timezone set to helsinki." and wrote nothing, because
// the timezone saver ignored etc_config_set()'s return value and three
// sibling savers returned void. Nothing was broken about the writer --
// it correctly reported failure to a caller that did not look. So the
// assertion worth having is not "the writer works", it is "the failure
// is reportable and the four setters report it".

#include "ktest.h"
#include "tmppath.h"
#include "fs.h"
#include "etc_config.h"
#include "etc_config_cases.h"
#include "kfmt.h"   // klog_printf, to name the case that failed
#include "font_config.h"
#include "cursor_config.h"
#include "keyboard_config.h"
#include "keyboard_layout.h"
#include "gfx.h"
#include "vga.h"
#include "fs.h"
#include "string.h"
#include "mount.h"  // the mount-epoch test
#include "heap.h"   // the leak check: heap_used_bytes()

// Scratch, under /tmp -- deliberately NOT /etc/saunaos.conf, which holds
// the live machine's real settings and which a test has no business
// rewriting (tests run inside the booted kernel, see ktest.h).
// Built from the configured directory rather than spelled out
// (api/tmppath.h), so moving scratch is a setting rather than a grep.
static const char *scratch_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_etc.conf");
    return p;
}
#define SCRATCH scratch_path()

static const char *missing_conf_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_no_such_file.conf");
    return p;
}
#define MISSING_CONF missing_conf_path()

KTEST("etc_config", "set then get round-trips a value") {
    fs_delete(SCRATCH); // may not exist; a failure here is not interesting
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "colour", "amber"), 1);

    char v[16];
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "amber"), 0);

    // Replacing a key in place, which is the case the four settings
    // actually exercise -- they rewrite the same key over and over.
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "colour", "green"), 1);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0);

    fs_delete(SCRATCH);
}

KTEST("etc_config", "unsetting a file's only key empties it rather than refusing") {
    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "only", "one"), 1);
    // The rewrite's length is 0 here, which used to read as "absent".
    KTEST_ASSERT_EQ(etc_config_unset(SCRATCH, "only"), 1);
    char v[16];
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "only", v, sizeof v), 0);
    // And a key that genuinely is not there is still refused.
    KTEST_ASSERT_EQ(etc_config_unset(SCRATCH, "only"), 0);
    fs_delete(SCRATCH);
}

KTEST("etc_config", "a write under a missing directory FAILS, and says so") {
    // The exact shape of the original bug: no directory, so the write
    // cannot land. What matters is that this returns 0 rather than
    // quietly doing nothing -- everything below depends on it.
    KTEST_ASSERT_EQ(etc_config_set("/no-such-dir-ktest/x.conf", "k", "v"), 0);
}

KTEST("etc_config", "the four setting savers report SETTING_SAVED on a good /etc") {
    // Each one re-saves the value that is already active, so a pass
    // changes nothing about the running machine -- but it does go all
    // the way through etc_config_set() to the real filesystem, which is
    // the only way to tell "returns a plausible constant" from
    // "actually wrote something".
    KTEST_ASSERT_EQ(font_config_save(gfx_font_size()), SETTING_SAVED);
    KTEST_ASSERT_EQ(cursor_config_save(vga_cursor_style()), SETTING_SAVED);
    KTEST_ASSERT_EQ(keyboard_config_save(keyboard_layout_current()), SETTING_SAVED);
}

// ---- read once, ask many --------------------------------------------

KTEST("etc_config", "a loaded buffer answers the same as a per-call read") {
    // EQUIVALENCE is the property worth pinning. etc_config_buf_get()
    // exists purely so a caller asking one file several questions stops
    // re-reading it (the desktop asked six keys per .desktop entry, so a
    // nine-entry reload did 54 whole-file reads) -- so the day it
    // answers differently from etc_config_get() is the day it is worse
    // than the thing it replaced. Both run one parser for that reason;
    // this is the assertion that they still do.
    fs_delete(SCRATCH);
    KTEST_ASSERT(etc_config_set(SCRATCH, "alpha", "one"));
    KTEST_ASSERT(etc_config_set(SCRATCH, "beta", "two"));

    struct etc_config_buf buf;
    KTEST_ASSERT(etc_config_load(SCRATCH, &buf));

    char from_buf[16], from_disk[16];
    KTEST_ASSERT(etc_config_buf_get(&buf, "alpha", from_buf, sizeof from_buf));
    KTEST_ASSERT(etc_config_get(SCRATCH, "alpha", from_disk, sizeof from_disk));
    KTEST_ASSERT_EQ(k_strcmp(from_buf, from_disk), 0);

    KTEST_ASSERT(etc_config_buf_get(&buf, "beta", from_buf, sizeof from_buf));
    KTEST_ASSERT_EQ(k_strcmp(from_buf, "two"), 0);

    // An absent key is 0 with `out` emptied, exactly as the per-call
    // form does it, so callers can use `out` without a second check.
    char none[16];
    KTEST_ASSERT_EQ(etc_config_buf_get(&buf, "missing", none, sizeof none), 0);
    KTEST_ASSERT_EQ(none[0], '\0');

    etc_config_buf_free(&buf);
    fs_delete(SCRATCH);
}

KTEST("etc_config", "an unloaded buffer answers nothing rather than garbage") {
    // The failure path callers depend on: a load that fails must make
    // every later get a clean miss, so a caller can load once and check
    // once rather than guarding every key.
    struct etc_config_buf buf;
    KTEST_ASSERT_EQ(etc_config_load(MISSING_CONF, &buf), 0);
    KTEST_ASSERT(buf.data == 0);   // nothing to free, and freeing is safe

    char out[16];
    KTEST_ASSERT_EQ(etc_config_buf_get(&buf, "alpha", out, sizeof out), 0);
    KTEST_ASSERT_EQ(out[0], '\0');
    etc_config_buf_free(&buf);
    etc_config_buf_free(&buf);     // twice: still nothing
}

KTEST("etc_config", "an invalid argument is INVALID, not a failed save") {
    // The three-way answer is the point: "you gave me nonsense" and "I
    // could not write it down" are different outcomes and a caller that
    // merges them cannot word its message correctly.
    KTEST_ASSERT_EQ(cursor_config_save(VGA_CURSOR_STYLE_COUNT), SETTING_INVALID);
    KTEST_ASSERT_EQ(keyboard_config_save(""), SETTING_INVALID);
}

// ---- sections -------------------------------------------------------
//
// The table is shared with the ring-3 build (api/etc_config_cases.h);
// these three walk it here, and userland/tests/etc_config_test.c walks
// the same rows over libuapp.a's second compilation of the parser.
static char g_case_got[256];

KTEST("etc_config", "the shared table: reading a section") {
    KTEST_ASSERT(etc_get_case_count >= 10); // an empty table asserts nothing
    for (int i = 0; i < etc_get_case_count; i++) {
        int ok = etc_get_case_run(&etc_get_cases[i], &etc_config_kmem,
                                  g_case_got, sizeof g_case_got);
        if (!ok) klog_printf("etc_config: get case \"%s\" gave \"%s\"\n",
                             etc_get_cases[i].name, g_case_got);
        KTEST_ASSERT(ok);
    }
}

KTEST("etc_config", "the shared table: writing into a section") {
    KTEST_ASSERT(etc_set_case_count >= 10);
    for (int i = 0; i < etc_set_case_count; i++) {
        int ok = etc_set_case_run(&etc_set_cases[i], &etc_config_kmem,
                                  g_case_got, sizeof g_case_got);
        if (!ok) klog_printf("etc_config: set case \"%s\" gave \"%s\"\n",
                             etc_set_cases[i].name, g_case_got);
        KTEST_ASSERT(ok);
    }
}

KTEST("etc_config", "the shared table: walking the sections") {
    KTEST_ASSERT(etc_sections_case_count >= 4);
    for (int i = 0; i < etc_sections_case_count; i++) {
        int ok = etc_sections_case_run(&etc_sections_cases[i], &etc_config_kmem,
                                       g_case_got, sizeof g_case_got);
        if (!ok) klog_printf("etc_config: sections case \"%s\" gave \"%s\"\n",
                             etc_sections_cases[i].name, g_case_got);
        KTEST_ASSERT(ok);
    }
}

// The cases above are buffer-to-buffer. This one goes through the FILE
// half, which is a different set of entry points and the one a setting
// actually persists through.
KTEST("etc_config", "a sectioned file round-trips through the disk") {
    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(etc_config_set_in(SCRATCH, "ipv4", "method", "dhcp"), 1);
    KTEST_ASSERT_EQ(etc_config_set_in(SCRATCH, "ipv4", "mtu", "1400"), 1);
    KTEST_ASSERT_EQ(etc_config_set_in(SCRATCH, "ipv6", "method", "off"), 1);
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "version", "1"), 1);

    char v[16];
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "method", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "dhcp"), 0);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv6", "method", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "off"), 0);
    // The top-level key is not in either section, and neither section's
    // key leaks to the top level.
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "version", v, sizeof v), 1);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "method", v, sizeof v), 0);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "version", v, sizeof v), 0);

    // ...and removing one leaves its neighbour alone.
    KTEST_ASSERT_EQ(etc_config_unset_in(SCRATCH, "ipv4", "mtu"), 1);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "mtu", v, sizeof v), 0);
    KTEST_ASSERT_EQ(etc_config_get_in(SCRATCH, "ipv4", "method", v, sizeof v), 1);
    KTEST_ASSERT_EQ(etc_config_unset_in(SCRATCH, "ipv4", "mtu"), 0); // gone already

    fs_delete(SCRATCH);
}

KTEST("etc_config", "an unchanged file is answered from memory, and a change is seen") {
    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "colour", "amber"), 1);
    char v[16];
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    uint64_t hits = etc_config_cache_hits();
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "amber"), 0);
    KTEST_ASSERT_EQ(etc_config_cache_hits(), hits + 1);

    // Same length, so only the generation can tell the two apart.
    KTEST_ASSERT_EQ(etc_config_set(SCRATCH, "colour", "green"), 1);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "green"), 0);

    // Renamed over, then deleted: neither writes the path itself.
    char other[FS_PATH_MAX];
    tmppath(other, sizeof other, TMP_PERSISTENT, "ktest_etc_other.conf");
    KTEST_ASSERT_EQ(etc_config_set(other, "colour", "white"), 1);
    KTEST_ASSERT_EQ(fs_rename_replace(other, SCRATCH), 1);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 1);
    KTEST_ASSERT_EQ(k_strcmp(v, "white"), 0);
    fs_delete(SCRATCH);
    KTEST_ASSERT_EQ(etc_config_get(SCRATCH, "colour", v, sizeof v), 0);
}

KTEST("etc_config", "a mount over a cached file's directory hides it, an unmount shows it") {
    char dir[FS_PATH_MAX], file[FS_PATH_MAX];
    const char *why;
    tmppath(dir, sizeof dir, TMP_PERSISTENT, "ktest_etc_mnt");
    k_strlcpy(file, dir, sizeof file);
    k_strlcat(file, "/a.conf", sizeof file);
    fs_mkdir(dir);
    KTEST_ASSERT_EQ(etc_config_set(file, "k", "disk"), 1);
    char v[16];
    KTEST_ASSERT_EQ(etc_config_get(file, "k", v, sizeof v), 1);   // now cached

    // Asserted after the cleanup: a failure must not leave the directory
    // on the image, where check_layout.py would then fail the gate.
    int mounted = mount_add(0, "ramfs", dir, 0, 0, &why);
    int hidden = mounted && !etc_config_get(file, "k", v, sizeof v);
    int unmounted = mounted && mount_remove(dir, &why);
    int shown = unmounted && etc_config_get(file, "k", v, sizeof v) &&
                k_strcmp(v, "disk") == 0;
    fs_delete(file);
    fs_delete(dir);
    KTEST_ASSERT(mounted);
    KTEST_ASSERT(hidden);
    KTEST_ASSERT(unmounted);
    KTEST_ASSERT(shown);
}

// ---- at the file's own size ------------------------------------------
//
// A file was read into a 4 KiB array and refused whole past it. These
// write documents either side of that and well past it, through the FILE
// half, and ask for the key on the LAST line -- the one a short read
// loses -- so a cap anywhere on the way turns them red.

static const char *big_path(const char *name) {
    static char p[FS_PATH_MAX];
    tmppath(p, sizeof p, TMP_PERSISTENT, name);
    return p;
}

// Writes a etc_config_big_doc() of `bytes` to `path`; its length, or 0.
static uint32_t write_big(const char *path, uint32_t bytes) {
    uint32_t len = 0;
    char *doc = etc_config_big_doc(bytes, &etc_config_kmem, &len);
    if (!doc) return 0;
    fs_delete(path);
    int ok = fs_write(path, doc, 0);
    kfree(doc);
    return ok ? len : 0;
}

static int read_big(uint32_t bytes) {
    const char *path = big_path("ktest_etc_big.conf");
    uint32_t len = write_big(path, bytes);
    struct etc_config_buf buf;
    int loaded = len && etc_config_load(path, &buf);
    char v[16] = "";
    int whole = loaded && buf.size == len
             && etc_config_buf_get(&buf, "last", v, sizeof v) && k_strcmp(v, "end") == 0;
    if (loaded) etc_config_buf_free(&buf);
    // ...and through the per-key call, which loads and frees inside.
    char w[16] = "";
    int per_key = len && etc_config_get(path, "last", w, sizeof w) && k_strcmp(w, "end") == 0;
    fs_delete(path);
    if (!whole || !per_key)
        klog_printf("etc_config: a %u-byte file: wrote %u, loaded %d (size %u), last=\"%s\"/\"%s\"\n",
                    bytes, len, loaded, loaded ? buf.size : 0, v, w);
    return whole && per_key;
}

KTEST("etc_config", "a 3 KiB file is read whole") {
    KTEST_ASSERT(read_big(3 * 1024));
}

KTEST("etc_config", "a 5 KiB file is read whole -- over the old 4 KiB buffer") {
    KTEST_ASSERT(read_big(5 * 1024));
}

KTEST("etc_config", "a 64 KiB file is read whole, its last key found") {
    KTEST_ASSERT(read_big(64 * 1024));
}

KTEST("etc_config", "a set that grows a big file past its old size keeps every key") {
    const char *path = big_path("ktest_etc_grow.conf");
    uint32_t len = write_big(path, 5 * 1024);
    KTEST_ASSERT(len);
    // A value long enough that the rewrite cannot fit the old document's
    // size: the out buffer has to be sized from the value, not the file.
    char value[600];
    for (uint32_t i = 0; i < sizeof value - 1; i++) value[i] = (char)('a' + i % 26);
    value[sizeof value - 1] = '\0';
    int set = etc_config_set(path, "grown", value);
    uint64_t after = fs_size(path);
    char v[sizeof value], last[16];
    int got = etc_config_get(path, "grown", v, sizeof v) && k_strcmp(v, value) == 0;
    int kept = etc_config_get(path, "last", last, sizeof last) && k_strcmp(last, "end") == 0;
    // And a key in the MIDDLE replaced in place by a longer value.
    int mid = etc_config_set(path, "pad100", value);
    int mid_got = etc_config_get(path, "pad100", v, sizeof v) && k_strcmp(v, value) == 0;
    int still = etc_config_get(path, "last", last, sizeof last) && k_strcmp(last, "end") == 0;
    fs_delete(path);
    KTEST_ASSERT_EQ(set, 1);
    KTEST_ASSERT(after > len);
    KTEST_ASSERT(got);
    KTEST_ASSERT(kept);
    KTEST_ASSERT_EQ(mid, 1);
    KTEST_ASSERT(mid_got);
    KTEST_ASSERT(still);
}

KTEST("etc_config", "a file over the sanity bound is refused, and a set does not clobber it") {
    const char *path = big_path("ktest_etc_huge.conf");
    fs_delete(path);
    KTEST_ASSERT_EQ(etc_config_set(path, "first", "1"), 1);
    // Sparse: growing a file is metadata only (fs_truncate()), so this
    // costs no megabyte of disk or of memory to set up.
    KTEST_ASSERT_EQ(fs_truncate(path, (uint64_t)ETC_CONFIG_FILE_MAX + 1), 1);
    struct etc_config_buf buf;
    int loaded = etc_config_load(path, &buf);
    etc_config_buf_free(&buf);
    // A rewrite of a file it cannot read whole must refuse, not start
    // from an empty document and replace the file with one key.
    int set = etc_config_set(path, "second", "2");
    uint64_t size = fs_size(path);
    // Exactly AT the bound is a config file still.
    int at_ok = fs_truncate(path, ETC_CONFIG_FILE_MAX);
    int at_loaded = at_ok && etc_config_load(path, &buf);
    char v[4];
    int at_first = at_loaded && etc_config_buf_get(&buf, "first", v, sizeof v);
    etc_config_buf_free(&buf);
    fs_delete(path);
    KTEST_ASSERT_EQ(loaded, 0);
    KTEST_ASSERT_EQ(set, 0);
    KTEST_ASSERT_EQ(size, (uint64_t)ETC_CONFIG_FILE_MAX + 1);
    KTEST_ASSERT(at_loaded);
    KTEST_ASSERT(at_first);
}

KTEST("etc_config", "the load cache's byte cap evicts, and logs ONCE") {
    const char *a = big_path("ktest_etc_cap_a.conf");
    static char a_path[FS_PATH_MAX];
    k_strlcpy(a_path, a, sizeof a_path);
    const char *b = big_path("ktest_etc_cap_b.conf");
    uint32_t la = write_big(a_path, 5 * 1024), lb = write_big(b, 5 * 1024);

    uint32_t was = etc_config_cache_set_cap(8 * 1024);   // one 5 KiB file fits, two do not
    uint64_t hits0 = etc_config_cache_cap_hits();
    char v[16];
    int r1 = etc_config_get(a_path, "last", v, sizeof v);   // cached
    int r2 = etc_config_get(b, "last", v, sizeof v);        // evicts a: the cap
    uint64_t hits1 = etc_config_cache_cap_hits();
    uint32_t logs1 = etc_config_cache_cap_logs();
    uint64_t served = etc_config_cache_hits();
    int r3 = etc_config_get(a_path, "last", v, sizeof v);   // a was dropped: a READ
    int a_missed = etc_config_cache_hits() == served;
    int r4 = etc_config_get(b, "last", v, sizeof v);        // b was evicted by a in turn
    uint64_t hits2 = etc_config_cache_cap_hits();
    uint32_t logs2 = etc_config_cache_cap_logs();
    etc_config_cache_set_cap(was);
    fs_delete(a_path);
    fs_delete(b);

    KTEST_ASSERT(la && lb);
    KTEST_ASSERT(r1 && r2 && r3 && r4);   // the cap costs reads, never answers
    KTEST_ASSERT(hits1 > hits0);
    KTEST_ASSERT(a_missed);
    KTEST_ASSERT(hits2 > hits1);
    KTEST_ASSERT_EQ(logs1, 1u);
    KTEST_ASSERT_EQ(logs2, 1u);           // said once, however often it is hit
}

// THE LEAK CHECK: a document is allocated per load now, so a load or a
// per-key read that forgot its free costs the file's size every call.
// N loads of a 5 KiB file must leave the heap where it was -- give or
// take less than one document, since the kernel is not idle meanwhile.
KTEST("etc_config", "loads, per-key reads and rewrites of a big file free what they allocate") {
    const char *path = big_path("ktest_etc_leak.conf");
    uint32_t len = write_big(path, 5 * 1024);
    KTEST_ASSERT(len);
    char v[16];
    struct etc_config_buf buf;
    etc_config_get(path, "last", v, sizeof v);   // warm: the cache's own copy
    uint64_t before = heap_used_bytes();
    for (int i = 0; i < 40; i++) {
        if (etc_config_load(path, &buf)) etc_config_buf_get(&buf, "pad7", v, sizeof v);
        etc_config_buf_free(&buf);
        etc_config_get(path, "last", v, sizeof v);
    }
    uint64_t after = heap_used_bytes();
    // A rewrite allocates its output too; one that leaked it would cost
    // the document again. The filesystem allocates beside it, so this
    // half asks only that 20 writes did not grow the heap by 20 files.
    uint64_t wbefore = heap_used_bytes();
    for (int i = 0; i < 20; i++) etc_config_set(path, "pad7", (i & 1) ? "odd" : "even");
    etc_config_get(path, "last", v, sizeof v);
    uint64_t wafter = heap_used_bytes();
    fs_delete(path);
    if (after > before + len || wafter > wbefore + 10ull * len)
        klog_printf("etc_config: heap %llu -> %llu over 40 loads, %llu -> %llu over 20 sets\n",
                    (unsigned long long)before, (unsigned long long)after,
                    (unsigned long long)wbefore, (unsigned long long)wafter);
    KTEST_ASSERT(after <= before + len);
    KTEST_ASSERT(wafter <= wbefore + 10ull * len);
}
