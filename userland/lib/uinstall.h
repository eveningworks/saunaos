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
// `grub-mkimage` time, and toy-os has no `grub-mkimage`, so the plan
// reproduces the layout the staged image expects (docs/commands/install.md,
// "The trap"):
//
//   GPT  p1 1 MiB BIOS boot (core.img) | p2 ESP, FAT32 | p3 TFS3 root
//   MBR  gap (core.img)                | p1 FAT32 [active] | p2 TFS3 root
//
// NOTHING HERE PRINTS: what a stage is doing arrives through
// `uinstall_ops`, and how far it has got is a `uinstall_progress` a UI
// thread may read while the run goes on in another.

#include <stdint.h>
#include "lib/udiskscan.h"

#define UINSTALL_PAYLOAD     "/install"   // what every toy-os root carries to install itself
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

// What the copy will carry: every file under / but the mount points the
// run recreates empty (/boot, /mnt, /tmp). 0, or -1 when / cannot be read.
int uinstall_measure(uint32_t *files, uint64_t *bytes);

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

// Run every stage onto `p`'s disk, which is ERASED. 0 when the disk was
// written and should boot; -1 otherwise, `prog->error` saying why. Blocks
// for as long as the copy takes (~30 s on a laptop SSD).
int uinstall_run(const struct uinstall_plan *p, unsigned flags, struct uinstall_progress *prog,
                 const struct uinstall_ops *ops);

// The boot sector and core image alone, onto `disk` -- the last stage,
// and what `install --bootloader` refreshes. `in_use` is the confirm the
// kernel wants before writing to a disk something is mounted from. NULL,
// or why it failed.
const char *uinstall_write_bootloader(const char *disk, int mbr, int in_use);

#endif
