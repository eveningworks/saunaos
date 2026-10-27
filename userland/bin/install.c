// install -- put this running system onto another disk, and make it boot.
//
// The five stages are lib/uinstall.h's, which the installer window runs
// too: partition, format, mount, copy, write the bootloader -- each an
// existing command's operation done through the same syscall it uses,
// so every step can be checked by hand.
//
// BARE AT A TERMINAL IT ASKS: the disk, the table, then the disk's name
// typed back before anything is erased -- Alpine's setup-alpine shape.
// Flags and `confirm` skip every question, and a program that is not a
// terminal is never asked anything, so a script or a test drives it
// exactly as before.
//
// WHAT IT REFUSES: the disk this machine is running from. An installer
// handed the running root is an installer being asked to saw off its own
// branch, and there is no reading of "reinstall over myself" that ends
// with a working machine.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "lib/uargs.h"
#include "lib/human.h"
#include "lib/ufile.h"
#include "lib/ubootcfg.h" // --bootloader also brings grub.cfg up to bootpart=
#include "lib/ubootlayout.h"
#include "lib/udiskscan.h"
#include "lib/uduration.h"
#include "lib/uinstall.h"
#include "lib/uprogress.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *f_disk, *f_esp;
static int f_mbr, f_ask_all, f_bootloader, f_migrate;

