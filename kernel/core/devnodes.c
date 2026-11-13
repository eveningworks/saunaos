// /dev/null, /dev/zero, /dev/random and /dev/urandom -- Linux's
// drivers/char/mem.c and random.c in the small: four kinds of
// description with no state, found by NAME in sys_open() because there
// are no device nodes on disk to find them by. random and urandom are
// one source here, as on Linux since 5.18 (krandom_bytes(), whose
// quality QUERY_RANDOM reports).
#include "syscalls.h"
#include "syscall_abi.h"  // SYS_WRITE_MAX
#include "errno.h"
#include "vmm.h"
#include "krandom.h"
#include "string.h"
#include "devnodes.h"
#include "vt.h"          // VGA_VC_COUNT -- /dev/tty1../dev/ttyN

#define CHUNK 256   // on the stack, as sys_do_getrandom()'s

// A read is capped at SYS_WRITE_MAX, as a write is, so a 1 GiB read of
// /dev/zero is many calls rather than one that holds the CPU.
static uint64_t capped(uint64_t len) { return len > SYS_WRITE_MAX ? SYS_WRITE_MAX : len; }

static int fill_read(struct syscall_ctx *c, uint64_t ubuf, uint64_t len, int random) {
    len = capped(len);
    if (len && !vmm_validate_user_range(c->pml4, ubuf, len)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    unsigned char chunk[CHUNK];
    if (!random) k_memset(chunk, 0, sizeof chunk);
    for (uint64_t done = 0; done < len;) {
        uint64_t n = len - done > CHUNK ? CHUNK : len - done;
        if (random) krandom_bytes(chunk, (size_t)n);
        if (!vmm_copy_to_user(c->pml4, ubuf + done, chunk, n)) {
            c->regs[14] = (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        done += n;
    }
    c->regs[14] = len;
    return 0;
}

static int null_read(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len) {
    (void)f; (void)ubuf; (void)len;
    c->regs[14] = 0;   // end of file, always
    return 0;
}

static int zero_read(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len) {
    (void)f;
    return fill_read(c, ubuf, len, 0);
}

static int random_read(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len) {
    (void)f;
    return fill_read(c, ubuf, len, 1);
}

// Every write is taken whole and dropped -- random's included, which on
// Linux mixes the bytes in without crediting them; here they are ignored.
static int sink_write(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len) {
    (void)f; (void)ubuf;
    c->regs[14] = len;
    return 0;
}

static int64_t dev_seek(struct open_file *f, int64_t off, uint64_t whence) {
    (void)f; (void)off; (void)whence;
    return 0;   // Linux's null/zero answer 0 to any seek
}

const struct fd_ops null_fd_ops   = { .name = "null",    .read = null_read,
                                      .write = sink_write, .seek = dev_seek };
const struct fd_ops zero_fd_ops   = { .name = "zero",    .read = zero_read,
                                      .write = sink_write, .seek = dev_seek };
const struct fd_ops random_fd_ops = { .name = "random",  .read = random_read,
                                      .write = sink_write, .seek = dev_seek };

int devnode_vt(const char *path) {
    if (k_strncmp(path, "/dev/tty", 8) != 0) return 0;
    char d = path[8];
    return d >= '1' && d <= '0' + VGA_VC_COUNT && path[9] == '\0' ? d - '0' : 0;
}

const struct fd_ops *devnode_ops(const char *path) {
    if (k_strncmp(path, "/dev/", 5) != 0) return 0;
    const char *n = path + 5;
    if (!k_strcmp(n, "null"))    return &null_fd_ops;
    if (!k_strcmp(n, "zero"))    return &zero_fd_ops;
    if (!k_strcmp(n, "random") || !k_strcmp(n, "urandom")) return &random_fd_ops;
    return 0;
}
