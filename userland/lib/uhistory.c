// See uhistory.h. A fixed ring of recent command lines.
//
// FIXED, not allocated, even though ring 3 has malloc() now. Two
// reasons and both are about what this is: it is bounded by what a
// person will scroll back through, so a ring that drops the oldest
// entry is the correct behaviour rather than a limitation -- and
// free() never returns memory to the kernel here (sbrk cannot move
// down), so a growing history would be a process footprint that only
// climbs. `struct uhistory` is ~4 KiB, which is why both front ends
// hold it at file scope: USERLAND_CFLAGS carries -Wframe-larger-than
// and a big local array in ring 3 steps over the single guard page.
//
// IT PERSISTS BY APPENDING, and to its own file. See the header for why
// both halves of that are the answer rather than a shortcut.
#include "lib/uhistory.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>    // rename()
#include <unistd.h>

void uhist_init(struct uhistory *h) {
    h->count = 0;
    h->head = 0;
    h->browse = 0;
    // CLEARED, because uhist_add() free()s the slot it is about to
    // overwrite: they held inert bytes before and hold pointers now, so
    // a struct that is not zero-initialised would free a garbage one on
    // its first add. Today's only caller is a file-scope static, which
    // is exactly why this would fail silently and at a distance.
    for (int i = 0; i < UHIST_MAX; i++) h->entries[i] = 0;
    h->pending = 0;
}

// Ring index of the entry `back` steps behind the newest (back = 1 is
// the newest). Callers guarantee 1 <= back <= count.
static int slot(const struct uhistory *h, int back) {
    int i = h->head - back;
    while (i < 0) i += UHIST_MAX;
    return i;
}

void uhist_add(struct uhistory *h, const char *line) {
    if (!line || !line[0]) return;
    if (h->count > 0 && h->entries[slot(h, 1)] &&
        strcmp(h->entries[slot(h, 1)], line) == 0) return;

    // Allocated to the line's own length, and the slot's previous
    // occupant freed. A failure leaves the slot empty rather than
    // holding a truncated command.
    size_t n = strlen(line) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, line, n);
    free(h->entries[h->head]);
    h->entries[h->head] = copy;
    h->head = (h->head + 1) % UHIST_MAX;
    if (h->count < UHIST_MAX) h->count++;
}

const char *uhist_prev(struct uhistory *h, const char *current) {
    if (h->browse >= h->count) return 0; // nothing older
    // Save what was being typed, once, on the way out of the live line.
    if (h->browse == 0) {
        free(h->pending);
        const char *cur = current ? current : "";
        size_t n = strlen(cur) + 1;
        h->pending = malloc(n);
        if (h->pending) memcpy(h->pending, cur, n);
    }
    h->browse++;
    return h->entries[slot(h, h->browse)];
}

const char *uhist_next(struct uhistory *h) {
    if (h->browse == 0) return 0; // not browsing
    h->browse--;
    if (h->browse == 0) return h->pending ? h->pending : ""; // the live line
    return h->entries[slot(h, h->browse)];
}

void uhist_reset(struct uhistory *h) {
    h->browse = 0;
    free(h->pending);   // the line the first Up staged, if the user browsed
    h->pending = 0;
}

const char *uhist_last(const struct uhistory *h) {
    if (h->count == 0) return 0;
    return h->entries[slot(h, 1)];
}

int uhist_count(const struct uhistory *h) { return h->count; }

const char *uhist_at(const struct uhistory *h, int i) {
    if (i < 0 || i >= h->count) return 0;
    // Index 0 is the OLDEST, so it is `count` steps behind the newest.
    return h->entries[slot(h, h->count - i)];
}

// --- the file ---------------------------------------------------------

void uhist_persist(const char *line) {
    if (!line || !line[0]) return;
    int fd = open(UHIST_FILE, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return;   // a read-only or RAM-only root is not an error here
    // ONE write, not two. The line and its terminator go out together so
    // that two shells appending at the same moment cannot interleave
    // half a line each -- which is the whole reason this appends rather
    // than rewriting.
    // Static: a ring-3 frame is budgeted at 2 KB and these two are not
    // re-entrant -- one shell, one file.
    static char buf[TOSH_HIST_LINE_MAX];
    size_t n = strlen(line);
    if (n > sizeof buf - 2) n = sizeof buf - 2;
    memcpy(buf, line, n);
    buf[n++] = '\n';
    write(fd, buf, n);
    close(fd);
}

// ONLY THE TAIL IS READ: the last UHIST_MAX lines fit in UHIST_MAX
// longest lines, so nothing before that can reach the ring. Reading the
// whole file cost a 3.3 MB history 6,500 reads at every shell start --
// every Terminal tab and every remote command.
#define UHIST_TAIL ((long)UHIST_MAX * TOSH_HIST_LINE_MAX)

// THE FILE IS TRIMMED once it passes UHIST_FILE_MAX, to its last
// UHIST_FILE_KEEP bytes (both in the header), through a temporary and an
// atomic rename -- bash's HISTFILESIZE. A line another shell appends
// between the read and the rename is lost, as with bash; a trim that
// fails leaves the file as it was.
static void trim(int fd, long size, const char *path) {
    static char tail[UHIST_FILE_KEEP];
    if (lseek(fd, size - UHIST_FILE_KEEP, SEEK_SET) < 0) return;
    long got = 0;
    while (got < UHIST_FILE_KEEP) {
        long n = read(fd, tail + got, (size_t)(UHIST_FILE_KEEP - got));
        if (n <= 0) return;
        got += n;
    }
    const char *start = memchr(tail, '\n', (size_t)got);   // a whole line first
    if (!start) return;
    start++;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) return;
    long n = (long)(tail + got - start);
    int ok = write(out, start, (size_t)n) == n;
    close(out);
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

void uhist_load(struct uhistory *h) { uhist_load_from(h, UHIST_FILE); }

void uhist_load_from(struct uhistory *h, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    long size = lseek(fd, 0, SEEK_END);
    long from = size > UHIST_TAIL ? size - UHIST_TAIL : 0;
    if (size < 0 || lseek(fd, from, SEEK_SET) < 0) { close(fd); return; }

    // What is kept is the LAST UHIST_MAX lines, which uhist_add()'s own
    // ring does for free -- every line is added and the early ones fall
    // off the back.
    static char chunk[4096], line[TOSH_HIST_LINE_MAX];
    int len = 0;
    int skip = from > 0;   // started mid-line: that part line is not a command
    for (;;) {
        long n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                line[len] = '\0';
                if (len && !skip) uhist_add(h, line);
                len = 0;
                skip = 0;
            } else if (len < (int)sizeof line - 1) {
                line[len++] = c;
            }
            // A line longer than the buffer is TRUNCATED rather than
            // split, because a split would put half a command in the
            // ring as if it were a whole one.
        }
    }
    if (len && !skip) { line[len] = '\0'; uhist_add(h, line); }
    if (size > UHIST_FILE_MAX) trim(fd, size, path);
    close(fd);
    uhist_reset(h);
}
