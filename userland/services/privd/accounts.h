#ifndef PRIVD_ACCOUNTS_H
#define PRIVD_ACCOUNTS_H
// Adding, removing and changing a person's account (users D3a; the
// client half is base/uusers.h) -- /etc/passwd, /etc/group, /etc/shadow
// and the home, privd's alone, as accounts-daemon is under polkit.
// Each returns 0 or -errno; the errors are uusers.h's.

int acct_add(const char *name, const char *full, int admin, const char *password);
int acct_del(const char *name, int delete_home);
int acct_mod(const char *name, const char *field, const char *value);

#endif
