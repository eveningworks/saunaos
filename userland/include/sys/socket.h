#ifndef ULIB_SYS_SOCKET_H
#define ULIB_SYS_SOCKET_H

// The TYPES of POSIX's <sys/socket.h>, and none of its functions.
//
// toy-os's sockets are its own calls (rt/sys.h's sys_net_*), which take
// addresses as integers; there is no socket()/connect()/bind() here, and
// they are ABSENT rather than stubbed -- unistd.h's rule: a link error
// says "this program needs something this OS has not got", which is
// true. The types exist because ported code passes them through
// interfaces that never open a socket: NetSurf parses an IP-literal
// host into a `struct in6_addr` and compares families.
#include <stdint.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_INET6  10    // Linux's value, as every other number here is
#define PF_UNSPEC AF_UNSPEC
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

#endif
