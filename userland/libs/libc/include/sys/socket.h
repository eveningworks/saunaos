#ifndef ULIB_SYS_SOCKET_H
#define ULIB_SYS_SOCKET_H

// POSIX's <sys/socket.h>: the address types, and the calls over
// SaunaOS's own sockets (rt/sys.h's sys_socket() and friends, which take
// an address as a host-order integer) -- libc/socket.c converts. IPv4
// only: AF_INET6 and AF_UNIX are EAFNOSUPPORT at socket(). No socket
// options, no shutdown(), no getaddrinfo(): those are ABSENT, not
// stubbed (unistd.h's rule), and a flag passed to send()/recv() is
// refused with EINVAL rather than ignored.
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

#define AF_UNSPEC 0
// Named so <sys/un.h> compiles; the kernel refuses the family
// (EAFNOSUPPORT) -- there are no local sockets.
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10    // Linux's value, as every other number here is
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

// Large enough and aligned enough for any address family here.
struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_pad[118];
    uint64_t    __ss_align;
};

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);            // the backlog is the stack's
int accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *to, socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *from, socklen_t *len);

#ifdef __cplusplus
}
#endif

#endif