static const struct uargs_opt OPTS[] = {
    { "disk", 0, "NAME", "the target, as `lsblk` names it; refuses the one this machine runs from", 0, &f_disk },
    { "esp", 0, "MIB", "size of the FAT32 /boot partition (default 64)", 0, &f_esp },
    { "mbr", 0, 0, "an MBR table instead of GPT, for firmware that will not boot a GPT disk", &f_mbr, 0 },
    { "ask-all", 0, 0, "the new machine's First-boot setup asks every page, not just the unanswered", &f_ask_all, 0 },
    { "bootloader", 0, 0, "rewrite THIS machine's bootloader in place; no partitioning", &f_bootloader, 0 },
    { "migrate", 0, 0, "the same, only when /boot/boot is still there (the boot-migrate service)", &f_migrate, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "install",
    .usage = "[--disk NAME] [--esp MIB] [--mbr] [--ask-all] [confirm]\n"
             "--bootloader [confirm]\n"
             "--migrate [confirm]",
    .summary = "Put this running system onto another disk, and make that disk boot.\n"
               "With no --disk at a terminal it asks which; `confirm` ERASES the target.",
    .opts = OPTS,
    .notes = "The installer window (Install SaunaOS, on a live session's desktop) runs the same steps.",
};

#define STAMP_PATH "/etc/grub-core.modules"

static void step(const char *what) { printf("install: %s\n", what); }
static void refuse(const char *why) { printf("install: %s\n", why); }

// THE BOOTLOADER, REWRITTEN IN PLACE -- `grub-install` on a machine
// that is already installed, plus the bootpart= lines grub.cfg needs
// from it. No table is written, no partition is formatted and no other
// file is touched: the 185 KiB core image goes into the embed area it
// already occupies and the boot sector is re-patched to point at it.
//
// IT EXISTS BECAUSE AN INSTALLED MACHINE COULD NOT OTHERWISE GAIN A
// BOOTLOADER IT DOES NOT ALREADY HAVE. A laptop installed before
// `gzio` joined CORE_MODULES read a gzipped kernel as raw bytes -- `no
// multiboot header found`, and the default menu entry was dead until
// the rescue entry was picked by hand. The full installer cannot help:
// it ERASES the disk and refuses the one the machine runs from.
//
// THE RUNNING DISK IS THE ONLY TARGET, and that is what makes the table
// format knowable: QUERY_PARTTABLE answers for this machine's disk, and
// the prefix baked into each staged core image (`(hd0,gpt2)` against
// `(hd0,msdos1)`) has to match it or GRUB comes up at a rescue prompt
// having found no config. Another disk is the full installer's job.
// grub.cfg's half of a refresh: every SaunaOS entry gains the bootpart=
// lines the payload's grub.cfg has, so the kernel roots on the disk GRUB
// booted from (docs/boot-flags.md). EDITED, never replaced -- the file
// is the machine's own, bootcfg and the Boot Manager write it, and
// grubby's rule is that a tool adds what it owns and leaves the rest.
// After the core image, because only a core with `probe` can answer it;
// either half without the other still boots. Returns entries changed,
// or -1.
static struct ubootcfg g_cfg, g_cfg_before;

static int refresh_grub_cfg(int confirmed) {
    if (ubootcfg_load(&g_cfg, UBOOTMENU_CFG) < 0) {
        printf("install: could not read %s -- left as it is.\n", UBOOTMENU_CFG);
        return -1;
    }
    int n = ubootcfg_bootpart_missing(&g_cfg);
    if (!confirmed || !n) return n;
    g_cfg_before = g_cfg;
    if (ubootcfg_add_bootpart(&g_cfg) < 0) {
        printf("install: grub.cfg not changed: %s\n", g_cfg.why);
        return -1;
    }
    static struct ubootcfg_problem probs[8];
    int np = ubootcfg_check(&g_cfg, &g_cfg_before, UBOOTCFG_CHECK_FILES, probs, 8);
    if (ubootcfg_has(probs, np, UBOOTCFG_BROKEN)) {
        printf("install: grub.cfg not changed -- the edit would not parse: %s\n", probs[0].msg);
        return -1;
    }
    const char *why = 0;
    if (ubootcfg_save(&g_cfg, UBOOTMENU_CFG, &why) < 0) {
        printf("install: grub.cfg not changed: %s\n", why ? why : "the save failed");
        return -1;
    }
    return n;
}

static int refresh_bootloader(int confirmed) {
    char disk[16] = "";
    if (!uinstall_root_disk(disk, sizeof disk)) {
        refuse("this machine has no disk-backed root -- there is no "
               "bootloader here to rewrite.");
        return 1;
    }

    struct query_parttable t;
    if (sys_query_record(QUERY_PARTTABLE, 0, &t, sizeof t) < (int)sizeof t ||
        t.kind == QUERY_PART_NONE) {
        refuse("this disk has no partition table -- refusing to guess "
               "where the core image goes.");
        return 1;
    }
    int mbr = (t.kind == QUERY_PART_MBR);

    const char *missing_payload = uinstall_payload_problem(mbr);
    if (missing_payload) { printf("install: %s.\n", missing_payload); return 1; }
    int legacy = ubootlayout_legacy();

    if (!confirmed) {
        int missing = legacy ? -2 : refresh_grub_cfg(0);
        printf("install: this rewrites %s's bootloader in place (%s layout):\n",
               disk, mbr ? "MBR" : "GPT");
        if (legacy)
            printf("  /boot/boot     copied up to /boot first, removed once the new GRUB "
                   "is in place\n");
        printf("  boot sector    LBA 0, keeping this disk's partition table\n");
        printf("  core image     %s\n",
               mbr ? UINSTALL_PAYLOAD "/core-msdos.img" : UINSTALL_PAYLOAD "/core.img");
        if (missing > 0)
            printf("  grub.cfg       bootpart= added to %d entr%s (a .bak is kept)\n",
                   missing, missing == 1 ? "y" : "ies");
        else if (missing == 0)
            printf("  grub.cfg       already passes bootpart= -- unchanged\n");
        printf("install: partitions, filesystems and other files are untouched. "
               "The core image is\n");
        printf("install: read back before the boot sector is written, so a "
               "bad write leaves the\n");
        printf("install: bootloader you have now. Re-run with `confirm` if "
               "that is what you want.\n");
        return 1;
    }

    // THE MOVE UP FROM /boot/boot, around the write: the copy is made
    // while the old core still boots the old tree, and the old tree goes
    // only once a core reading /boot/grub is in place (lib/ubootlayout.h).
    char dev[64] = "";
    const char *why = 0;
    if (legacy) {
        step("copying /boot/boot up to /boot");
        if (uboot_writable(dev, sizeof dev) < 0) {
            refuse("cannot remount /boot read-write -- nothing was changed.");
            return 1;
        }
        int ok = ubootlayout_copy_up(&why) == 0;
        if (!ok) ubootlayout_drop_flat();
        uboot_restore(dev);     // read-only again, which flushes the copy
        if (!ok) {
            printf("install: %s -- nothing was changed.\n", why);
            return 1;
        }
    }

    step("writing the bootloader");
    const char *failed = uinstall_write_bootloader(disk, mbr, 1);
    if (failed) {
        printf("install: %s\n", failed);
        if (legacy && uboot_writable(dev, sizeof dev) == 0) {
            ubootlayout_drop_flat();
            uboot_restore(dev);
        }
        printf("install: FAILED -- %s still has the bootloader it had.\n", disk);
        return 1;
    }
    if (legacy) {
        step("removing /boot/boot");
        int ok = uboot_writable(dev, sizeof dev) == 0 && ubootlayout_drop_legacy(&why) == 0;
        uboot_restore(dev);
        if (!ok)
            printf("install: %s -- the machine boots from /boot already; run "
                   "`install --migrate confirm` again to tidy it.\n",
                   why ? why : "cannot remount /boot read-write");
    }

    // WHAT WENT IN, WRITTEN DOWN. The core image lives in raw sectors
    // nothing can open and is lzma-compressed besides, so its module set
    // cannot be recovered by looking at the disk. This stamp is the only
    // record, and `remote.py flash` reads it before sending a kernel
    // whose unpacking GRUB has to do.
    uint8_t *mods = 0;
    size_t mod_size = 0;
    if (ufile_slurp(UINSTALL_PAYLOAD "/core.modules", 0, &mods, &mod_size) == UFILE_OK) {
        int fd = open(STAMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, mods, mod_size);
            close(fd);
        } else {
            printf("install: could not write %s -- the bootloader is in "
                   "place, but nothing records what it can do.\n", STAMP_PATH);
        }
        free(mods);
    }

    step("adding bootpart= to grub.cfg");
    int added = refresh_grub_cfg(1);
    if (added > 0)
        printf("install: %d grub.cfg entr%s now pass%s bootpart= (the old file is %s.bak)\n",
               added, added == 1 ? "y" : "ies", added == 1 ? "es" : "", UBOOTMENU_CFG);
    else if (added == 0)
        printf("install: grub.cfg already passes bootpart=\n");

    printf("install: done. %s boots the newly written GRUB from now on%s.\n", disk,
           added < 0 ? "; grub.cfg is as it was" : "");
    return 0;
}

