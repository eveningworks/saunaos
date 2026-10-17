// Tests for per-process I/O accounting (api/scheduler.h's struct sched_io,
// QUERY_PROCIO). Task Manager's Read and Write columns are these counters;
// one that never moves shows every program as idle, and one charged to the
// wrong kind shows a /tmp writer as a disk writer. The assertions are
// DELTAS over a sum of every process and the kernel, because this runs in
// a live kernel with a desktop doing its own I/O and the charge lands on
// whichever process is current.
#include "ktest.h"
#include "tmppath.h"
#include "fs.h"
#include "scheduler.h"
#include "string.h"

static const char *scratch_path(void) {
    static char p[FS_PATH_MAX];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "ktest_procio.bin");
    return p;
}

static void total(struct sched_io *sum) {
    k_memset(sum, 0, sizeof *sum);
    struct sched_io io;
    for (int slot = -1; slot < scheduler_slot_end(); slot++) {
        if (!scheduler_io_get(slot, &io)) continue;
        for (int k = 0; k < SCHED_IO_KINDS; k++) {
            sum->bytes[k] += io.bytes[k];
            sum->ops[k] += io.ops[k];
        }
    }
}

KTEST("procio", "a charge lands on its own kind and nowhere else") {
    struct sched_io a, b;
    total(&a);
    scheduler_io_charge(SCHED_IO_FILE_WRITE, 12345);
    total(&b);
    KTEST_ASSERT(b.bytes[SCHED_IO_FILE_WRITE] - a.bytes[SCHED_IO_FILE_WRITE] >= 12345);
    KTEST_ASSERT(b.ops[SCHED_IO_FILE_WRITE] - a.ops[SCHED_IO_FILE_WRITE] >= 1);
    // An out-of-range kind is refused, not charged somewhere.
    total(&a);
    scheduler_io_charge(SCHED_IO_KINDS, 999);
    total(&b);
    for (int k = 0; k < SCHED_IO_KINDS; k++)
        KTEST_ASSERT(b.bytes[k] - a.bytes[k] < 999);
}

KTEST("procio", "the kernel's bucket and a slot that is no process") {
    struct sched_io io;
    KTEST_ASSERT_EQ(scheduler_io_get(-1, &io), 1);
    KTEST_ASSERT_EQ(scheduler_io_get(-2, &io), 0);
    KTEST_ASSERT_EQ(scheduler_io_get(scheduler_slot_end() + 1000000, &io), 0);
    KTEST_ASSERT_EQ(scheduler_io_get(0, 0), 0);
}

// THE ONE THAT WOULD CATCH THE BLOCK LAYER NOT CHARGING: a file written
// to a disk has to move the disk-write bytes by at least its size.
KTEST("procio", "writing a file to the disk charges disk-write bytes") {
    if (!fs_is_persistent()) { KTEST_SKIP("no persistent filesystem"); return; }
    static uint8_t buf[16384];
    k_memset(buf, 0x5A, sizeof buf);
    KTEST_ASSERT_EQ(fs_touch(scratch_path()), 1);
    struct sched_io a, b;
    total(&a);
    KTEST_ASSERT_EQ(fs_write_range(scratch_path(), 0, buf, sizeof buf), 1);
    total(&b);
    KTEST_ASSERT(b.bytes[SCHED_IO_DISK_WRITE] - a.bytes[SCHED_IO_DISK_WRITE] >= sizeof buf);
    KTEST_ASSERT(b.ops[SCHED_IO_DISK_WRITE] > a.ops[SCHED_IO_DISK_WRITE]);
    fs_delete(scratch_path());
}
