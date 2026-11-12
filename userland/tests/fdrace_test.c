// TWO OPENS AT ONCE MUST EACH GET THEIR OWN OPEN-FILE DESCRIPTION.
//
// Descriptions are a pool (kernel/proc/syscall_fd.c, fd_desc_alloc()),
// and a free one is claimed by finding refs == 0 and setting it to 1.
// If a thread can be preempted between the two, a second open claims the
// same slot: two descriptors then name ONE description, whose path,
// position and mode are whichever open wrote last, and the first close
// frees it under the other.
//
// So: several threads, each with its own small file filled with its own
// tag byte, each looping open / read / close on that file alone. Nothing
// is shared in ring 3. A thread that reads another's tag, or is refused
// on a descriptor it opened one line earlier, was handed a description
// somebody else also holds.
//
// The files stay cached after the first pass, so no call parks and the
// only way one thread's open interleaves with another's is a preemption
// inside the syscall.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: every run with interrupts
// off in syscalls**, which is the default boot. The claim is atomic
// there by construction, so this is evidence only on an image built with
// KCMDLINE="syscall_irq=on" (docs/boot-flags.md).
//
// It must be SPAWNED, not `run` -- threads need a scheduler slot.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define THREADS 4
#define FILE_SIZE 64
#define READ_SIZE 16
#define GIVE_UP 16      // refusals are logged by the kernel: stop early

struct worker {
    int id;
    char path[40];
    unsigned loops, open_failed, short_read, foreign, close_failed;
    int first_foreign;   // the tag that was read instead, or -1
};

static struct worker g_w[THREADS];
static volatile int g_stop;
static volatile unsigned g_bad;
static unsigned long long g_end;

static uint8_t tag_of(int id) { return (uint8_t)('A' + id); }

static void *work(void *arg) {
    struct worker *w = arg;
    uint8_t buf[READ_SIZE];
    while (!g_stop && g_bad < GIVE_UP && sys_monotonic_ns() < g_end) {
        int fd = open(w->path, O_RDONLY);
        if (fd < 0) { w->open_failed++; g_bad++; continue; }
        memset(buf, 0, sizeof buf);
        long n = read(fd, buf, READ_SIZE);
        if (n != READ_SIZE) {
            w->short_read++;
            g_bad++;
        } else {
            for (int i = 0; i < READ_SIZE; i++) {
                if (buf[i] == tag_of(w->id)) continue;
                if (w->first_foreign < 0) w->first_foreign = buf[i];
                w->foreign++;
                g_bad++;
                break;
            }
        }
        if (close(fd) != 0) { w->close_failed++; g_bad++; }
        w->loops++;
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = 3;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    utest_begin("fdrace_test", "opens at once: each descriptor its own description",
                UTEST_VERDICT_FILE);

    int made = 1;
    for (int i = 0; i < THREADS; i++) {
        struct worker *w = &g_w[i];
        w->id = i;
        w->first_foreign = -1;
        snprintf(w->path, sizeof w->path, "/var/tmp/fdrace.%d", i);
        uint8_t fill[FILE_SIZE];
        memset(fill, tag_of(i), sizeof fill);
        int fd = open(w->path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0 || write(fd, fill, sizeof fill) != (long)sizeof fill) made = 0;
        if (fd >= 0) close(fd);
    }
    utest_check(made, "wrote one tagged file per thread");

    g_end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    pthread_t t[THREADS];
    int started = 1;
    for (int i = 1; i < THREADS; i++)
        if (pthread_create(&t[i], NULL, work, &g_w[i]) != 0) { started = 0; t[i] = 0; }
    utest_check(started, "started the threads");
    work(&g_w[0]);
    g_stop = 1;
    for (int i = 1; i < THREADS; i++)
        if (t[i]) pthread_join(t[i], NULL);

    unsigned loops = 0, least = ~0u, open_failed = 0, short_read = 0, foreign = 0, close_failed = 0;
    int first_foreign = -1, whose = -1;
    for (int i = 0; i < THREADS; i++) {
        struct worker *w = &g_w[i];
        loops += w->loops;
        if (w->loops < least) least = w->loops;
        open_failed += w->open_failed;
        short_read += w->short_read;
        foreign += w->foreign;
        close_failed += w->close_failed;
        if (first_foreign < 0 && w->first_foreign >= 0) { first_foreign = w->first_foreign; whose = i; }
        unlink(w->path);
    }

    // The fixture reached the code: every thread ran, so opens overlapped.
    utest_checkf(least > 0, "%u open/read/close loops, the slowest thread %u", loops, least);
    utest_checkf(foreign == 0, "every read is the thread's own file: %u foreign "
                 "(thread %d read 0x%02x)", foreign, whose, first_foreign < 0 ? 0 : first_foreign);
    utest_checkf(open_failed == 0 && short_read == 0 && close_failed == 0,
                 "no call refused on a descriptor just opened: %u opens, %u reads, %u closes",
                 open_failed, short_read, close_failed);
    return utest_end();
}
