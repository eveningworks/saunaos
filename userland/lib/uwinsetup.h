#ifndef ULIB_UWINSETUP_H
#define ULIB_UWINSETUP_H

#include <stdint.h>
#include <stddef.h>

// Windows 3.x installed from its own disks into w16's drive C: -- what its
// Setup did in Express mode on a VGA PC with a PS/2 mouse, file for file:
// SETUP.INF says which disk holds each file and whether it goes to
// WINDOWS or WINDOWS\SYSTEM, umsexpand undoes COMPRESS.EXE, and
// WIN.INI, SYSTEM.INI and CONTROL.INI are made from their .SRC templates.
// w16 then finds what Windows' own programs look for where they look.
//
// THE DISKS ARE CONTAINERS, NOT NUMBERS: a folder holding every disk's
// files, a folder per disk, floppy images (lib/ufatimg) or any mix. Each
// file is found by its name wherever it is, so the INF's disk number is
// only what a message names ("on disk 6").
//
// WHAT "WHERE SETUP PUT IT" MEANS is measured, not read off the INF's
// comments: Express Setup's own C:\WINDOWS on the reference VM (Windows
// for Workgroups 3.11, English) is the answer every rule here reproduces
// -- `tools/uwinsetup_hostcheck.py` holds the plan to it. The files Setup
// MADE rather than copied (WIN.COM, the Program Manager groups, REG.DAT,
// the swap file) are not made here: w16 needs none of them.
//
// Plain POSIX -- dirent, stdio, rename -- so the host check compiles it as
// it is. The disks are untrusted: every parser under it rejects rather
// than guesses.

enum { UWS_WINDOWS, UWS_SYSTEM };
enum { UWS_FILE, UWS_TEMPLATE, UWS_OPTIONAL };   // a template is an .INI made from a .SRC

struct uws_item {
    char name[13];        // installed as, "MAIN.CPL"
    char want[13];        // as SETUP.INF names it ("WIN.SRC" for WIN.INI)
    char disk_name[13];   // as the disk has it, "MAIN.CP_"; "" when no disk has it
    int disk;             // SETUP.INF's disk number, for messages
    uint8_t dest;         // UWS_WINDOWS or UWS_SYSTEM
    uint8_t kind;
    uint32_t size;        // expanded, when the file says; else as on the disk
};

struct uwinsetup;

// Reads the disks at `src`: a folder (its files, each folder in it, each
// .img in it) or one image (with every .img beside it), finds SETUP.INF and
// plans the install. NULL with `err` a sentence when there is no SETUP.INF
// or it is not one this reads.
struct uwinsetup *uwinsetup_open(const char *src, char *err, size_t errcap);
void uwinsetup_close(struct uwinsetup *s);

const char *uwinsetup_title(const struct uwinsetup *s);   // "Microsoft Windows for Workgroups 3.11"
const char *uwinsetup_lang(const struct uwinsetup *s);    // SETUP.INF's deflang: "enu", "fin"...
const char *uwinsetup_windir(const struct uwinsetup *s);  // "WINDOWS", from defdir
int uwinsetup_containers(const struct uwinsetup *s);      // folders and images holding files
int uwinsetup_count(const struct uwinsetup *s);
const struct uws_item *uwinsetup_item(const struct uwinsetup *s, int i);
int uwinsetup_missing(const struct uwinsetup *s);         // planned files no disk has (Setup's own aside)
uint64_t uwinsetup_bytes(const struct uwinsetup *s);

enum { UWS_KEEP_INI = 1 };   // an .INI already on drive C: is left as it is

// Called before each file with what is about to happen to it: "copy",
// "kept" (an INI left alone), "absent" (Setup's own, not on these disks).
// Non-zero stops the install, which then answers -EINTR.
typedef int (*uws_progress)(void *ctx, int i, int n, const struct uws_item *it, const char *what);

// Installs into `drive_c` (WINDOWS and WINDOWS\SYSTEM under it). Each file
// is written as NAME.part and renamed, so a stopped install leaves no half
// file under a real name. 0, or a negative errno with `err` naming the file.
int uwinsetup_run(struct uwinsetup *s, const char *drive_c, int flags, uws_progress progress, void *ctx,
                  char *err, size_t errcap);

#endif
