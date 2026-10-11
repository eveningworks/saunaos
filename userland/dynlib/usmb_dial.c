// usmb_dial() -- the SMB client's toy-os transport: a TCP connection
// through the kernel stack. Separate from usmb.c so the protocol file
// builds on the host, where tools/smb_hostcheck.py supplies a POSIX
// transport instead.
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "rt/sys.h"
#include "lib/uresolv.h"
#include "usmb.h"

struct dial { int fd; };

static int d_send(void *ctx, const void *buf, size_t len) {
    struct dial *d = ctx;
    const char *p = buf;
    while (len) {
        // write(), not sys_send(): SYS_SEND is the DATAGRAM call and
        // refuses a stream. A short write is the stream's convention.
        long n = write(d->fd, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// recvfrom() on a stream is read() with a ceiling on the wait: 0 when it
// expires, which the caller treats exactly like the end of the stream.
static int d_recv(void *ctx, void *buf, size_t len, unsigned timeout_ms) {
    struct dial *d = ctx;
    int64_t n = sys_recvfrom(d->fd, buf, len, NULL, NULL, timeout_ms ? timeout_ms : 1);
    return n < 0 ? -1 : (int)n;
}

static void d_close(void *ctx) {
    struct dial *d = ctx;
    if (d->fd >= 0) close(d->fd);
    free(d);
}

static int d_random(void *ctx, void *buf, size_t len) {
    (void)ctx;
    unsigned char *p = buf;
    while (len) {
        size_t n = len > SYS_GETRANDOM_MAX ? SYS_GETRANDOM_MAX : len;
        if (sys_getrandom(p, n) != (int)n) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int usmb_dial(const char *server, uint16_t port, unsigned timeout_ms, struct usmb_io *io) {
    uint32_t ip;
    if (!uresolv_parse_ip(server, &ip) && uresolv_lookup(server, 0, &ip) < 0)
        return USMB_E_RESOLVE;
    struct dial *d = malloc(sizeof *d);
    if (!d) return USMB_E_NOMEM;
    d->fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (d->fd < 0) { free(d); return USMB_E_NET; }
    if (sys_connect(d->fd, ip, port ? port : 445, timeout_ms) < 0) {
        // The stack reports a connect nobody answered as ECONNRESET.
        int timed_out = sys_errno() == ECONNRESET;
        d_close(d);
        return timed_out ? USMB_E_TIMEOUT : USMB_E_NET;
    }
    io->ctx = d;
    io->send = d_send;
    io->recv = d_recv;
    io->close = d_close;
    io->random = d_random;
    return 0;
}