// --- choosing a disk -------------------------------------------------------

static struct udisk_scan g_scan;   // ~10 KB: never on the stack
static uint64_t g_need;            // bytes the root must hold, once measured

// Every whole disk, numbered when it can take an install; the rest say
// why not. Returns how many are numbered, with their indexes in `pick`.
static int list_disks(int esp, int mbr, int *pick, int max) {
    int n = 0;
    for (int i = 0; i < g_scan.ndisks; i++) {
        const struct udisk *d = &g_scan.disks[i];
        char size[16], what[64] = "";
        human_size_iec(size, sizeof size, d->sectors * 512);
        const char *why = uinstall_refusal(d, (uint64_t)esp, mbr, g_need);
        if (!d->persistent) snprintf(what, sizeof what, "live image");
        else snprintf(what, sizeof what, "%s", d->model[0] ? d->model : d->driver);
        int has = 0;
        for (int k = 0; k < d->nparts; k++) if (d->parts[k].fs[0] || d->parts[k].number) has = 1;
        if (!why && n < max) {
            pick[n++] = i;
            printf("  %d) %-8s %10s  %-28s %s\n", n, d->name, size, what, has ? "(has partitions)" : "(empty)");
        } else {
            printf("     %-8s %10s  %-28s %s\n", d->name, size, what, why ? why : "");
        }
    }
    return n;
}

static int ask(const char *prompt, char *out, int cap) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, cap, stdin)) return 0;
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}

// What erasing `d` loses, one line a partition.
static void print_losses(const struct udisk *d) {
    if (!d->nparts) { printf("  (no partitions -- nothing on it is lost)\n"); return; }
    for (int k = 0; k < d->nparts; k++) {
        const struct udisk_part *p = &d->parts[k];
        char size[16], title[40];
        human_size_iec(size, sizeof size, p->count * 512);
        udisk_part_title(p, title, sizeof title);
        printf("  %-10s %-14s %-6s %s\n", p->dev[0] ? p->dev : "-", title, udisk_part_fs(p), size);
    }
}

static void print_plan(const struct uinstall_plan *p) {
    char size[16];
    human_size_iec(size, sizeof size, p->sectors * 512);
    printf("install: this ERASES %s (%s) and puts this system on it, as %s:\n",
           p->disk, size, p->mbr ? "MBR" : "GPT");
    if (p->mbr) printf("  (gap)      1M        GRUB's core image, sectors 1..2047\n");
    for (int i = 0; i < p->nparts; i++) {
        const struct uinstall_part *q = &p->part[i];
        human_size_iec(size, sizeof size, q->sectors * 512);
        printf("  %-10s %-9s %-6s %s%s\n", q->dev, size, q->fs ? q->fs : "-", q->holds,
               p->mbr && i == p->boot ? " [active]" : "");
    }
}

