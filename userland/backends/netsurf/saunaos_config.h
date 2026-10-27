// NetSurf's platform switches for SaunaOS, in place of its utils/config.h.
//
// Upstream decides what a platform has with an #if per operating system
// in utils/config.h, and has no branch for this one. The vendored tree is
// not edited (userland/ports/netsurf/README.md), so the build force-
// includes THIS file into every NetSurf unit (-include) and it claims
// config.h's include guard first -- mbedTLS's MBEDTLS_CONFIG_FILE, done
// with the tools C gives. Each switch below is a fact about tolibc;
// change one only with the library.
//
// THE TRAP: a HAVE_ left defined for something tolibc lacks compiles and
// then fails to LINK, which is the honest outcome -- but undefining one
// that tolibc HAS makes NetSurf define its own copy and the link fails
// on a duplicate. Either way the build says so.
#ifndef NETSURF_UTILS_CONFIG_H_
#define NETSURF_UTILS_CONFIG_H_

#include <stddef.h>

// What the upstream Makefiles pass on the command line, kept here so
// the strings need no shell quoting.
#define NETSURF_UA_FORMAT_STRING "Mozilla/5.0 (%s) NetSurf/%d.%d"
#define NETSURF_HOMEPAGE "about:welcome"
#define NETSURF_LOG_LEVEL VERBOSE
#define NETSURF_BUILTIN_LOG_FILTER "(level:WARNING || cat:jserrors)"
#define NETSURF_BUILTIN_VERBOSE_FILTER "(level:VERBOSE || cat:jserrors)"

// tolibc has these.
#define HAVE_STRNDUP
#define HAVE_STRFTIME
#define HAVE_STRTOULL
#define HAVE_SYS_SELECT          // the TYPES; select() itself is absent
#define HAVE_POSIX_INET_HEADERS  // likewise: types and inet_pton, no sockets API
#define HAVE_INETPTON
#define HAVE_REALPATH
#define HAVE_MKDIR
#define HAVE_SIGPIPE
#define HAVE_STDOUT
#define HAVE_SCANDIR
#define HAVE_REGEX

// tolibc has not, and NetSurf supplies its own (utils/utils.c).
#undef HAVE_STRCASESTR           // GNU/BSD, not C or POSIX
char *strcasestr(const char *haystack, const char *needle);
#undef HAVE_STRCHRNUL            // GNU
char *strchrnul(const char *s, int c);
#undef HAVE_STRPTIME
#undef HAVE_INETATON             // BSD, not POSIX; inet_pton is the POSIX one
#undef HAVE_UTSNAME              // uname() needs the host name, which is a
                                 // ring-3 setting tolibc cannot read yet

// tolibc has not, and the code has a portable path without them.
#undef HAVE_MMAP                 // a private file mapping is not offered
#undef HAVE_DIRFD                // no fd-relative (*at) calls: the kernel
#undef HAVE_UNLINKAT             //   resolves paths, not directory handles
#undef HAVE_FSTATAT
#undef HAVE_EXECINFO

// desktop/gui_factory.c's default socket opener calls socket(), which
// tolibc does not declare; ../no_sockets.c defines one that refuses.
int socket(int domain, int type, int protocol);

#endif
