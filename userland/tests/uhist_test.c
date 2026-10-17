// lib/uhistory's file: a shell start reads only the TAIL of a large
// history, keeps exactly its last UHIST_MAX lines whole, and trims a file
// past UHIST_FILE_MAX to a tail that still starts on a line.
//
// The read count is this process's own (QUERY_PROCIO): a load that read
// the whole file would still fill the ring correctly, so the ring alone
// cannot tell the two apart.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "lib/uhistory.h"
#include "lib/utest.h"

#define HIST  "/tmp/uhist_test.history"
#define LINES 12000   // "command 00000\n" x 12000 = 168 KB; doubled below

static struct uhistory g_h;
static char g_buf[4096];

static uint64_t my_file_reads(void) {
    int me = sys_getpid();
    struct query_procio r;
    QUERY_FOREACH(QUERY_PROCIO, r, i) {
        if (r.pid == me) return r.file_reads;
    }
    return (uint64_t)-1;   // no slot of our own: SPAWN this test, never `run` it
}

// `lines` lines "command NNNNN", in one file, written a block at a time.
static long write_history(int lines) {
    int fd = sys_open(HIST, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return -1;
    long total = 0;
    int len = 0;
    for (int i = 0; i < lines; i++) {
        len += snprintf(g_buf + len, sizeof g_buf - (size_t)len, "command %05d\n", i);
        if (len > (int)sizeof g_buf - 32 || i == lines - 1) {
            if (sys_write(fd, g_buf, (uint64_t)len) != len) { sys_close(fd); return -1; }
            total += len;
            len = 0;
        }
    }
    sys_close(fd);
    return total;
}

static long file_size(const char *path) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    long n = (long)sys_lseek(fd, 0, 2);
    sys_close(fd);
    return n;
}

int main(void) {
    utest_begin("uhist_test", "shell history: read the tail, trim a large file",
                UTEST_VERDICT_FILE);
    char want[32];

    // Small enough that nothing is trimmed: the ring and the read count.
    long size = write_history(LINES);
    utest_checkf(size > UHIST_MAX * 1024L && size < UHIST_FILE_MAX,
                 "a %ld-byte history, larger than the tail and under the trim", size);
    uhist_init(&g_h);
    uint64_t r0 = my_file_reads();
    uhist_load_from(&g_h, HIST);
    uint64_t r1 = my_file_reads();
    uint64_t reads = r1 - r0;
    utest_checkf(uhist_count(&g_h) == UHIST_MAX, "the ring holds %d lines (want %d)",
                 uhist_count(&g_h), UHIST_MAX);
    snprintf(want, sizeof want, "command %05d", LINES - 1);
    utest_check_detail(uhist_last(&g_h) && !strcmp(uhist_last(&g_h), want),
                       "the newest line is the file's last", uhist_last(&g_h));
    snprintf(want, sizeof want, "command %05d", LINES - UHIST_MAX);
    utest_check_detail(uhist_at(&g_h, 0) && !strcmp(uhist_at(&g_h, 0), want),
                       "the oldest is the 32nd from the end, whole", uhist_at(&g_h, 0));
    // The tail is UHIST_MAX lines of at most 1 KiB: 8 reads of 4 KiB and
    // the one that finds the end. The whole file would be ~40.
    utest_checkf(r0 != (uint64_t)-1 && reads >= 8 && reads <= 10,
                 "only the tail was read (%llu reads)", (unsigned long long)reads);
    utest_checkf(file_size(HIST) == size, "a history under the bound is left alone");

    // Past the bound: trimmed to a tail starting on a whole line.
    size = write_history(2 * LINES + 1000);
    utest_checkf(size > UHIST_FILE_MAX, "a %ld-byte history, over the bound", size);
    uhist_init(&g_h);
    uhist_load_from(&g_h, HIST);
    long after = file_size(HIST);
    utest_checkf(after > 0 && after <= UHIST_FILE_KEEP,
                 "it was trimmed (%ld -> %ld bytes)", size, after);
    int fd = sys_open(HIST, 0);
    long n = fd >= 0 ? (long)sys_read(fd, g_buf, 13) : -1;
    if (fd >= 0) sys_close(fd);
    utest_check(n == 13 && !memcmp(g_buf, "command ", 8) && g_buf[12] >= '0',
                "...to a tail that starts on a whole line");
    uhist_init(&g_h);
    uhist_load_from(&g_h, HIST);
    snprintf(want, sizeof want, "command %05d", 2 * LINES + 999);
    utest_check_detail(uhist_last(&g_h) && !strcmp(uhist_last(&g_h), want),
                       "...and still ends with the newest line", uhist_last(&g_h));
    utest_check(file_size(HIST ".new") < 0, "no temporary is left behind");

    sys_unlink(HIST);
    return utest_end();
}