// --- running it ------------------------------------------------------------

static struct uinstall_progress g_prog;
static struct uinstall_plan g_plan;
static unsigned g_flags;
static int g_tty;
static char g_detail[UINSTALL_STAGES][128];

static void print_stage(int s) {
    printf("[%d/%d] %-11s %s\n", s + 1, UINSTALL_STAGES, uinstall_stage_name(s), g_detail[s]);
}

// Off a terminal the run prints as it goes; on one, the main thread
// prints (one writer for the screen) and draws a meter over the copy.
static void on_stage(void *ctx, int s, const char *detail) {
    (void)ctx;
    if (s >= 0 && s < UINSTALL_STAGES) snprintf(g_detail[s], sizeof g_detail[s], "%s", detail);
    if (!g_tty) print_stage(s);
}

static void on_line(void *ctx, const char *line) {
    (void)ctx;
    if (!g_tty) printf("  %s\n", line);
}

static const struct uinstall_ops OPS = { 0, on_stage, on_line };

static void *worker(void *arg) {
    (void)arg;
    uinstall_run(&g_plan, g_flags, &g_prog, &OPS);
    return 0;
}

static int run_install(void) {
    uint64_t t0 = sys_monotonic_ns();
    if (!g_tty) {
        uinstall_run(&g_plan, g_flags, &g_prog, &OPS);
    } else {
        pthread_t th;
        if (pthread_create(&th, 0, worker, 0) != 0) {
            g_tty = 0;
            uinstall_run(&g_plan, g_flags, &g_prog, &OPS);
        } else {
            int shown = -1;
            struct uprogress meter;
            int metering = 0;
            uint64_t last = 0;
            for (;;) {
                int done = g_prog.finished;
                // A stage's detail is written as it begins, so a stage with
                // one has begun -- and a failed run begins no more.
                while (shown + 1 < UINSTALL_STAGES && g_detail[shown + 1][0]) {
                    if (metering) { uprogress_end(&meter); metering = 0; }
                    print_stage(++shown);
                    if (shown == UINSTALL_COPY && !g_detail[UINSTALL_BOOTLOADER][0] && !done) {
                        uprogress_begin(&meter, 1, g_prog.bytes_total);
                        metering = 1;
                        last = 0;
                    }
                }
                if (metering && g_prog.bytes_done > last) {
                    uprogress_add(&meter, g_prog.bytes_done - last);
                    last = g_prog.bytes_done;
                }
                if (done) break;
                sys_sleep_ms(200);
            }
            if (metering) uprogress_end(&meter);
            pthread_join(th, 0);
        }
    }
    char took[16];
    uduration_clock((sys_monotonic_ns() - t0) / 1000000, took, sizeof took);
    if (!g_prog.ok) {
        printf("install: FAILED -- %s\n", g_prog.error[0] ? g_prog.error : "see above");
        if (g_prog.errors > 1) printf("install: %d errors in all\n", g_prog.errors);
        printf("install: %s is in an unknown state; re-run to start over.\n", g_plan.disk);
        return 1;
    }
    printf("install: done in %s. Boot %s on its own and it should come up.\n", took, g_plan.disk);
    return 0;
}

// --- the questions ----------------------------------------------------------

