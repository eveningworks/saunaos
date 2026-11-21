// `trace` -- the kernel event trace: switches, wakeups, interrupts, page
// faults, syscalls, contended spinlocks and kmutex sleeps, recorded per
// core by the kernel (`kernel.trace`, abi/ktrace_abi.h) and decoded
// here -- Linux's trace-cmd over ftrace, Windows' xperf over ETW.
//
//     trace [-s RECORDS] start [GROUPS]   arm (default: every group)
//     trace stop                          disarm; the records stay
//     trace [-n LINES] show               decoded, the cores merged by time
//     trace dump FILE                     the raw records, for trace_view.py
//     trace [-s RECORDS] run GROUPS PROGRAM [ARGS...]
//                                         start, run PROGRAM to its end, stop
//     trace                               what is armed, and what each core holds
//
// A short option comes BEFORE the command (uargs: what follows a command
// is its own), or as --size=/--lines= anywhere.
//
// The records come over QUERY_KTRACE, a few per call; a running trace
// moves between calls, so each core's records are kept by number and a
// repeat is dropped (query_abi.h). The lines are api/ktrace_fmt.h's, the
// formatter the kernel's panic tail uses too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "proc_info.h"
#include "ktrace_abi.h"
#include "ktrace_fmt.h"
#include "base/uargs.h"
#include "base/tunable.h"
#include "base/utrace.h"   // a syscall number's name

static const char *g_size, *g_lines;

static const struct uargs_opt OPTS[] = {
    { "size", 's', "RECORDS", "with start or run: records each core's ring holds (kernel.trace_size)", 0, &g_size },
    { "lines", 'n', "LINES", "with show: only the newest LINES", 0, &g_lines },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "start", "[GROUPS]", "record GROUPS (switch,wakeup,irq,fault,syscall,spin,mutex), or all" },
    { "stop", 0, "stop recording; what was recorded stays to be read" },
    { "show", 0, "print the records, every core's merged by time, newest last" },
    { "dump", "FILE", "write the raw records to FILE, for tools/dev/trace_view.py" },
    { "run", "GROUPS PROGRAM [ARGS...]", "start, run PROGRAM until it exits, then stop" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "trace",
    .usage = "\n[-s RECORDS] start [GROUPS]\nstop\n[-n LINES] show\ndump FILE\n[-s RECORDS] run GROUPS PROGRAM [ARGS...]",
    .summary = "Record kernel events -- switches, wakeups, interrupts, page faults, syscalls,\n"
               "lock waits -- on every core, and show or dump them.",
    .opts = OPTS,
    .cmds = CMDS,
};

// ---- reading ---------------------------------------------------------

struct core {
    struct query_ktrace_cpu info;
    struct ktrace_rec *rec;
    size_t n, cap;
    uint64_t next;     // the record number expected next
    uint64_t gaps;     // records that moved past us between two reads
};

struct snap {
    struct core *c;
    int n;
};

static int take_cpus(struct snap *s) {
    struct query_ktrace_cpu r;
    int cap = 0;
    QUERY_FOREACH(QUERY_KTRACE_CPU, r, i) {
        if (s->n == cap) {
            cap = cap ? cap * 2 : 4;
            struct core *nc = realloc(s->c, (size_t)cap * sizeof *nc);
            if (!nc) return 0;
            s->c = nc;
        }
        memset(&s->c[s->n], 0, sizeof s->c[s->n]);
        s->c[s->n++].info = r;
    }
    return 1;
}

static struct core *core_of(struct snap *s, uint32_t cpu) {
    for (int i = 0; i < s->n; i++)
        if (s->c[i].info.cpu == cpu) return &s->c[i];
    return 0;
}

static int keep(struct core *c, const struct ktrace_rec *r, uint64_t seq) {
    if (seq < c->next) return 1;            // seen in an earlier read
    if (c->n && seq > c->next) c->gaps += seq - c->next;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 1024;
        struct ktrace_rec *nr = realloc(c->rec, cap * sizeof *nr);
        if (!nr) return 0;
        c->rec = nr;
        c->cap = cap;
    }
    c->rec[c->n++] = *r;
    c->next = seq + 1;
    return 1;
}

static int take_records(struct snap *s) {
    struct query_ktrace q;
    QUERY_FOREACH(QUERY_KTRACE, q, i) {
        struct core *c = core_of(s, q.cpu);
        if (!c) continue;
        for (uint32_t k = 0; k < q.n && k < QUERY_KTRACE_PER; k++)
            if (!keep(c, &q.rec[k], q.seq + k)) return 0;
    }
    return 1;
}

static int snapshot(struct snap *s) {
    memset(s, 0, sizeof *s);
    if (!take_cpus(s) || !take_records(s)) {
        fprintf(stderr, "trace: out of memory reading the records\n");
        return 0;
    }
    return 1;
}

static const char *sysname(unsigned nr) { return utrace_name((int)nr); }

