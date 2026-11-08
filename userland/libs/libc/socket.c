// <sys/socket.h>'s calls: POSIX's names over the native socket calls
// (rt/sys.h's sys_socket(), sys_connect() and the rest), which take an
// address as a host-order integer and a port. What changes on the way
// through is the ADDRESS: a `struct sockaddr_in` in network byte order
// becomes the two integers, and back. IPv4 only -- AF_INET6 and AF_UNIX
// are EAFNOSUPPORT, the answer of a system without the family -- and no
// flags on send/recv: each is refused rather than ignored.
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include "rt/sys.h"
#include "net_abi.h"

int socket(int domain, int type, int protocol) {
    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (protocol == 0)
        protocol = type == SOCK_STREAM ? IPPROTO_TCP : type == SOCK_DGRAM ? IPPROTO_UDP : 0;
    return sys_socket(NET_ABI_AF_INET, type, protocol);
}

// A sockaddr_in as the native calls want it. -1 with errno for anything
// else.
static int unpack(const struct sockaddr *sa, socklen_t len, uint32_t *ip, uint16_t *port) {
    if (!sa || len < sizeof(struct sockaddr_in)) { errno = EINVAL; return -1; }
    if (sa->sa_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    *ip = __builtin_bswap32(in->sin_addr.s_addr);
    *port = __builtin_bswap16(in->sin_port);
    return 0;
}

// The reverse, into the caller's buffer, truncating as POSIX says and
// reporting the full length.
static void pack(struct sockaddr *sa, socklen_t *len, uint32_t ip, uint16_t port) {
    if (!sa || !len) return;
    struct sockaddr_in in;
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_port = __builtin_bswap16(port);
    in.sin_addr.s_addr = __builtin_bswap32(ip);
    memcpy(sa, &in, *len < sizeof in ? *len : sizeof in);
    *len = sizeof in;
}

int connect(int fd, const struct sockaddr *sa, socklen_t len) {
    uint32_t ip;
    uint16_t port;
    if (unpack(sa, len, &ip, &port) < 0) return -1;
    return sys_connect(fd, ip, port, 0);
}

int bind(int fd, const struct sockaddr *sa, socklen_t len) {
    uint32_t ip;
    uint16_t port;
    if (unpack(sa, len, &ip, &port) < 0) return -1;
    return sys_bind(fd, ip, port, 0) < 0 ? -1 : 0;
}

// The backlog is the stack's own (sys_listen()); any value is accepted.
int listen(int fd, int backlog) {
    (void)backlog;
    return sys_listen(fd);
}

int accept(int fd, struct sockaddr *sa, socklen_t *len) {
    uint32_t ip = 0;
    uint16_t port = 0;
    int c = sys_accept(fd, &ip, &port, 0);
    if (c >= 0) pack(sa, len, ip, port);
    return c;
}

ssize_t send(int fd, const void *buf, size_t n, int flags) {
    if (flags) { errno = EINVAL; return -1; }
    return (ssize_t)sys_write(fd, buf, n);
}

ssize_t recv(int fd, void *buf, size_t n, int flags) {
    if (flags) { errno = EINVAL; return -1; }
    return (ssize_t)sys_read(fd, buf, n);
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *to, socklen_t len) {
    if (flags) { errno = EINVAL; return -1; }
    if (!to) return send(fd, buf, n, 0);
    uint32_t ip;
    uint16_t port;
    if (unpack(to, len, &ip, &port) < 0) return -1;
    return (ssize_t)sys_sendto(fd, buf, n, ip, port);
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *from, socklen_t *len) {
    if (flags) { errno = EINVAL; return -1; }
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t r = sys_recvfrom(fd, buf, n, &ip, &port, 0);
    if (r >= 0) pack(from, len, ip, port);
    return (ssize_t)r;
}
