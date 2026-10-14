#ifndef ULIB_UARCHIVE_H
#define ULIB_UARCHIVE_H

// AN ARCHIVE, WHICHEVER KIND IT IS -- .zip (lib/uzip.h), .tar, .tar.gz
// or .tgz, and a lone .gz (lib/utar.h) -- listed, a member taken out,
// or a new one made from files and folders. The File Manager's archive
// folder view and its Compress commands are built on this, so a format
// added here is one the File Manager opens.
//
// Read-only once made: there is no adding to or deleting from an
// existing archive, which is the subset Explorer offers too.

#include <stdint.h>
#include "syscall_abi.h"   // struct rtc_time

enum uarchive_kind { UARC_NONE, UARC_ZIP, UARC_TAR, UARC_TGZ, UARC_GZ };

// By name. UARC_NONE for anything else.
enum uarchive_kind uarchive_kind_of(const char *path);
// What a kind's name ends in: ".zip", ".tar", ".tar.gz", ".gz".
const char *uarchive_suffix(enum uarchive_kind k);
// A short label for the status bar: "ZIP", "tar", "tar + gzip", "gzip".
const char *uarchive_label(enum uarchive_kind k);

// "/home/x.tar.gz/a/b" -> archive "/home/x.tar.gz", inner "a/b": the
// archive ends at the FIRST component that names one. 0 when none does.
int uarchive_split(const char *path, char *archive, int acap, const char **inner);

// The name with its archive suffix taken off -- "x.tar.gz" -> "x" --
// for the folder an extract makes beside it.
int uarchive_stem(const char *path, char *out, int cap);

struct uarchive_entry {
    const char *name;            // full, '/'-separated; a folder's ends in '/'
    unsigned long long size;
    unsigned long long packed;   // its bytes in the archive, when has_packed
    int has_packed;              // 0 for a tar's members: gzip packs the stream
    int is_dir;
    struct rtc_time modified;
};
// Return non-zero to stop.
typedef int (*uarchive_fn)(void *ctx, const struct uarchive_entry *e);
int uarchive_list(const char *path, uarchive_fn fn, void *ctx, char *err, int errcap);

typedef int (*uarchive_progress)(void *ctx, unsigned long long done, unsigned long long total);

// One member to `out_path`, verified and renamed into place.
int uarchive_extract(const char *path, const char *member, const char *out_path,
                     uarchive_progress progress, void *ctx, char *err, int errcap);

// MANY MEMBERS, in one pass where the format needs it (a tar has no
// index, so one uarchive_extract() per member walks it whole each time).
// `route` returns 1 with `out` filled to take a member, 0 to pass it by;
// a folder's record reaches it too, and the caller makes the folder.
// Stops at the first failure.
typedef int (*uarchive_route)(void *ctx, const struct uarchive_entry *e, char *out, int cap);
int uarchive_extract_many(const char *path, uarchive_route route, void *ctx,
                          uarchive_progress progress, void *pctx, char *err, int errcap);

// A NEW ARCHIVE of `count` names in `dir` -- files, and folders with
// everything under them -- each member named from `dir` down, as a
// shell's `zip -r x.zip a b` from inside `dir` names them. `kind` is
// ZIP, TAR or TGZ; `level` 0 stores (ZIP), else a udeflate_level.
// `progress` sees bytes over the total measured first. Into
// `<out_path>.part` and renamed, so a cancel or failure leaves nothing.
int uarchive_create(const char *out_path, enum uarchive_kind kind, int level,
                    const char *dir, const char *const *names, int count,
                    uarchive_progress progress, void *ctx, char *err, int errcap);

#endif
