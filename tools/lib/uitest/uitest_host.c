// The harness's half that talks to the HOST, compiled against the
// host's own headers -- unlike everything else in a uitest binary, which
// is compiled against SaunaOS's libc headers so the toolkit sees the
// types it was written for. The two never share a struct: everything
// crossing uitest_host.h is a plain integer, a string or a buffer, so a
// layout that differs between glibc and tolibc (struct stat, struct
// dirent) cannot leak through.
//
// **IT CALLS NOTHING BY A NAME THE OTHER HALF DEFINES.** One executable
// has one `stat`, and it is tolibc's (stat.c, linked for the toolkit) --
// a call to stat() here would reach THAT, hand it a glibc struct and the
// root-prefixed path, and come back ENOENT; mkdir() would recurse through
// sys_mkdir() back into uh_mkdir(). The same for opendir/readdir, which
// uitest_sys.c defines. So EVERY file call here goes to the kernel by
// number (syscall(2)) -- open, read, write, close, lseek, mkdir, unlink,
// rename as much as stat -- and struct stat is the kernel's.
//
// Every guest path is resolved under $UITEST_ROOT, which the runner
// builds from the tree (tools/hostchecks/uitest_hostcheck.py), so the
// host's own /etc or /usr/share/fonts can never answer.
#define _GNU_SOURCE
#include <dirent.h>   // DT_DIR
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "uitest_host.h"

static int host_path(const char *guest, char *out, size_t cap) {
    const char *root = getenv("UITEST_ROOT");
    if (!root || !*root || !guest) return 0;
    int n = guest[0] == '/' ? snprintf(out, cap, "%s%s", root, guest)
                            : snprintf(out, cap, "%s/%s", root, guest);
    return n > 0 && (size_t)n < cap;
}

int uh_open(const char *guest, int write, int create, int trunc, int append, int rdwr) {
    char p[4096];
    if (!host_path(guest, p, sizeof p)) return -ENOENT;
    int fl = rdwr ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    if (create) fl |= O_CREAT;
    if (trunc) fl |= O_TRUNC;
    if (append) fl |= O_APPEND;
    long fd = syscall(SYS_openat, AT_FDCWD, p, fl | O_CLOEXEC, 0644);
    return fd < 0 ? -errno : (int)fd;
}

long long uh_read(int fd, void *buf, unsigned long len) {
    long n = syscall(SYS_read, fd, buf, len);
    return n < 0 ? -errno : n;
}

long long uh_write(int fd, const void *buf, unsigned long len) {
    long n = syscall(SYS_write, fd, buf, len);
    return n < 0 ? -errno : n;
}

int uh_close(int fd) { return syscall(SYS_close, fd) < 0 ? -errno : 0; }

long long uh_lseek(int fd, long long off, int whence) {
    long r = syscall(SYS_lseek, fd, (off_t)off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    return r < 0 ? -errno : (long long)r;
}

static int fill(const struct stat *st, struct uh_stat *out) {
    memset(out, 0, sizeof *out);
    out->size = S_ISDIR(st->st_mode) ? 0 : (unsigned long long)st->st_size;
    out->ino = (unsigned long long)st->st_ino;
    out->is_dir = S_ISDIR(st->st_mode);
    out->mode = (unsigned)(st->st_mode & 07777);
    out->nlink = (unsigned)st->st_nlink;
    out->mtime = (long long)st->st_mtime;
    return 0;
}

int uh_stat(const char *guest, struct uh_stat *out) {
    char p[4096];
    struct stat st;
    if (!host_path(guest, p, sizeof p)) return -ENOENT;
    if (syscall(SYS_newfstatat, AT_FDCWD, p, &st, 0) < 0) return -errno;
    return fill(&st, out);
}

int uh_fstat(int fd, struct uh_stat *out) {
    struct stat st;
    if (syscall(SYS_fstat, fd, &st) < 0) return -errno;
    return fill(&st, out);
}

// A directory read with getdents64 into a buffer of our own.
struct linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct uh_dir {
    int fd, pos, len;
    char buf[8192];
};

void *uh_dir_open(const char *guest) {
    char p[4096];
    if (!host_path(guest, p, sizeof p)) return NULL;
    long fd = syscall(SYS_openat, AT_FDCWD, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0) return NULL;
    struct uh_dir *d = calloc(1, sizeof *d);
    if (!d) { syscall(SYS_close, fd); return NULL; }
    d->fd = (int)fd;
    return d;
}

// The next entry's name and whether it is a directory; 0 at the end.
// "." and ".." are skipped, as SYS_LISTDIR never reports them.
int uh_dir_next(void *dir, char *name, unsigned long cap, int *is_dir) {
    struct uh_dir *d = dir;
    for (;;) {
        if (d->pos >= d->len) {
            long n = syscall(SYS_getdents64, d->fd, d->buf, sizeof d->buf);
            if (n <= 0) return 0;
            d->len = (int)n;
            d->pos = 0;
        }
        struct linux_dirent64 *e = (struct linux_dirent64 *)(d->buf + d->pos);
        d->pos += e->d_reclen;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, cap, "%s", e->d_name);
        // THE ROOT IS BUILT OF SYMLINKS (/usr/share/fonts, each .ui file),
        // which getdents reports as DT_LNK, and some filesystems answer
        // DT_UNKNOWN: either is asked of stat, which follows the link.
        if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            struct stat st;
            *is_dir = syscall(SYS_newfstatat, d->fd, e->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);
        } else {
            *is_dir = e->d_type == DT_DIR;
        }
        return 1;
    }
}

void uh_dir_close(void *dir) {
    struct uh_dir *d = dir;
    if (!d) return;
    syscall(SYS_close, d->fd);
    free(d);
}

int uh_mkdir(const char *guest) {
    char p[4096];
    if (!host_path(guest, p, sizeof p)) return -ENOENT;
    return syscall(SYS_mkdirat, AT_FDCWD, p, 0755) < 0 ? -errno : 0;
}

int uh_unlink(const char *guest) {
    char p[4096];
    if (!host_path(guest, p, sizeof p)) return -ENOENT;
    return syscall(SYS_unlinkat, AT_FDCWD, p, 0) < 0 ? -errno : 0;
}

int uh_rename(const char *from, const char *to) {
    char a[4096], b[4096];
    if (!host_path(from, a, sizeof a) || !host_path(to, b, sizeof b)) return -ENOENT;
    return syscall(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b) < 0 ? -errno : 0;
}

// The real fopen, reached through ld's --wrap: the toolkit's fopen() of
// a guest path lands under the root too.
FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    char p[4096];
    if (!host_path(path, p, sizeof p)) { errno = ENOENT; return NULL; }
    return __real_fopen(p, mode);
}

void uh_die(const char *msg) {
    fprintf(stderr, "uitest: %s\n", msg);
    exit(99);
}
