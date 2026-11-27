#ifndef ULIB_UNETMOUNT_H
#define ULIB_UNETMOUNT_H

// Saved network shares: one file per share in the PERSON's own
// $XDG_CONFIG_HOME/mounts.d (~/.config/mounts.d, the folder 0700 and each
// file 0600), mounted at /net/<file name> by /bin/smbfsd for the account
// whose session holds the seat (users D3b2a). This is the file format,
// for the things that touch it -- smbfsd (reads), the File Manager's
// Add-share dialog (writes, removes), the Sign-in dialog and `smb login`
// when "Remember password" saves a hash, and a session share's worker
// when it is saved (USERFS_CTL_SAVE) -- each writing as the person.
//
// ONE FILE PER SHARE, as services.d and settings.d are: adding one is
// writing a file, forgetting one is deleting it, and nothing rewrites a
// list other programs are reading. smbfsd notices either within a few
// seconds and mounts or unmounts to match.
//
//     # ~/.config/mounts.d/media
//     Source=//nas/media
//     User=reader
//     Hash=8846f7eaee8fb117ad06bdd830b7586c
//
// `Hash` is the NT hash of the password (MD4 of its UTF-16) -- all an
// NTLM login needs, so the password itself is never written. It is
// still a LOGIN for that account on that server to anyone who can read
// the file -- the person and root; the Sign-in dialog says so beside the
// checkbox.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNETMOUNT_SUBDIR "mounts.d"   // under the person's $XDG_CONFIG_HOME
#define UNETMOUNT_ROOT "/net"
#define UNETMOUNT_NAME_MAX 32
// The person's folder and a share's file in it: PATH_MAX, past which no
// path opens -- a home of any length that can be opened fits.
#define UNETMOUNT_PATH_MAX 4096

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

// The folder this process's saved shares are in: the person's, from
// XDG_CONFIG_HOME (else the account's home). Root has none of its own --
// smbfsd names the session's with unetmount_use_home(). 1 with `out`
// filled, 0 for none.
int unetmount_dir(char *out, size_t cap);
// Root's (smbfsd): read and write the shares of the account whose home is
// `home` from now on; NULL for none.
void unetmount_use_home(const char *home);

// 1 and *out filled, or 0 for a missing or malformed file.
int unetmount_load(const char *name, struct unetmount *out);
// Writes the whole file, 0600, published by a rename -- its folder made
// 0700 when missing. 1, or 0.
int unetmount_save(const struct unetmount *m);
int unetmount_remove(const char *name);
// The saved names, sorted, into `*names` GROWN to fit (`*cap` its
// capacity, the caller's to keep); how many.
int unetmount_list(char (**names)[UNETMOUNT_NAME_MAX], int *cap);

// ---- share ADDRESSES: smb://nas/media/Films, //nas/media, \\nas\media ----
//
// What a person types into an address bar, a file chooser or `open`.
// Opening one never needs a saved share: a share already mounted with
// that address is reused, and any other is mounted FOR THE SESSION by a
// worker of its own (`smbfsd --url`) at /net/<share>, gone after a
// restart unless saved -- Windows' \\server\share and Nautilus'
// smb://, neither of which asks you to map a drive first.

// 1 if `s` is spelled as a share address (not whether it exists).
int unetmount_is_address(const char *s);
// The address written the one way, "//server[:port]/share[/folder...]",
// with no trailing '/'. 1, or 0 if it names no share.
int unetmount_normalise(const char *in, char *out, size_t cap);
// Opens an address: writes the folder to go to (the mount point plus any
// folder inside the share) to `out`. 0, or -EINVAL (not a share address,
// or one with a space, which smbfsd's argument cannot carry), -ENOSPC
// (no name free), -EIO (the worker did not mount it within a few
// seconds). Connecting -- and asking for a password -- happens on the
// first listing, as with any share.
int unetmount_open_address(const char *addr, char *out, size_t cap);
// The same for a share ALREADY MOUNTED only, -ENOENT otherwise -- for a
// sandboxed caller that may not spawn the worker.
int unetmount_find_address(const char *addr, char *out, size_t cap);
// 1 if `point` is a network mount none of this person's saved shares
// names: one opened for the session.
int unetmount_is_session(const char *point);
// Keep a session mount across restarts, saved under `name`
// (USERFS_CTL_SAVE to the mount's server, which writes the file as the
// person whose worker it is -- a root worker writes none: -EPERM). 0, or
// -errno: -EEXIST when a share of that name is saved already.
int unetmount_save_session(const char *point, const char *name);

#ifdef __cplusplus
}
#endif

#endif
