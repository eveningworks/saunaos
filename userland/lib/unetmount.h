#ifndef ULIB_UNETMOUNT_H
#define ULIB_UNETMOUNT_H

// Saved network shares: one file per share in /etc/mounts.d, mounted at
// /net/<file name> by /bin/smbfsd. This is the file format, for the
// three things that touch it -- smbfsd (reads), the File Manager's
// Add-share dialog (writes, removes) and smbfsd again when "Remember
// password" saves a hash.
//
// ONE FILE PER SHARE, as services.d and settings.d are: adding one is
// writing a file, forgetting one is deleting it, and nothing rewrites a
// list other programs are reading. smbfsd notices either within a few
// seconds and mounts or unmounts to match.
//
//     # /etc/mounts.d/media
//     Source=//nas/media
//     User=reader
//     Hash=8846f7eaee8fb117ad06bdd830b7586c
//
// `Hash` is the NT hash of the password (MD4 of its UTF-16) -- all an
// NTLM login needs, so the password itself is never written. It is
// still a LOGIN for that account on that server to anyone who can read
// the file, and with no user accounts here everyone can; the Sign-in
// dialog says so beside the checkbox.
#include <stddef.h>

#define UNETMOUNT_DIR  "/etc/mounts.d"
#define UNETMOUNT_ROOT "/net"
#define UNETMOUNT_NAME_MAX 32

struct unetmount {
    char name[UNETMOUNT_NAME_MAX];   // the file's name; also /net/<name>
    char source[160];                // //server/share
    char user[64];                   // "" anonymous, "guest" the guest account
    char domain[64];                 // "" the server's own
    char hash[33];                   // 32 hex digits, or "" when not remembered
};

// A name is a file name and a path component: letters, digits, '-',
// '_' and '.', not starting with '.', at most 31 bytes. No spaces: it
// is also smbfsd's argument, and sys_spawn() splits on whitespace.
int unetmount_name_ok(const char *name);
// "/net/<name>".
void unetmount_point(const char *name, char *out, size_t cap);
// A free name for a share of `source`: its share name, then "name-2"...
void unetmount_suggest(const char *source, char *out, size_t cap);

// 1 and *out filled, or 0 for a missing or malformed file.
int unetmount_load(const char *name, struct unetmount *out);
// Writes the whole file, published by a rename. 1, or 0.
int unetmount_save(const struct unetmount *m);
int unetmount_remove(const char *name);
// The saved names, sorted; how many (at most `max`).
int unetmount_list(char (*names)[UNETMOUNT_NAME_MAX], int max);

#endif
