// lib/uconf's per-process cache: a second ask of an unchanged file is
// answered without reading it, and ANY change -- a write that did not go
// through uconf, a file appearing -- is seen on the very next ask.
//
// "Without reading it" is measured, not assumed: this process's own
// file-read count (QUERY_PROCIO) must not move across the cached asks.
// A cache that re-read every time would pass every other check here.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "lib/uconf.h"
#include "lib/utest.h"

#define CONF  "/tmp/uconf_cache_test.conf"
#define LATER "/tmp/uconf_cache_test.later"

static uint64_t my_file_reads(void) {
    int me = sys_getpid();
    struct query_procio r;
    QUERY_FOREACH(QUERY_PROCIO, r, i) {
        if (r.pid == me) return r.file_reads;
    }
    return (uint64_t)-1;   // no slot of our own: SPAWN this test, never `run` it
}

// Behind uconf's back: a plain truncate-and-write.
static int raw_write(const char *path, const char *text) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return 0;
    int64_t n = sys_write(fd, text, strlen(text));
    sys_close(fd);
    return n == (int64_t)strlen(text);
}

static int ask(const char *path, const char *key, const char *want) {
    char got[64];
    int ok = uconf_get(path, key, got, sizeof got);
    return want ? ok && !strcmp(got, want) : !ok;
}

int main(void) {
    utest_begin("uconf_cache_test", "lib/uconf answers an unchanged file from memory",
                UTEST_VERDICT_FILE);
    sys_unlink(CONF);
    sys_unlink(LATER);

    utest_check(raw_write(CONF, "colour=blue\nsize=3\n"), "the file is written");
    utest_check(ask(CONF, "colour", "blue"), "the first ask reads it");

    uint64_t before = my_file_reads();
    int all = 1;
    for (int i = 0; i < 8; i++) all &= ask(CONF, i & 1 ? "size" : "colour", i & 1 ? "3" : "blue");
    uint64_t after = my_file_reads();
    utest_check(all, "eight more asks, two keys, all answered");
    utest_checkf(before != (uint64_t)-1 && after == before,
                 "...without reading the file again (file reads %llu -> %llu)",
                 (unsigned long long)before, (unsigned long long)after);

    utest_check(raw_write(CONF, "colour=red\n"), "the file is rewritten without uconf");
    utest_check(ask(CONF, "colour", "red"), "the next ask sees the new text");
    utest_check(ask(CONF, "size", 0), "...and a key it dropped is gone");

    utest_check(uconf_set(CONF, "size", "9") && ask(CONF, "size", "9"),
                "uconf_set() is seen by the next ask");

    utest_check(ask(LATER, "x", 0), "a missing file has no keys");
    utest_check(raw_write(LATER, "x=1\n") && ask(LATER, "x", "1"),
                "...and once it is created, the next ask finds it");

    sys_unlink(CONF);
    utest_check(ask(CONF, "colour", 0), "a deleted file is noticed");
    sys_unlink(LATER);
    return utest_end();
}
