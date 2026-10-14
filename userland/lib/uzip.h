#ifndef ULIB_UZIP_H
#define ULIB_UZIP_H

#include <stddef.h>

// Reading ONE member out of a .zip archive into a file or memory, and
// writing a new archive -- PKWARE's
// APPNOTE, the subset every archiver writes: stored (method 0) and
// deflated (method 8) members, no encryption, no zip64.
//
// **THE CENTRAL DIRECTORY IS THE INDEX, NEVER A SCAN OF LOCAL HEADERS.**
// It is at the end of the file, it is what unzip(1) and every OS shell
// read, and a member's local header can carry sizes of 0 (bit 3, a
// streamed archive) that only the central record has right.
//
// THE MEMBER'S COMPRESSED BYTES ARE HELD IN MEMORY, its OUTPUT is
// streamed to the file: lib/uinflate.h consumes one contiguous input.
// So a 10 MB member costs 10 MB of heap, not the 28 MB it inflates to.
//
// A PARSER HERE REJECTS rather than guesses: every offset is checked
// against the file's size, an entry that does not fit its record is an
// error, and the output must match the CRC-32 and the size recorded for
// it -- or nothing is left at `out_path`.

// Called as the member is written: bytes out so far, and the size it
// will have. Return non-zero to stop, which the extract reports as
// cancelled and cleans up after. May be NULL.
typedef int (*uzip_progress)(void *ctx, unsigned long done, unsigned long total);

// Writes member `name` (its full path in the archive, '/'-separated) of
// `zip_path` to `out_path`, through `<out_path>.part` and a rename, so a
// failed or cancelled extract never leaves a partial file under the
// real name. 0 on success, else a negative errno with `err` a sentence:
// -ENOENT no such member, -ENOTSUP a method/encryption/zip64 this does
// not read, -EINVAL a damaged archive or a CRC/size mismatch,
// -EINTR stopped by `progress`.
int uzip_extract(const char *zip_path, const char *name, const char *out_path,
                 uzip_progress progress, void *ctx, char *err, int errcap);
// The same member into MEMORY: *out (malloc'd, the caller frees it) and
// its length. Refused with -E2BIG when it would be bigger than `max`.
int uzip_read(const char *zip_path, const char *name, size_t max, unsigned char **out, size_t *out_len,
              char *err, int errcap);

// Every member of the archive, in the central directory's order: its
// full name ('/'-separated; a folder's ends in '/'), its size, its size
// in the archive, and its MS-DOS date and time as stored. Return non-zero
// to stop. A name of UZIP_NAME_MAX bytes or more is skipped. 0, or a
// negative errno as uzip_extract() gives.
#define UZIP_NAME_MAX 256
typedef int (*uzip_entry_fn)(void *ctx, const char *name, unsigned long size,
                             unsigned long packed, unsigned dos_date, unsigned dos_time);
int uzip_list(const char *zip_path, uzip_entry_fn fn, void *ctx, char *err, int errcap);

// --- writing ----------------------------------------------------------
//
// A NEW ARCHIVE, a member at a time: begin, add files and folders, end.
// Written to `<out_path>.part` and renamed into place at the end, so a
// failed or cancelled compress never leaves a half archive under the
// real name. Each file is read whole and stored, or deflated when that
// is smaller (`level` 0 always stores); no zip64, so a member must be
// under 4 GiB -- refused, not truncated. The central directory is kept
// in memory until uzip_create_end() writes it.
struct uzip_wentry;
struct uzip_writer {
    int fd;
    char part[512], out[512];
    struct uzip_wentry *ent;
    int count, cap;
    unsigned long offset;
};
int uzip_create_begin(struct uzip_writer *w, const char *out_path, char *err, int errcap);
// `name` is the member's path in the archive, '/'-separated, no leading
// '/'. `progress` reports this member's bytes (may be NULL).
int uzip_add_file(struct uzip_writer *w, const char *name, const char *src_path, int level,
                  uzip_progress progress, void *ctx, char *err, int errcap);
// A member from bytes in memory, dated now -- written in the order added,
// so a format whose first member is fixed (OpenRaster's "mimetype",
// stored: level 0) gets it first.
int uzip_add_mem(struct uzip_writer *w, const char *name, const void *data, size_t n, int level,
                 char *err, int errcap);
// A folder record, "name/" -- so an empty folder survives the round trip.
int uzip_add_dir(struct uzip_writer *w, const char *name, char *err, int errcap);
int uzip_create_end(struct uzip_writer *w, char *err, int errcap);
// Gives up: closes and removes the `.part`, frees the directory.
void uzip_create_abort(struct uzip_writer *w);

#endif