static uint64_t total(const struct snap *s) {
    uint64_t t = 0;
    for (int i = 0; i < s->n; i++) t += s->c[i].n;
    return t;
}

// ---- the commands ----------------------------------------------------

static int status(void) {
    char v[64];
    if (!tunable_get("kernel.trace", v, sizeof v)) {
        printf("trace: this kernel has no event trace (kernel.trace)\n");
        return 1;
    }
    char size[24] = "?";
    tunable_get("kernel.trace_size", size, sizeof size);
    printf("kernel.trace=%s, %s records per core\n", v, size);
    struct snap s;
    if (!snapshot(&s)) return 1;
    if (!s.n) printf("nothing recorded since boot\n");
    for (int i = 0; i < s.n; i++) {
        const struct query_ktrace_cpu *q = &s.c[i].info;
        printf("core %u: %llu records written, %llu held, %llu lost to a full ring\n", q->cpu,
               (unsigned long long)q->written, (unsigned long long)(q->written - q->lost),
               (unsigned long long)q->lost);
    }
    return 0;
}

static int arm(const char *groups) {
    uint32_t mask;
    if (!ktrace_parse_groups(groups, &mask))
        return uargs_error(&PROG, "%s is not a list of groups (switch,wakeup,irq,fault,syscall,spin,mutex, or all)",
                           groups);
    if (g_size && !tunable_set("kernel.trace_size", g_size)) {
        fprintf(stderr, "trace: the kernel refused %s records per core (64 up to a 32nd of RAM)\n", g_size);
        return 1;
    }
    if (!tunable_set("kernel.trace", groups)) {
        fprintf(stderr, "trace: the kernel refused to start (no calibrated TSC rate, or no memory: see dmesg)\n");
        return 1;
    }
    char size[24] = "?";
    tunable_get("kernel.trace_size", size, sizeof size);
    char now[64] = "?";
    tunable_get("kernel.trace", now, sizeof now);
    printf("trace: recording %s, %s records per core\n", now, size);
    return 0;
}

static int start(int argc, char **argv) {
    if (argc > 2) return uargs_error(&PROG, "start takes one list of groups");
    return arm(argc == 2 ? argv[1] : "all");
}

static int stop(void);

// trace-cmd record's and `perf record -- CMD`'s shape: the trace brackets
// one program, so what fills the rings is the program's stretch only.
static int run(int argc, char **argv) {
    if (argc < 3) return uargs_error(&PROG, "run takes GROUPS and a PROGRAM");
    // uargs compacts the operands in place, so argv[argc] is not NULL.
    char **child = calloc((size_t)argc - 1, sizeof *child);
    if (!child) return 1;
    for (int i = 2; i < argc; i++) child[i - 2] = argv[i];
    int rc = arm(argv[1]);
    if (rc) return rc;
    int pid = sys_spawn_argv(child[0], child);
    if (pid < 0) {
        fprintf(stderr, "trace: cannot run %s\n", argv[2]);
        stop();
        return 1;
    }
    int code = 0;
    sys_waitpid(pid, &code);
    rc = stop();
    printf("trace: %s (pid %d) exited %d\n", argv[2], pid, code);
    return rc;
}

static int stop(void) {
    if (!tunable_set("kernel.trace", "off")) {
        fprintf(stderr, "trace: the kernel refused to stop the trace\n");
        return 1;
    }
    struct snap s;
    if (!snapshot(&s)) return 1;
    uint64_t lost = 0;
    for (int i = 0; i < s.n; i++) lost += s.c[i].info.lost;
    printf("trace: stopped -- %llu records held", (unsigned long long)total(&s));
    if (lost) printf(", %llu lost to full rings (trace start -s raises the size)", (unsigned long long)lost);
    printf("\n");
    return 0;
}

static int show(void) {
    long last = -1;
    if (g_lines) {
        char *end;
        last = strtol(g_lines, &end, 10);
        if (*end || last < 1) return uargs_error(&PROG, "-n takes a count of lines, not %s", g_lines);
    }
    struct snap s;
    if (!snapshot(&s)) return 1;
    if (!s.n) {
        printf("trace: nothing recorded -- `trace start` first\n");
        return 1;
    }
    const struct query_ktrace_cpu *q0 = &s.c[0].info;
    struct ktrace_fmt_ctx ctx = { .t0_tsc = q0->armed_tsc, .tsc_hz = q0->tsc_hz, .syscall_name = sysname };
    uint64_t all = total(&s), skip = last > 0 && (uint64_t)last < all ? all - (uint64_t)last : 0;
    size_t *at = calloc((size_t)s.n, sizeof *at);
    if (!at) return 1;
    char line[256];
    // The cores merged by time: each is in order already.
    for (uint64_t k = 0; k < all; k++) {
        int best = -1;
        for (int i = 0; i < s.n; i++)
            if (at[i] < s.c[i].n &&
                (best < 0 || s.c[i].rec[at[i]].tsc < s.c[best].rec[at[best]].tsc)) best = i;
        const struct ktrace_rec *r = &s.c[best].rec[at[best]++];
        if (k < skip) continue;
        if (ktrace_fmt_rec(line, sizeof line, r, &ctx) > 0) printf("%s\n", line);
    }
    for (int i = 0; i < s.n; i++) {
        const struct core *c = &s.c[i];
        if (c->info.lost || c->gaps)
            printf("trace: core %u lost %llu records before they were read\n", c->info.cpu,
                   (unsigned long long)(c->info.lost + c->gaps));
    }
    return 0;
}