static int interactive(int esp, int mbr, int mbr_given) {
    int pick[UDISK_MAX];
    printf("Install SaunaOS onto which disk?\n");
    int n = list_disks(esp, mbr, pick, UDISK_MAX);
    if (!n) {
        printf("install: no disk here can take an install.\n");
        return 1;
    }
    char reply[32];
    if (!ask("Disk [1]: ", reply, sizeof reply)) return 1;
    int choice = reply[0] ? atoi(reply) : 1;
    if (choice < 1 || choice > n) {
        printf("install: no disk %s -- nothing was changed.\n", reply);
        return 1;
    }
    struct udisk *d = &g_scan.disks[pick[choice - 1]];
    if (!mbr_given) {
        if (!ask("Partition table -- GPT, or MBR for firmware that won't boot GPT [GPT]: ",
                 reply, sizeof reply)) return 1;
        if (!strcasecmp(reply, "mbr")) mbr = 1;
        else if (reply[0] && strcasecmp(reply, "gpt")) {
            printf("install: %s is neither GPT nor MBR -- nothing was changed.\n", reply);
            return 1;
        }
    }
    const char *why = uinstall_plan(&g_plan, &g_scan, d->name, (uint64_t)esp, mbr, g_need);
    if (why) { printf("install: %s: %s.\n", d->name, why); return 1; }
    if ((why = uinstall_payload_problem(mbr))) { printf("install: %s.\n", why); return 1; }
    printf("This ERASES %s:\n", d->name);
    print_losses(d);
    char prompt[64];
    snprintf(prompt, sizeof prompt, "Type %s to go on: ", d->name);
    if (!ask(prompt, reply, sizeof reply)) return 1;
    if (strcmp(reply, d->name) != 0) {
        printf("install: nothing was changed.\n");
        return 1;
    }
    return run_install();
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    int confirmed = 0;
    for (int i = 0; i < a.argc; i++) {
        if (!strcmp(a.argv[i], "confirm")) { confirmed = 1; continue; }
        return uargs_error(&PROG, "unexpected `%s` -- the one word it takes is `confirm`", a.argv[i]);
    }
    int esp = f_esp ? atoi(f_esp) : UINSTALL_ESP_DEFAULT;
    g_flags = f_ask_all ? UINSTALL_ASK_ALL : 0;

    if (f_migrate) {
        if (f_bootloader || f_disk || f_mbr) {
            refuse("--migrate rewrites THIS machine's bootloader; it takes no other flag.");
            return 1;
        }
        if (!ubootlayout_legacy()) {
            printf("install: /boot has kernel.bin and grub/ at its root already -- "
                   "nothing to move.\n");
            return 0;
        }
        return refresh_bootloader(confirmed);
    }
    if (f_bootloader) {
        // The target is this machine's own disk and the layout is read
        // from it, so neither flag has a meaning here -- refused rather
        // than ignored, since a `--mbr` silently doing nothing is how
        // somebody ends up believing they chose a layout.
        if (f_disk || f_mbr) {
            refuse("--bootloader rewrites THIS machine's bootloader; "
                   "--disk and --mbr do not apply.");
            return 1;
        }
        return refresh_bootloader(confirmed);
    }
    if (esp < UINSTALL_ESP_MIN) { refuse("--esp must be at least 8 MiB"); return 1; }

    udisk_scan(&g_scan);
    uint32_t files = 0;
    uint64_t bytes = 0;
    if (uinstall_measure(&files, &bytes) == 0) g_need = uinstall_root_need(bytes);
    g_tty = sys_isatty(0) && sys_isatty(1);

    if (!f_disk) {
        if (g_tty) return interactive(esp, f_mbr, f_mbr);
        int pick[UDISK_MAX];
        printf("Disks this can install onto:\n");
        list_disks(esp, f_mbr, pick, UDISK_MAX);
        printf("Next: install --disk NAME   (prints the plan; add `confirm` to go ahead)\n");
        return 1;
    }

    char mine[16] = "";
    uinstall_root_disk(mine, sizeof mine);
    if (mine[0] && strcmp(mine, f_disk) == 0) {
        printf("install: %s is the disk this machine is running from -- refused.\n", f_disk);
        printf("install: install ONTO another disk; there is no reading of "
               "\"over myself\" that ends with a working machine.\n");
        return 1;
    }
    const char *why = uinstall_payload_problem(f_mbr);
    if (why) { printf("install: %s.\n", why); return 1; }
    why = uinstall_plan(&g_plan, &g_scan, f_disk, (uint64_t)esp, f_mbr, g_need);
    if (why) { printf("install: %s: %s.\n", f_disk, why); return 1; }

    if (!confirmed) {
        print_plan(&g_plan);
        if (!g_tty) {
            printf("install: re-run with `confirm` as the last argument if that is what you want.\n");
            return 1;
        }
        struct udisk *d = udisk_named(&g_scan, f_disk);
        printf("These are lost:\n");
        print_losses(d);
        char prompt[64], reply[32];
        snprintf(prompt, sizeof prompt, "Type %s to go on: ", f_disk);
        if (!ask(prompt, reply, sizeof reply) || strcmp(reply, f_disk) != 0) {
            printf("install: nothing was changed.\n");
            return 1;
        }
    }
    return run_install();
}
