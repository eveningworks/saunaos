#ifndef ULIB_UTAR_H
#define ULIB_UTAR_H

// TAR ARCHIVES, plain or gzipped, and a lone .gz file -- reading one
// member out, listing them, and writing a new archive. POSIX ustar, plus
// what GNU tar and every other writer put in it: a GNU long name ('L')
// and a pax record's `path=` for a name ustar cannot hold. Links,
// devices and FIFOs are listed past, never extracted.
//
// **A TAR HAS NO INDEX**, unlike a zip's central directory: listing it
// and taking one member out both walk the whole stream from the start,
// and a .tar.gz has to be inflated all the way through to do it. The
// COMPRESSED file is held in memory for that (a .tar.gz of a few hundred
// MB is the limit); the inflated stream never is.
//
// A PARSER HERE REJECTS rather than guesses: a header whose checksum
// does not match ends the walk with an error, and an extracted member
// must come out at the size its header gave -- or nothing is left at
// `out_path`.

#include <stdint.h>
#include "lib/uinflate.h"

#define UTAR_NAME_MAX 256

enum utar_kind { UTAR_NONE, UTAR_TAR, UTAR_TGZ, UTAR_GZ };

// By name: .tar, .tar.gz/.tgz, and any other .gz (one compressed file).
enum utar_kind utar_kind_of(const char *path);

typedef int (*utar_progress)(void *ctx, unsigned long done, unsigned long total);

// Every member, in the archive's order: its full name ('/'-separated, a
// folder's ending in '/'), its size, its modification time in seconds
// since 1970, and whether it is a folder. Return non-zero to stop. A
// lone .gz lists one member: its name without the .gz, its size from
// the gzip trailer. 0, or a negative errno with `err` a sentence.
typedef int (*utar_entry_fn)(void *ctx, const char *name, unsigned long long size,
                             int64_t mtime, int is_dir);
int utar_list(const char *path, utar_entry_fn fn, void *ctx, char *err, int errcap);

// Writes member `name` of `path` to `out_path`, through `<out_path>.part`
// and a rename. 0, or -ENOENT no such member, -EINVAL a damaged archive,
// -EINTR stopped by `progress`, -ENOMEM, -EIO.
int utar_extract(const char *path, const char *name, const char *out_path,
                 utar_progress progress, void *ctx, char *err, int errcap);

// MANY MEMBERS IN ONE PASS -- a tar has no index, so taking members out
// one utar_extract() at a time walks (and inflates) the whole archive
// for each. `route` sees every member in order and returns 1 with
// `out` filled to write it there, 0 to pass it by (a folder's record
// included: the caller makes the folder). Each file written goes through
// `<out>.part` and is checked against its size. Stops at the first
// failure. `progress` sees the bytes written of all the members taken.
typedef int (*utar_route)(void *ctx, const char *name, unsigned long long size, int is_dir,
                          char *out, int cap);
int utar_extract_many(const char *path, utar_route route, void *ctx,
                      utar_progress progress, void *pctx, char *err, int errcap);

// --- writing ----------------------------------------------------------
//
// Begin, add files and folders, end -- into `<out_path>.part`, renamed
// into place at the end. A file is STREAMED in, 64 KiB at a time, so a
// big one costs no more memory than a small one; gzipped as it goes when
// asked (lib/uinflate.h's udeflate_gz_piece()), at `level` (a
// udeflate_level, or 0 for UDEFLATE_NORMAL).
struct utar_writer {
    int fd, gz;
    char part[512], out[512];
    unsigned char *buf, *zbuf;
    unsigned long fill, zcap;
    struct udeflate_gz gzs;
};
int utar_create_begin(struct utar_writer *w, const char *out_path, int gzip, int level,
                      char *err, int errcap);
int utar_add_file(struct utar_writer *w, const char *name, const char *src_path,
                  utar_progress progress, void *ctx, char *err, int errcap);
int utar_add_dir(struct utar_writer *w, const char *name, char *err, int errcap);
int utar_create_end(struct utar_writer *w, char *err, int errcap);
void utar_create_abort(struct utar_writer *w);

#endif
