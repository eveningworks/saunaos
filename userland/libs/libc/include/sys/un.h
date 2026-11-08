// <sys/un.h> -- the address of a local (AF_UNIX) socket.
//
// **THE TYPE, NOT THE TRANSPORT.** The socket layer here is the network's
// (AF_INET, AF_INET6); socket(AF_UNIX, ...) is refused by the kernel
// with EAFNOSUPPORT. The header exists because ported code names the
// address type in a path it may never take -- GCC's SARIF output to a
// socket is one -- and a program that does take it gets the error at the
// socket() call, where it says what is missing. Linux's layout.
#ifndef _SYS_UN_H
#define _SYS_UN_H

#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_un {
    sa_family_t sun_family;    // AF_UNIX
    char        sun_path[108]; // the socket's path, NUL-terminated
};

#ifdef __cplusplus
}
#endif

#endif
