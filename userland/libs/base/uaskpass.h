#ifndef ULIB_UASKPASS_H
#define ULIB_UASKPASS_H

// Reading a password at a terminal: the prompt on stderr, the line from
// stdin with ECHO off, the terminal put back however the read ends --
// Ctrl+C included (BSD's readpassphrase(), not glibc's getpass(), which
// leaves echo off behind a signal). For `passwd`, `sudo`, `login`, `smb`.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The line's length, with `out` NUL-terminated; -1 at end of input, or
// when the line does not fit `cap` (E2BIG: nothing of it is kept). Not
// a terminal: the line is read as it is, with no prompt shown.
int uaskpass(const char *prompt, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
