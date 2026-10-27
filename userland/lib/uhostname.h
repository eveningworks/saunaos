#ifndef ULIB_UHOSTNAME_H
#define ULIB_UHOSTNAME_H

// This computer's name -- `system.hostname`, what About shows, what a VNC
// viewer's title bar says and what the remote-desktop certificate names.
// A setting rather than Linux's /etc/hostname + sethostname(): nothing in
// the kernel needs it, so it lives where every other ring-3 preference does.

#define UHOSTNAME_SETTING "system.hostname"
#define UHOSTNAME_DEFAULT "saunaos"
#define UHOSTNAME_MAX     63          // one DNS label (RFC 1123)

// The name into `out`, or UHOSTNAME_DEFAULT when unset or invalid.
void uhostname_get(char *out, int cap);

// RFC 1123's label: 1-63 of A-Z a-z 0-9 and '-', not starting or ending
// with '-'. A parser that REJECTS rather than guesses -- "My PC" is not
// turned into "My-PC" behind anyone's back. 1 when valid.
int uhostname_valid(const char *name);

#endif
