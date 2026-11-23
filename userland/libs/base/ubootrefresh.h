#ifndef ULIB_UBOOTREFRESH_H
#define ULIB_UBOOTREFRESH_H

// THE BOOT LOADER, REWRITTEN IN PLACE -- `grub-install` on a machine that
// is already installed -- and whether it is worth doing: what `install
// --bootloader` runs and what System Update offers.
//
// An update replaces every file and the kernel but never GRUB's core
// image, which lives in raw sectors. What that core carries is known only
// from the RECORD written beside it (UBOOTREFRESH_RECORD): its module list
// and GRUB's version, staged by tools/build/install_grub.py. The machine's
// record against the staged one (/install/core.modules) says whether a
// newer boot loader is waiting -- any difference counts, a GRUB upgrade
// on the build host as well as a module added -- and a machine with no
// record at all was installed before records existed. Never written
// silently: it is the one write that decides whether the machine boots.
//
// THE RUNNING DISK IS THE ONLY TARGET, which is what makes its table
// format knowable (QUERY_PARTTABLE), and the prefix baked into each staged
// core image (`(hd0,gpt2)` against `(hd0,msdos1)`) must match it or GRUB
// comes up at a rescue prompt. Another disk is the full installer's job.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Beside the core it describes, on the boot volume: whatever writes GRUB
// writes this (install_grub.py on the host, the installer, a refresh), and
// an update never writes /boot/grub, so it cannot drift from the sectors.
#define UBOOTREFRESH_RECORD     "/boot/grub/core.modules"
#define UBOOTREFRESH_RECORD_OLD "/etc/grub-core.modules"   // where `install --bootloader` kept it first
#define UBOOTREFRESH_STAGED     "/install/core.modules"

// This disk's record, NUL-terminated, from UBOOTREFRESH_RECORD (or the
// legacy layout's /boot/boot/grub/core.modules), else the old /etc one.
// NULL when there is none. The caller frees it.
char *ubootrefresh_record(void);

// 1 when `record` names GRUB module `module` as a whole word (the
// module line only), 0 otherwise or for a NULL record.
int ubootrefresh_has(const char *record, const char *module);

// What a refresh of this machine would write. Filled by
// ubootrefresh_plan(); the caller only reads it.
struct ubootrefresh_plan {
    char disk[16];      // the root's disk, as lsblk names it
    int mbr;            // the table: 1 MBR, 0 GPT
    int legacy;         // /boot/boot is still there and is moved up first
    int cfg_missing;    // grub.cfg entries that gain bootpart=; -1 unreadable
    int has_efi;        // the payload carries a UEFI loader
    const char *core;   // the staged core image it writes
};

// Can this machine refresh its boot loader? NULL with `p` filled, or a
// sentence saying why not: a live session (no disk-backed root), a disk
// with no partition table, a build that staged no install payload, a
// payload replaced only at the next restart.
const char *ubootrefresh_plan(struct ubootrefresh_plan *p);

// Rewrite it: the move up from /boot/boot when `legacy`, the boot sector
// and core image (read back before the boot sector is written), the UEFI
// loader, the record, then bootpart= in grub.cfg. Each step and each
// problem is handed to `say` as a sentence. 0 done (a later step's problem
// said but not fatal); -1 when the boot loader was NOT written, and the
// disk still boots the one it had.
int ubootrefresh_run(const struct ubootrefresh_plan *p,
                     void (*say)(void *ctx, const char *line), void *ctx);

// WHETHER TO OFFER IT. `adds` is the staged core's modules this disk's
// record lacks, space-separated ("" for none, NULL with no record); a
// version is NULL when its record names none.
//
// NEWER: modules added and none dropped, and GRUB's version not older.
// DIFFERENT: any other difference -- a module dropped, an older GRUB (a
// channel gone back) -- still offered, worded as a difference.
// UNKNOWN: this disk has no record. `pending`: the update waiting for a
// restart carries the payload compared; ubootrefresh_plan() refuses until
// that restart, which would otherwise write the OLD core.
enum { UBOOTREFRESH_SAME, UBOOTREFRESH_NEWER, UBOOTREFRESH_DIFFERENT, UBOOTREFRESH_UNKNOWN };
struct ubootrefresh_offer {
    int kind;               // UBOOTREFRESH_*
    int pending;            // compared against the payload staged for the next boot
    char *adds;             // allocated, as are both versions; ubootrefresh_offer_free()
    char *version;          // the staged GRUB's
    char *installed;        // this disk's
};

// SAME also when there is nothing to offer from: no staged record, or a
// machine with no disk to refresh. 0, or -1 out of memory (kind SAME).
int ubootrefresh_offer(struct ubootrefresh_offer *o);
void ubootrefresh_offer_free(struct ubootrefresh_offer *o);

// What differs, as a phrase for a sentence: "adds videoinfo", "GRUB 2.12
// -> 2.16", "adds probe, GRUB 2.12 -> 2.16", "adds 11 GRUB modules" (past
// three), "GRUB 2.16" (a record naming no version). "" for SAME and
// UNKNOWN. Fits or writes nothing.
void ubootrefresh_offer_detail(const struct ubootrefresh_offer *o, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
