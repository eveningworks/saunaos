// See wm_fs.h.
#include "wm/wm_fs.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>

static int stat_entry(const char *path, struct sys_stat *out) {
    struct sys_stat st;
    if (!path || !*path || sys_stat(path, &st) < 0) return 0;
    if (out) *out = st;
    return 1;
}

int wm_fs_exists(const char *path) {
    if (path && path[0] == '/' && path[1] == '\0') return 1; // the root
    return stat_entry(path, 0);
}

int wm_fs_is_dir(const char *path) {
    if (path && path[0] == '/' && path[1] == '\0') return 1;
    struct sys_stat st;
    if (!stat_entry(path, &st)) return 0;
    return st.is_dir != 0;
}

uint32_t wm_fs_size(const char *path) {
    struct sys_stat st;
    if (!stat_entry(path, &st)) return 0;
    return st.is_dir || st.size > UINT32_MAX ? 0 : (uint32_t)st.size;
}

uint32_t wm_fs_read_into(const char *path, void *buf, uint32_t cap) {
    if (!path || !buf || cap == 0) return 0;

    // REFUSE an oversized file rather than truncating -- see wm_fs.h.
    // Checked before opening so the failure costs no I/O, and because a
    // partial read leaves the caller with something that parses.
    uint32_t size = wm_fs_size(path);
    if (size == 0 || size > cap) return 0;

    int fd = sys_open(path, 0); // 0 = read-only, see SYS_O_WRITE
    if (fd < 0) return 0;

    uint32_t got = 0;
    while (got < size) {
        int64_t r = sys_read(fd, (uint8_t *)buf + got, size - got);
        if (r <= 0) break; // 0 is EOF, negative is an error; both stop
        got += (uint32_t)r;
    }
    sys_close(fd);

    // A short read is a failed read here. The caller asked for a whole
    // file and every one of them parses what it gets.
    return got == size ? got : 0;
}
