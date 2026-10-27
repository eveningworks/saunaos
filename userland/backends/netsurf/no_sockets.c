// socket(), for NetSurf alone: it REFUSES.
//
// desktop/gui_factory.c installs a default `socket_open` that calls
// socket(), and the only fetcher that opens sockets through that table
// is curl's, which is not built. tolibc has no socket() -- SaunaOS's
// sockets are its own calls -- so the reference would not link; this
// satisfies it here, beside the port, rather than giving every program
// a socket() that fails. Our fetchers (libhttp's, stage 2) do not use it.
#include <errno.h>

int socket(int domain, int type, int protocol);

int socket(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    errno = EAFNOSUPPORT;
    return -1;
}
