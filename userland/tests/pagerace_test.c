// TWO PAGES ALLOCATED AT ONCE ARE TWO FRAMES.
//
// The frame allocator finds a clear bit and then sets it
// (kernel/mm/pmm.c). A syscall that faults a page in -- a read into a
// buffer never touched -- allocates with interrupts on when they are
// allowed inside a call, and a second allocation preempting it between
// the test and the set took the same frame: one physical page mapped
// twice, each owner writing over the other.
//
// So: two threads, each reading its own tagged file into pages of its
// own that have never been touched, then checking every page still
// holds its own tag.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls**, and nearly every one with them on. Evidence on an
// image built with KCMDLINE="syscall_irq=on", red there with the gap
// widened by hand (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- it runs a thread.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "rt/sys.h"
#include "base/utest.h"

#define PAGE 4096
#define PAGES 12

struct worker {
    int id;
    char path[40];
    uint8_t *pages;
    unsigned filled, foreign;
};

static struct worker g_w[2];

static uint8_t tag_of(int id) { return (uint8_t)('P' + id); }

static void *work(void *arg) {
    struct worker *w = arg;
    int fd = open(w->path, O_RDONLY);
    if (fd < 0) return 0;
    for (int p = 0; p < PAGES; p++) {
        lseek(fd, 0, SEEK_SET);
        if (read(fd, w->pages + (size_t)p * PAGE, PAGE) == PAGE) w->filled++;
        sys_yield();   // let the other thread's fault-ins interleave
    }
    close(fd);
    for (int p = 0; p < PAGES; p++)
        for (int i = 0; i < PAGE; i++)
            if (w->pages[(size_t)p * PAGE + i] != tag_of(w->id)) { w->foreign++; break; }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    utest_begin("pagerace_test", "two pages faulted in at once are two frames",
                UTEST_VERDICT_FILE);

    int made = 1;
    for (int i = 0; i < 2; i++) {
        struct worker *w = &g_w[i];
        w->id = i;
        snprintf(w->path, sizeof w->path, "/var/tmp/pagerace.%d", i);
        uint8_t fill[PAGE];
        memset(fill, tag_of(i), sizeof fill);
        int fd = open(w->path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0 || write(fd, fill, sizeof fill) != PAGE) made = 0;
        if (fd >= 0) close(fd);
        // Never touched from ring 3: each page is faulted in by the read.
        w->pages = mmap(NULL, (size_t)PAGES * PAGE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (w->pages == MAP_FAILED) { w->pages = 0; made = 0; }
    }
    utest_check(made, "wrote the files and mapped the pages");
    if (!made) return utest_end();

    pthread_t t;
    int started = pthread_create(&t, NULL, work, &g_w[1]) == 0;
    utest_check(started, "started the second thread");
    work(&g_w[0]);
    if (started) pthread_join(t, NULL);
    for (int i = 0; i < 2; i++) unlink(g_w[i].path);

    utest_checkf(g_w[0].filled == PAGES && g_w[1].filled == PAGES,
                 "every page was read into: %u and %u of %d", g_w[0].filled, g_w[1].filled, PAGES);
    utest_checkf(g_w[0].foreign == 0 && g_w[1].foreign == 0,
                 "every page holds its own thread's bytes: %u and %u foreign",
                 g_w[0].foreign, g_w[1].foreign);
    return utest_end();
}
