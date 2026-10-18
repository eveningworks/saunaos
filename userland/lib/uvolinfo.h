#ifndef ULIB_UVOLINFO_H
#define ULIB_UVOLINFO_H

// What one mounted volume or network share IS, gathered in one place:
// the kernel's QUERY_FSINFO record, the share's file in /etc/mounts.d
// and its server's last word -- for Properties' volume page and the File
// Manager's details pane, so the two show the same facts. The ring-3
// counterpart of lib/ufileinfo.h for a mount point rather than a file.
//
// Its facts come out as (key, value) rows, ready to draw and to log;
// a reader never formats a size or a state itself.

#include <stdint.h>

#define UVOL_POINT 64
#define UVOL_ROWS  10

struct uvolinfo_row { char key[16]; char value[96]; };

struct uvolinfo {
    char point[UVOL_POINT];    // "/", "/boot", "/net/media"
    char label[32];            // "System", "Boot", "media"
    char server[64];           // a share's host ("nas"), else ""
    char fs[16];               // "tfs3", "fat32", "smb"
    char device[16];           // "ata0p2"; "" for a share or a RAM volume
    int remote, readonly, persistent, root;
    int disks_mounted;         // under /mnt: Disks mounted it and may unmount it
    uint64_t total, used;      // bytes; total 0 = not known (a share not connected)
    // A share only.
    char source[64];           // "//nas/media"
    int state;                 // USERFS_ST_*
    char note[48];             // "SMB 3.1.1, signed", "No answer from nas"
    int session;               // opened for the session: no file in /etc/mounts.d
    char user[64], domain[64]; // from that file; "" anonymous
    int remembered;            // a password hash is saved
};

// The volume mounted exactly at `point`: 1, or 0 when nothing is.
int uvolinfo_load(const char *point, struct uvolinfo *out);
// The volume `path` lies on (the longest mount prefix): 1, or 0.
int uvolinfo_of(const char *path, struct uvolinfo *out);
// "System" for "/", "Boot", "Temporary", else the last component.
void uvolinfo_name(const char *point, char *out, int cap);
// "Connected", "Not connected", "Needs a password", ...
const char *uvolinfo_state_text(int state);
// "smb://nas/media" for a share, the mount point for a volume.
void uvolinfo_address(const struct uvolinfo *v, char *out, int cap);
// "Network share - read-only" / "Volume - tfs3 on ata0p2".
void uvolinfo_kind_text(const struct uvolinfo *v, char *out, int cap);
// The facts, in display order; how many.
int uvolinfo_rows(const struct uvolinfo *v, struct uvolinfo_row *out, int cap);
// "1.1T free of 3.6T", or "" when the size is not known.
void uvolinfo_space_text(const struct uvolinfo *v, char *out, int cap);
// Bytes free; 0 when the size is not known.
uint64_t uvolinfo_free(const struct uvolinfo *v);
// The largest NEW file that fits, leaving room for what the filesystem
// spends on it beside its data -- block maps, ~1/512 of it on tfs3, or a
// FAT cluster chain -- as 1/256 plus a MiB, so "fits" cannot be the run
// that fails at 99%. 0 when the size is not known.
uint64_t uvolinfo_room(const struct uvolinfo *v);
// bytes <= uvolinfo_room(v): the one test, so a window's limit and the
// program it starts cannot disagree.
int uvolinfo_fits(const struct uvolinfo *v, uint64_t bytes);
// Used, per mille (0..1000), or -1 when the size is not known.
int uvolinfo_used_pm(const struct uvolinfo *v);
// A share: drop its connection; the next access makes a new one. 0 or -errno.
int uvolinfo_disconnect(const char *point);

#endif
