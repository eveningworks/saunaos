#ifndef ULIB_UCAPTURE_H
#define ULIB_UCAPTURE_H

// ucapture -- run a program and take what it prints: popen("r") without
// a shell. Its standard error goes to the log; the call waits for it to
// end. For asking a helper a question it answers in lines -- `w16
// --check`, a desktop entry's X-Details command.

// `bin` with `argv` (argv[0] its name, NULL-ended). Its standard output,
// NUL-ended, allocated (free() it); NULL when it could not be started or
// memory ran out.
char *ucapture(const char *bin, char *const argv[]);

#endif
