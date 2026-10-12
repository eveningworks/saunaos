#ifndef ULIB_NETINET_IN_H
#define ULIB_NETINET_IN_H

// POSIX's Internet address types. Types only, as <sys/socket.h> is.
#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;          // NETWORK byte order
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;   // network byte order
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)   // HOST byte order, as POSIX has it

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41

#endif