static int put(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return 0;
        b += w;
        n -= (size_t)w;
    }
    return 1;
}

static int dump(const char *path) {
    struct snap s;
    if (!snapshot(&s)) return 1;
    if (!s.n) {
        fprintf(stderr, "trace: nothing recorded -- `trace start` first\n");
        return 1;
    }
    unsigned nev;
    const struct ktrace_event_desc *ev = ktrace_event_table(&nev);
    unsigned nsys = 0;
    for (int nr = 0; nr < 4096; nr++) nsys += utrace_name(nr) != 0;
    // The threads alive now: a viewer's track names.
    int slots = sys_proc_max();
    struct ktrace_dump_name *names = calloc(slots > 0 ? (size_t)slots : 1, sizeof *names);
    unsigned nnames = 0;
    struct proc_info info;
    for (int i = 0; names && i < slots && sys_proc_info(i, &info) == 0; i++) {
        if (!info.pid) continue;
        names[nnames].tid = info.pid;
        names[nnames].tgid = info.tgid;
        strlcpy(names[nnames].name, info.name, sizeof names[nnames].name);
        nnames++;
    }

    struct ktrace_dump_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, KTRACE_DUMP_MAGIC, sizeof h.magic);
    h.version = KTRACE_DUMP_VERSION;
    h.hdr_size = sizeof h;
    h.tsc_hz = s.c[0].info.tsc_hz;
    h.armed_tsc = s.c[0].info.armed_tsc;
    h.mask = s.c[0].info.mask;
    h.ncpu = (uint32_t)s.n;
    h.nevents = nev;
    h.nsyscalls = nsys;
    h.nnames = nnames;
    h.rec_size = sizeof(struct ktrace_rec);
    h.cpu_size = sizeof(struct ktrace_dump_cpu);
    h.event_size = sizeof(struct ktrace_dump_event);
    h.syscall_size = sizeof(struct ktrace_dump_syscall);
    h.name_size = sizeof(struct ktrace_dump_name);
    h.nrecs = total(&s);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "trace: cannot write %s\n", path);
        return 1;
    }
    int ok = put(fd, &h, sizeof h);
    for (int i = 0; ok && i < s.n; i++) {
        struct ktrace_dump_cpu c = { .cpu = s.c[i].info.cpu, .capacity = s.c[i].info.capacity,
                                     .written = s.c[i].info.written,
                                     .lost = s.c[i].info.lost + s.c[i].gaps, .nrecs = s.c[i].n };
        ok = put(fd, &c, sizeof c);
    }
    for (unsigned i = 0; ok && i < nev; i++) {
        struct ktrace_dump_event e;
        memset(&e, 0, sizeof e);
        e.id = ev[i].id;
        e.group = ev[i].group;
        strlcpy(e.name, ev[i].name, sizeof e.name);
        strlcpy(e.args, ev[i].args, sizeof e.args);
        ok = put(fd, &e, sizeof e);
    }
    for (int nr = 0; ok && nr < 4096; nr++) {
        const char *nm = utrace_name(nr);
        if (!nm) continue;
        struct ktrace_dump_syscall sc;
        memset(&sc, 0, sizeof sc);
        sc.nr = (uint32_t)nr;
        strlcpy(sc.name, nm, sizeof sc.name);
        ok = put(fd, &sc, sizeof sc);
    }
    if (ok && nnames) ok = put(fd, names, nnames * sizeof *names);
    for (int i = 0; ok && i < s.n; i++)
        if (s.c[i].n) ok = put(fd, s.c[i].rec, s.c[i].n * sizeof(struct ktrace_rec));
    if (close(fd) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "trace: writing %s failed\n", path);
        return 1;
    }
    printf("trace: %llu records from %d cores to %s\n", (unsigned long long)h.nrecs, s.n, path);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (!a.argc) return status();
    const char *cmd = a.argv[0];
    if (!strcmp(cmd, "start")) return start(a.argc, a.argv);
    if (!strcmp(cmd, "run")) return run(a.argc, a.argv);
    if (a.argc > (strcmp(cmd, "dump") ? 1 : 2)) return uargs_error(&PROG, "%s takes no more arguments", cmd);
    if (!strcmp(cmd, "stop")) return stop();
    if (!strcmp(cmd, "show")) return show();
    if (!strcmp(cmd, "dump")) {
        if (a.argc != 2) return uargs_error(&PROG, "dump takes a FILE");
        return dump(a.argv[1]);
    }
    return uargs_error(&PROG, "no command %s", cmd);
}
