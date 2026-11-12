// A READ IN FLIGHT KEEPS THE FILE IT STARTED ON.
//
// A read looks its descriptor up, then works on the open-file
// description it found. If that description held no reference for the
// call, a close and reopen from another thread while the read waited
// freed it and handed it to the new file: the read then finished on the
// other file -- its path, its position. POSIX, and Linux's
// fdget()/fdput(), keep the old file until the call is done.
//
// So: a read of a file filled with 'A', while another thread closes
// that descriptor and opens a file filled with 'B' -- which gets the
// same number, and with no reference held the same description. The
// read must return 'A's.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS: nearly every run**, since a
// cached read takes microseconds and the close lands after it. Wrong
// with interrupts off in syscalls too, wherever the read waits for the
// disk or a lock. Red with the read held open by hand
// (docs/roadmap-details.md, "Interruptible syscalls").
//
// It must be SPAWNED, not `run` -- it runs a thread.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define FILE_A "/var/tmp/readref.a"
#define FILE_B "/var/tmp/readref.b"
#define SIZE 64
#define ROUNDS 8

static volatile int g_go;
static int g_fd, g_new_fd;

static int fill(const char *path, char tag) {
    char buf[SIZE];
    memset(buf, tag, sizeof buf);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    int ok = fd >= 0 && write(fd, buf, sizeof buf) == (long)sizeof buf;
    if (fd >= 0) close(fd);
    return ok;
}

// The other thread: once the read has begun, swap the file under it.
static void *swap(void *arg) {
    (void)arg;
    while (!g_go) sys_yield();
    sys_sleep_ms(10);   // the read is inside the kernel by now
    close(g_fd);
    g_new_fd = open(FILE_B, O_RDONLY);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    utest_begin("readref_test", "a read in flight keeps the file it started on",
                UTEST_VERDICT_FILE);
    utest_check(fill(FILE_A, 'A') && fill(FILE_B, 'B'), "wrote the two files");

    unsigned rounds = 0, reused = 0, foreign = 0, short_read = 0;
    for (int r = 0; r < ROUNDS; r++) {
        g_go = 0;
        g_new_fd = -1;
        g_fd = open(FILE_A, O_RDONLY);
        if (g_fd < 0) { short_read++; continue; }
        pthread_t t;
        if (pthread_create(&t, NULL, swap, NULL) != 0) { close(g_fd); short_read++; continue; }
        char buf[SIZE];
        memset(buf, 0, sizeof buf);
        g_go = 1;
        long n = read(g_fd, buf, sizeof buf);
        pthread_join(t, NULL);
        if (n != SIZE) short_read++;
        else if (memchr(buf, 'B', sizeof buf)) foreign++;
        if (g_new_fd == g_fd) reused++;   // the case that can go wrong
        if (g_new_fd >= 0) close(g_new_fd);
        rounds++;
    }
    unlink(FILE_A);
    unlink(FILE_B);

    // The fixture reached the code: the reopen took the read's number.
    utest_checkf(reused > 0, "%u rounds, the reopen took the read's descriptor in %u",
                 rounds, reused);
    utest_checkf(foreign == 0 && short_read == 0,
                 "every read returns the first file: %u returned the second, %u short",
                 foreign, short_read);
    return utest_end();
}
