// A file read by range through a page cache. See lib/urange.h.
#include "lib/urange.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int urange_open(struct urange *r, const char *path) {
    memset(r, 0, sizeof *r);
    r->fd = -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -(errno ? errno : ENOENT);
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -EIO; }
    if (S_ISDIR(st.st_mode)) { close(fd); return -EISDIR; }
    r->data = malloc((size_t)URANGE_PAGES * URANGE_PAGE);
    if (!r->data) { close(fd); return -ENOMEM; }
    r->fd = fd;
    r->size = (uint64_t)st.st_size;
    for (int i = 0; i < URANGE_PAGES; i++) r->page[i].index = UINT64_MAX;
    return 0;
}

void urange_close(struct urange *r) {
    if (r->fd >= 0) close(r->fd);
    free(r->data);
    memset(r, 0, sizeof *r);
    r->fd = -1;
}

// The slot holding page `index`, reading it in over the least recently
// used one; NULL when the file cannot be read there.
static struct urange_page *page_for(struct urange *r, uint64_t index, uint8_t **data) {
    int victim = 0;
    for (int i = 0; i < URANGE_PAGES; i++) {
        if (r->page[i].index == index) {
            r->page[i].used = ++r->clock;
            *data = r->data + (size_t)i * URANGE_PAGE;
            return &r->page[i];
        }
        if (r->page[i].used < r->page[victim].used) victim = i;
    }
    struct urange_page *p = &r->page[victim];
    uint8_t *buf = r->data + (size_t)victim * URANGE_PAGE;
    uint64_t off = index * URANGE_PAGE;
    p->index = UINT64_MAX;   // invalid until the read lands
    if (lseek(r->fd, (off_t)off, SEEK_SET) != (off_t)off) return 0;
    size_t want = r->size - off < URANGE_PAGE ? (size_t)(r->size - off) : URANGE_PAGE, got = 0;
    while (got < want) {
        ssize_t n = read(r->fd, buf + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (!got) return 0;
    r->misses++;
    p->index = index;
    p->len = (uint32_t)got;
    p->used = ++r->clock;
    *data = buf;
    return p;
}

size_t urange_read(struct urange *r, uint64_t off, void *buf, size_t n) {
    if (r->fd < 0 || off >= r->size) return 0;
    if (n > r->size - off) n = (size_t)(r->size - off);
    size_t done = 0;
    while (done < n) {
        uint64_t at = off + done;
        uint8_t *data;
        struct urange_page *p = page_for(r, at / URANGE_PAGE, &data);
        if (!p) break;
        size_t in = (size_t)(at % URANGE_PAGE);
        if (in >= p->len) break;
        size_t take = p->len - in;
        if (take > n - done) take = n - done;
        memcpy((uint8_t *)buf + done, data + in, take);
        done += take;
    }
    return done;
}

size_t urange_reader(void *ctx, uint64_t off, void *buf, size_t n) {
    return urange_read((struct urange *)ctx, off, buf, n);
}
