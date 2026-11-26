#ifndef PRIVD_PASS_H
#define PRIVD_PASS_H
// Passwords: /etc/shadow and yescrypt, privd's alone (docs/users-design.md,
// D2a). Nothing else on the system links a password hash, so this is the
// one place a password is ever compared -- LSASS's shape.

// 1 when `pw` is the account's password, 0 when it is not -- or the
// account cannot sign in by password at all (a locked "!" hash, or root
// with no line). An account with NO password matches only "".
int pass_check(const char *user, const char *pw);

// 1 when the account has a password set.
int pass_has(const char *user);

// Sets the account's password, or removes it when `pw` is NULL or "".
// /etc/shadow is replaced whole (written beside it, then renamed), so a
// power cut leaves the old file or the new one. 0, or -errno.
int pass_set(const char *user, const char *pw);

// The account's line taken out of /etc/shadow (a removed account), the
// file replaced as pass_set() does. 0, or -errno.
int pass_drop(const char *user);

#endif
