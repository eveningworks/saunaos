// <paths.h> -- the system's well-known paths, BSD's and glibc's header.
//
// Two only, each a fact about this system's layout
// (docs/filesystem-layout.md) rather than a Unix's list copied over:
//
//   _PATH_DEFPATH  the search path when PATH is unset -- the one init
//                  gives every session, so execvp(), posix_spawnp() and
//                  confstr(_CS_PATH) agree with the shell
//   _PATH_BSHELL   the POSIX shell, which is dash (/bin/sh is a copy of
//                  /bin/dash); the login shell is tosh, and system()
//                  asks the `system.shell` setting
#ifndef _PATHS_H
#define _PATHS_H

#define _PATH_DEFPATH "/bin:/usr/bin"
#define _PATH_BSHELL  "/bin/sh"

#endif
