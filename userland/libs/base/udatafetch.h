#ifndef ULIB_UDATAFETCH_H
#define ULIB_UDATAFETCH_H

// GAME DATA FETCHED ON THE MACHINE -- a program's table of the data it
// can download (DOOM's IWADs, ScummVM's freeware games), each pinned by
// SHA-256, unpacked into place and its licence saved beside the others:
// Debian's game-data-packager shape. The card that offers them is
// ui/uui_datacard.h.
//
// **THE CHECKSUM IS THE AUTHORITY, NOT THE TRANSPORT**: a fetch skips
// certificate checks (a machine has no CA bundle by default) and a
// `mirror=` stands in for the origin, because a substituted file fails
// the hash and leaves nothing behind.
//
// **THE TRANSFER IS THE CALLER'S** (`download`, normally uhttp_download):
// libhttp is not one of the toolkit's libraries, so this one takes it as
// a pointer rather than linking it.

#include "uhttp.h"

#ifdef __cplusplus
extern "C" {
#endif

// How a downloaded part becomes the item.
enum udata_unpack {
    UDATA_FILE,     // the download IS the item's file
    UDATA_MEMBER,   // a .zip; its one member `member` is the item's file
    UDATA_TREE,     // a .zip; the folder `member` in it ("lure/", or "" for
                    // all of it) becomes the item's FOLDER
};

struct udata_part {
    const char *url;         // the origin
    const char *sha256;      // 64 hex digits; required
    unsigned long size;      // the download's size, for the card
    enum udata_unpack how;
    const char *member;
};

// One thing a program can fetch. `id` is its name in the program's data
// directory -- a file for UDATA_FILE/MEMBER, a folder for UDATA_TREE,
// whose parts all unpack into the one folder (a game, then its music).
// `part_count` 0: never downloaded, only found (a retail file).
struct udata_item {
    const char *id;
    const char *name;        // on screen
    const char *detail;      // one line about it
    const char *source;      // the host the card names
    const struct udata_part *parts;
    int part_count;
    // THE LICENCE, shown before a download: its name, the notice in plain
    // words ('\n' between paragraphs), and whether the person must AGREE
    // or is only told. Saved as /usr/share/licenses/<licence_file> --
    // from <licence_member> of the first part's zip when there is one
    // (the real text), else the notice itself.
    const char *licence;
    const char *notice;
    int agree;
    const char *licence_file;
    const char *licence_member;
};

// Where the item lives: `dir`/`id`. Allocated; free() it.
char *udata_path(const char *dir, const struct udata_item *it);
int udata_present(const char *dir, const struct udata_item *it);
// The bytes a fetch downloads, summed over the parts.
unsigned long udata_size(const struct udata_item *it);
// The URL a part is fetched from: `mirror` (no trailing slash) replaces
// all but the file's name, so a LAN copy or a test's server stands in.
// Allocated; free() it. NULL when the part has no URL.
char *udata_url(const struct udata_part *p, const char *mirror);
// The host a part's URL (mirror applied) names, for the card.
void udata_host(const struct udata_part *p, const char *mirror, char *out, int cap);

// "ASKED ONCE PER LICENCE": the CRC of the notice agreed to is kept in
// `conf` as licence_<id up to its first '.'>, so a changed text asks again.
int udata_licence_agreed(const char *conf, const struct udata_item *it);
void udata_licence_agree(const char *conf, const struct udata_item *it);

#define UDATA_ERR_MAX 192

// One fetch, run on a worker thread: every part downloaded to /var/tmp,
// checked, unpacked into `dir`/`id`.part and renamed into place once all
// of them are, then the licence saved. Nothing is left on failure.
struct udata_fetch {
    const char *dir;
    const char *mirror;      // or NULL
    int (*download)(struct uhttp_download *d);
    // Phase 0 downloading, 1 unpacking; `part` counts from 0. Called from
    // the fetching thread.
    void (*progress)(void *ctx, int phase, int part, unsigned long got, unsigned long total);
    volatile int *cancel;    // non-zero stops it
    void *ctx;
    char err[UDATA_ERR_MAX]; // out: a sentence
};
// 0, or -1 with `err` filled.
int udata_fetch_run(struct udata_fetch *f, const struct udata_item *it);

#ifdef __cplusplus
}
#endif

#endif
