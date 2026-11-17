#ifndef UITEST_HOST_H
#define UITEST_HOST_H

// The seam between uitest_sys.c (SaunaOS's headers) and uitest_host.c
// (the host's). Plain integers, strings and buffers only: a struct both
// libcs define differently must never cross here. Negative = -errno,
// whose numbers SaunaOS shares with Linux (abi/errno.h).
struct uh_stat {
    unsigned long long size, ino;
    long long mtime;
    unsigned is_dir, mode, nlink;
};

int       uh_open(const char *guest, int write, int create, int trunc, int append, int rdwr);
long long uh_read(int fd, void *buf, unsigned long len);
long long uh_write(int fd, const void *buf, unsigned long len);
int       uh_close(int fd);
long long uh_lseek(int fd, long long off, int whence);
int       uh_stat(const char *guest, struct uh_stat *out);
int       uh_fstat(int fd, struct uh_stat *out);
void     *uh_dir_open(const char *guest);
int       uh_dir_next(void *dir, char *name, unsigned long cap, int *is_dir);
void      uh_dir_close(void *dir);
int       uh_mkdir(const char *guest);
int       uh_unlink(const char *guest);
int       uh_rename(const char *from, const char *to);
void      uh_die(const char *msg);

// The app's coroutine: start it (not yet run), resume it until it yields
// or returns, yield from inside it. done: its function returned.
int       uh_coro_start(void (*fn)(void), unsigned long stack_bytes);
void      uh_coro_resume(void);
void      uh_coro_yield(void);
int       uh_coro_in_app(void);
int       uh_coro_done(void);

#endif
