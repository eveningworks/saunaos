#ifndef ULIB_UINSTALL_H
#define ULIB_UINSTALL_H

// Putting this running system onto another disk and making that disk
// boot: the five stages `install` and the installer window both run --
// partition (SYS_MKPART), format (SYS_MKFS), mount (SYS_MOUNT), copy
// (ufileop), write the bootloader (SYS_INSTALL_BOOT). Calamares' and
// Ubiquity's job, as a library both front ends call, so the CLI and the
// GUI cannot install two different ways.
//
// THE LAYOUT IS A CONSTRAINT, NOT A PREFERENCE: `core.img` has its prefix
// -- table format and partition number -- baked in at the host's
// `grub-mkimage` time, and SaunaOS has no `grub-mkimage`, so the plan
// reproduces the layout the staged image expects (docs/commands/install.md,
// "The trap"):
//
//   GPT  p1 1 MiB BIOS boot (core.img) | p2 ESP, FAT32 | p3 TFS3 root
//   MBR  gap (core.img)                | p1 FAT32 [active] | p2 TFS3 root
//
// NOTHING HERE PRINTS: what a stage is doing arrives through
// `uinstall_ops`, and how far it has got is a `uinstall_progress` a UI
// thread may read while the run goes on in another.

#include <stddef.h>
#include <stdint.h>
#include "base/ucomponent.h"
#include "base/udiskscan.h"

#define UINSTALL_PAYLOAD     "/install"   // what every SaunaOS root carries to install itself
#define UINSTALL_TARGET      "/mnt"       // where the target's root is mounted
#define UINSTALL_TARGET_BOOT "/mnt/boot"
#define UINSTALL_ESP_DEFAULT 64           // MiB
#define UINSTALL_ESP_MIN     8
// The live session's desktop launcher (written by `installer --boot`),
// by name in the Desktop folder -- left behind by every install.
#define UINSTALL_LAUNCHER    "installer.desktop"

enum { UINSTALL_PARTITION, UINSTALL_FORMAT, UINSTALL_MOUNT, UINSTALL_COPY,
       UINSTALL_BOOTLOADER, UINSTALL_STAGES };

// "Partition", "Format", "Mount", "Copy files", "Bootloader".
const char *uinstall_stage_name(int stage);

// The installer=on / installer=off boot word: 1 on, 2 off, 0 absent.
int uinstall_boot_word(void);

// --- the plan ---------------------------------------------------------

struct uinstall_part {
    uint64_t start, sectors;      // 512-byte sectors
    int role;                     // MKPART_ROLE_*
    char dev[24];                 // "ata0p2" -- what it will be called
    const char *fs;               // "fat32", "tfs3", or NULL (BIOS boot)
    const char *holds;            // "GRUB's core image", "/boot", "the root"
};

struct uinstall_plan {
    char disk[16];
    uint64_t sectors;
    uint64_t esp_mib;
    int mbr;
    int nparts;
    struct uinstall_part part[3];
    int boot, root;               // indexes into `part`
};

// --- what goes on the disk: the optional components ---------------------
//
// THE INSTALL CHOOSES COMPONENTS, NOT FILES -- Calamares' netinstall,
// Anaconda's Software Selection, Debian's tasksel: each optional
// component of this build (base/ucomponent.h) is copied whole or left out
// whole. A left-out one's files -- its Paths=, by the longest-path rule,
// so /tests/nsmonkey stays with the Web page engine -- are not copied,
// and the TARGET's state file says `removed <id>`, so the new machine's
// updates keep it out until `component add` (docs/software-design.md).
//
// `set`'s `removed` mark MEANS "LEFT OUT OF THIS INSTALL" here, so
// ucomponent's closures keep Needs= whole both ways: leaving the engine
// out leaves the Browser out, putting the Browser in puts the engine in.
// A component removed on THIS system is `absent`: there is nothing to
// copy, so it starts out and cannot be put in.
struct uinstall_software {
    struct ucomponent_set set;   // this build's definitions; removed = left out
    unsigned char *absent;       // per component: removed here, not on this disk
    uint32_t *files;             // per component, measured by uinstall_measure()
    uint64_t *bytes;
    uint32_t base_files;         // ...and the base system's, in no component
    uint64_t base_bytes;
};

