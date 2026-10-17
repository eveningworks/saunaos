// What each live process has read and written -- QUERY_PROCIO, Linux's
// /proc/<pid>/io. The counters are charged in the block layer and the
// file read/write syscalls (scheduler_io_charge()); this only lists them,
// processes first and the kernel's own bucket last as pid 0.
//
// driver-none: a QUERY provider over the scheduler's I/O counters
#include "query.h"
#include "scheduler.h"
#include "proc_info.h"
#include "string.h"
#include "initcall.h"

_Static_assert(sizeof(struct query_procio) <= QUERY_RECORD_MAX,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

// A process, not a thread: a thread's I/O is charged to its leader.
static int is_process(int slot) {
    struct proc_info pi;
    if (!scheduler_proc_info(slot, &pi) || !pi.pid) return 0;
    return !pi.tgid || pi.tgid == pi.pid;
}

static int procio_count(void) {
    int n = 1;   // the kernel
    for (int slot = 0; slot < scheduler_slot_end(); slot++) n += is_process(slot);
    return n;
}

static void fill(struct query_procio *q, int pid, const struct sched_io *io) {
    k_memset(q, 0, sizeof *q);
    q->pid = pid;
    q->disk_read_bytes = io->bytes[SCHED_IO_DISK_READ];
    q->disk_write_bytes = io->bytes[SCHED_IO_DISK_WRITE];
    q->disk_reads = io->ops[SCHED_IO_DISK_READ];
    q->disk_writes = io->ops[SCHED_IO_DISK_WRITE];
    q->file_read_bytes = io->bytes[SCHED_IO_FILE_READ];
    q->file_write_bytes = io->bytes[SCHED_IO_FILE_WRITE];
    q->file_reads = io->ops[SCHED_IO_FILE_READ];
    q->file_writes = io->ops[SCHED_IO_FILE_WRITE];
}

static int procio_fill(int index, void *out) {
    if (index < 0) return 0;
    struct sched_io io;
    for (int slot = 0; slot < scheduler_slot_end(); slot++) {
        if (!is_process(slot) || index--) continue;
        if (!scheduler_io_get(slot, &io)) return 0;
        fill(out, scheduler_slot_pid(slot), &io);
        return 1;
    }
    if (index != 0 || !scheduler_io_get(-1, &io)) return 0;
    fill(out, 0, &io);
    return 1;
}

static const struct query_provider procio_provider = {
    .cls = QUERY_PROCIO,
    .name = "procio",
    .record_size = sizeof(struct query_procio),
    .flags = QUERY_F_LIST,
    .count = procio_count,
    .fill = procio_fill,
    .fields = 0,
    .field_count = 0,
};

void procio_query_init(void) {
    query_register(&procio_provider);
}
INITCALL(procio_query_init, INIT_QUERY);
