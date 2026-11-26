#ifndef ULIB_UUSERS_H
#define ULIB_UUSERS_H
// THE PEOPLE'S ACCOUNTS (users D3a): who may sign in, what an account
// name may be, and the privd actions that add, remove and change one --
// what the login screen, Settings > Users, `user` and uui_newaccount
// share. An account is a PERSON's when its uid is 1000 or more and its
// shell is not a nologin (useradd's UID_MIN, GDM's and AccountsService's
// filter); `greeter` and root are not listed.
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uuser {
    char *name;      // the account name
    char *real;      // GECOS's first field, or the name when it is empty
    char *home;
    uid_t uid;
    int   admin;     // in `wheel` (primary group or member)
};

// Every person's account, in /etc/passwd's order, into a malloc'd array
// the caller frees with uusers_free(). The count, or -1 with errno.
int uusers_list(struct uuser **out);
void uusers_free(struct uuser *u, int n);

// Is `name` in `wheel`? By the account databases, as privd decides.
int uusers_is_admin(const char *name);

// AN ACCOUNT NAME a new account may take: 1-32 bytes, a lower-case ASCII
// letter or '_' first, then letters, digits, '_' and '-' (shadow-utils'
// NAME_REGEX, which every other tool reading /etc/passwd assumes).
// Says nothing about whether it is taken.
int uusers_name_ok(const char *name);

// THE NAME A FULL NAME SUGGESTS: its first word, lower-cased, Latin
// letters folded to ASCII (Ä -> a, ö -> o, é -> e), everything else
// dropped -- "Anna Virtanen" -> "anna", "Åke Öberg" -> "ake". "" when
// nothing in it fits a name. GNOME's and Windows' first guess, which the
// person may then edit.
void uusers_name_from_full(const char *full, char *out, int cap);

// A full name an account may have: no ':' or newline, at most
// UUSERS_FULL_MAX bytes -- what privd's prompt shows whole beside the rest
// of its sentence (a longer one would be refused there, E2BIG).
#define UUSERS_FULL_MAX 64
int uusers_full_ok(const char *full);

// --- the privd actions (an administrator's; base/upriv.h) ---------------
// 0, or -1 with errno: as upriv_action(), plus EEXIST (the name is
// taken), EINVAL (a name or full name the rules above refuse), EBUSY
// (the account has processes running -- signed in), EMLINK (it is the
// last administrator), ENOENT (no such account).
// `on_tty`: asked as `sudo` asks, on this terminal (a command-line
// tool; upriv_action_tty()); 0 through the desktop's prompt (an app).

// A new account `name` (full name `full`, may be ""), an administrator
// when `admin`, with `password` ("" for none); its home made from
// /etc/skel.
int uusers_add(const char *name, const char *full, int admin, const char *password, int on_tty);
// Removes `name`; its home deleted when `delete_home`, else kept.
int uusers_remove(const char *name, int delete_home, int on_tty);
// Changes one thing: `field` "admin" ("1"/"0"), "real" (the full name)
// or "password" (the new one, "" for none).
int uusers_set(const char *name, const char *field, const char *value, int on_tty);

#ifdef __cplusplus
}
#endif
#endif