// This system's components and its state: every one here in, every
// absent one out. 0, or -1 out of memory (an empty set). MUST START
// ZEROED; free with uinstall_software_free().
int  uinstall_software_load(struct uinstall_software *sw);
// The same from a directory of definitions (a test's own).
int  uinstall_software_load_dir(struct uinstall_software *sw, const char *dir);
void uinstall_software_free(struct uinstall_software *sw);
int  uinstall_software_in(const struct uinstall_software *sw, int idx);
int  uinstall_software_count_in(const struct uinstall_software *sw);
int  uinstall_software_count_here(const struct uinstall_software *sw);   // not absent
// Put component `idx` in (1) or leave it out (0), with what Needs= takes
// along. How many changed, or -1 -- nothing changed -- when putting it in
// would need an absent one (or is one).
int  uinstall_software_choose(struct uinstall_software *sw, int idx, int in);
// Every component that is here in (Select all), or every one out (The
// system only).
void uinstall_software_all(struct uinstall_software *sw, int in);
// Does the copy leave `path` out? A file whose owner is out; a directory
// whose owner is out and under which no component going in claims a
// longer path -- /tests stays, holding nsmonkey, when only Demos is out.
int  uinstall_software_skips(const struct uinstall_software *sw, const char *path, int is_dir);
// What the copy carries, from the last uinstall_measure(): files and
// bytes going, and the bytes of what is here but left out. Any may be NULL.
void uinstall_software_totals(const struct uinstall_software *sw, uint32_t *files, uint64_t *bytes,
                              uint64_t *left_out);
// The choice as a sentence: "4 of 5 components -- Demos and tests left
// out", "all 5 components", "the system only -- every component left out".
void uinstall_software_describe(const struct uinstall_software *sw, char *out, size_t cap);

// What the copy will carry: every file under / but the mount points the
// run recreates empty (/boot, /mnt, /tmp) -- and, with `sw`, but what it
// leaves out, whose per-component sums it records for
// uinstall_software_totals(). 0, or -1 when / cannot be read.
int uinstall_measure(struct uinstall_software *sw, uint32_t *files, uint64_t *bytes);

// The smallest root that holds `bytes` of system: a quarter again for
// TFS3's own structures, as `make live-iso` sizes its image.
uint64_t uinstall_root_need(uint64_t bytes);

// Why `d` cannot take an install, as a sentence, or NULL when it can: the
// disk the system runs from, a live image, sectors that are not 512
// bytes, or too small for the layout and `need` bytes of root (0 to
// check the layout only).
const char *uinstall_refusal(const struct udisk *d, uint64_t esp_mib, int mbr, uint64_t need);

// Lay `disk` out. NULL, or why not (uinstall_refusal()'s sentences, and
// "no such disk").
const char *uinstall_plan(struct uinstall_plan *p, struct udisk_scan *s, const char *disk,
                          uint64_t esp_mib, int mbr, uint64_t need);

// Can THIS system install itself with that table? NULL, or why not (a
// build staged without GRUB's BIOS target carries no payload). Said
// before anything is erased.
const char *uinstall_payload_problem(int mbr);

// The disk the root is on, by name: 1, or 0 when the root has none.
int uinstall_root_disk(char *out, int cap);

// --- the run ------------------------------------------------------------

// HOW FAR IT HAS GOT, written by the run and read by anyone: a UI thread
// polls it from its tick. Each field is a plain word written whole; the
// strings may be caught mid-write and are only ever shown.
struct uinstall_progress {
    volatile int stage;                // UINSTALL_*; UINSTALL_STAGES once done
    volatile int finished;             // 1 when the run has returned
    volatile int ok;                   // ...and whether it succeeded
    volatile uint32_t files_done, files_total;
    volatile uint64_t bytes_done, bytes_total;
    volatile int errors;
    char path[96];                     // the file being copied
    char error[128];                   // the last failure, as a sentence
};

struct uinstall_ops {
    void *ctx;
    // A stage begins: the CLI prints "[2/5] Format", a window moves its bar.
    void (*on_stage)(void *ctx, int stage, const char *detail);
    // A line about the stage -- each top directory copied, each failure.
    void (*on_line)(void *ctx, const char *line);
};

#define UINSTALL_ASK_ALL 0x1   // the target's First-boot setup asks every page again

// Run every stage onto `p`'s disk, which is ERASED, copying what `sw`
// puts in (NULL: everything here). 0 when the disk was written and should
// boot; -1 otherwise, `prog->error` saying why -- including a target whose
// state file could not record what was left out, whose first update
// would fetch it back. Blocks for as long as the copy takes (~30 s on a
// laptop SSD).
int uinstall_run(const struct uinstall_plan *p, unsigned flags, struct uinstall_software *sw,
                 struct uinstall_progress *prog, const struct uinstall_ops *ops);

// The boot sector and core image alone, onto `disk` -- the last stage,
// and what `install --bootloader` refreshes. `in_use` is the confirm the
// kernel wants before writing to a disk something is mounted from. NULL,
// or why it failed.
const char *uinstall_write_bootloader(const char *disk, int mbr, int in_use);

// GRUB's UEFI loader from the payload to `boot`/EFI/BOOT/BOOTX64.EFI --
// the firmware's removable-media path, so no NVRAM entry is needed, on a
// GPT disk or an MBR one alike. `boot` is the mounted boot volume and must
// be writable. 0 written; 1 when the payload carries none (a build host
// without GRUB's EFI target), and the disk boots BIOS/CSM only; -1 with
// *why on a failure.
int uinstall_write_efi(const char *boot, const char **why);

#endif
