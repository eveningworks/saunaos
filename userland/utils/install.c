// install -- put this running system onto another disk, and make it boot.
//
// The five stages are base/uinstall.h's, which the installer window runs
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
// WHAT IT COPIES is this system less the optional components --without
// or --only-system leave out (base/uinstall.h's software choice, the
// window's Software step), each then marked removed on the target.
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
#include "base/uargs.h"
#include "base/human.h"
#include "base/ufile.h"
#include "base/ubootlayout.h"
#include "base/ubootrefresh.h"
#include "base/udiskscan.h"
#include "base/ugrow.h"
#include "base/uduration.h"
#include "base/uinstall.h"
#include "base/uprogress.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *f_disk, *f_esp, *f_without;
static int f_mbr, f_ask_all, f_bootloader, f_migrate, f_only_system, f_uefi;

static const struct uargs_opt OPTS[] = {
    { "disk", 0, "NAME", "the target, as `lsblk` names it; refuses the one this machine runs from", 0, &f_disk },
    { "esp", 0, "MIB", "size of the FAT32 /boot partition (default 64)", 0, &f_esp },
    { "mbr", 0, 0, "an MBR table instead of GPT, for firmware that will not boot a GPT disk", &f_mbr, 0 },
    { "ask-all", 0, 0, "the new machine's First-boot setup asks every page, not just the unanswered", &f_ask_all, 0 },
    { "without", 0, "ID,...", "leave these optional components out, and what needs them (`component list`)", 0,
      &f_without },
    { "only-system", 0, 0, "leave every optional component out: the base system alone", &f_only_system, 0 },
    { "bootloader", 0, 0, "rewrite THIS machine's bootloader in place; no partitioning", &f_bootloader, 0 },
    { "uefi", 0, 0, "with --bootloader: ADD the UEFI loader to a disk that boots BIOS/CSM, so the firmware may boot it the UEFI way", &f_uefi, 0 },
    { "migrate", 0, 0, "the same, only when /boot/boot is still there (the boot-migrate service)", &f_migrate, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "install",
    .usage = "[--disk NAME] [--esp MIB] [--mbr] [--ask-all] [--without ID,...] [--only-system] [confirm]\n"
             "--bootloader [--uefi] [confirm]\n"
             "--migrate [confirm]",
    .summary = "Put this running system onto another disk, and make that disk boot.\n"
               "With no --disk at a terminal it asks which; `confirm` ERASES the target.",
    .opts = OPTS,
    .notes = "The installer window (Install SaunaOS, on a live session's desktop) runs the same steps.",
};

static void refuse(const char *why) { printf("install: %s\n", why); }

static void say(void *ctx, const char *line) { (void)ctx; printf("install: %s\n", line); }

// THE BOOTLOADER, REWRITTEN IN PLACE -- `grub-install` on a machine that
// is already installed (base/ubootrefresh.h, which System Update runs
// too). It exists because an installed machine could not otherwise gain a
// boot loader it does not already have: the full installer ERASES its
// target and refuses the disk the machine runs from.
static int refresh_bootloader(int confirmed) {
    struct ubootrefresh_plan p;
    const char *no = ubootrefresh_plan(&p);
    if (no) { printf("install: %s.\n", no); return 1; }
    if (f_uefi && ubootrefresh_add_efi(&p) < 0) {
        printf("install: this build staged no UEFI loader in " UINSTALL_PAYLOAD " -- nothing to add.\n");
        return 1;
    }
    if (confirmed) return ubootrefresh_run(&p, say, 0) == 0 ? 0 : 1;

    printf("install: this rewrites %s's bootloader in place (%s layout):\n",
           p.disk, p.mbr ? "MBR" : "GPT");
    if (p.legacy)
        printf("  /boot/boot     copied up to /boot first, removed once the new GRUB "
               "is in place\n");
    printf("  boot sector    LBA 0, keeping this disk's partition table\n");
    printf("  core image     %s\n", p.core);
    if (p.has_efi)
        printf("  UEFI loader    /boot/EFI/BOOT/BOOTX64.EFI, from "
               UINSTALL_PAYLOAD "/bootx64.efi\n");
    else if (p.efi_kept_off)
        printf("  UEFI loader    none added -- this disk boots BIOS/CSM (--uefi adds one)\n");
    if (p.legacy) {
        // grub.cfg is read once it has moved up
    } else if (p.cfg_missing > 0) {
        printf("  grub.cfg       bootpart= added to %d entr%s (a .bak is kept)\n",
               p.cfg_missing, p.cfg_missing == 1 ? "y" : "ies");
    } else if (p.cfg_missing == 0) {
        printf("  grub.cfg       already passes bootpart= -- unchanged\n");
    } else {
        printf("  grub.cfg       cannot be read -- left as it is\n");
    }
    struct ubootrefresh_offer o;
    ubootrefresh_offer(&o);
    if (o.kind == UBOOTREFRESH_UNKNOWN)
        printf("  record         none on this disk -- what its GRUB carries is not known\n");
    else if (o.kind == UBOOTREFRESH_NEWER && o.adds && o.adds[0])
        printf("  adds           %s\n", o.adds);
    else if (o.kind == UBOOTREFRESH_SAME)
        printf("  record         this disk already has the staged GRUB%s%s\n",
               o.version ? " " : "", o.version ? o.version : "");
    ubootrefresh_offer_free(&o);
    printf("install: partitions, filesystems and other files are untouched. "
           "The core image is\n");
    printf("install: read back before the boot sector is written, so a "
           "bad write leaves the\n");
    printf("install: bootloader you have now. Re-run with `confirm` if "
           "that is what you want.\n");
    return 1;
}

// --- choosing a disk -------------------------------------------------------

static struct udisk_scan g_scan;
static int *g_pick, g_pick_cap;    // list_disks()'s numbered disks, one per disk
static uint64_t g_need;            // bytes the root must hold, once measured

// Every whole disk, numbered when it can take an install; the rest say
// why not. Returns how many are numbered, with their indexes in `pick`.
static int list_disks(int esp, int mbr) {
    int n = 0, max = 0;
    if (UGROW(g_pick, g_pick_cap, g_scan.ndisks > 0 ? g_scan.ndisks : 1)) max = g_pick_cap;
    else fprintf(stderr, "install: out of memory listing %d disks\n", g_scan.ndisks);
    int *pick = g_pick;
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

// --- what goes on the disk ----------------------------------------------------

static struct uinstall_software g_sw;   // this system's components; removed = left out

// --without and --only-system, onto the session's own set: an id this
// build has no component for is refused by name, and what needs one left
// out goes with it, said aloud. 0, or 1 having said why not.
static int choose_software(void) {
    if (uinstall_software_load(&g_sw) != 0) {
        printf("install: out of memory reading the optional components -- nothing was changed.\n");
        return 1;
    }
    if (f_only_system) uinstall_software_all(&g_sw, 0);
    if (!f_without) return 0;
    char *list = strdup(f_without);
    if (!list) return 1;
    int bad = 0;
    for (char *id = list, *next; id && !bad; id = next) {
        next = strchr(id, ',');
        if (next) *next++ = '\0';
        while (*id == ' ') id++;
        if (!*id) continue;
        int i = ucomponent_find(&g_sw.set, id);
        if (i < 0) {
            printf("install: there is no optional component `%s` -- this build has:", id);
            for (int k = 0; k < g_sw.set.count; k++) printf("%s %s", k ? "," : "", g_sw.set.c[k].id);
            printf("%s\n", g_sw.set.count ? "" : " none");
            bad = 1;
            continue;
        }
        int *deps = malloc(sizeof *deps * (size_t)(g_sw.set.count + 1));
        int nd = deps ? ucomponent_dependents(&g_sw.set, i, deps, g_sw.set.count + 1) : 0;
        uinstall_software_choose(&g_sw, i, 0);
        for (int k = 0; k < nd; k++)
            printf("install: leaving out %s too -- it needs %s\n", g_sw.set.c[deps[k]].name, g_sw.set.c[i].name);
        free(deps);
    }
    free(list);
    return bad;
}

static void print_software(void) {
    char sw[192], size[16];
    uint32_t files = 0;
    uint64_t bytes = 0;
    uinstall_software_totals(&g_sw, &files, &bytes, 0);
    human_size_iec(size, sizeof size, bytes);
    uinstall_software_describe(&g_sw, sw, sizeof sw);
    printf("  copies     %u files, %s\n", (unsigned)files, size);
    printf("  software   %s\n", sw);
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
    print_software();
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
    uinstall_run(&g_plan, g_flags, &g_sw, &g_prog, &OPS);
    return 0;
}

static int run_install(void) {
    uint64_t t0 = sys_monotonic_ns();
    if (!g_tty) {
        uinstall_run(&g_plan, g_flags, &g_sw, &g_prog, &OPS);
    } else {
        pthread_t th;
        if (pthread_create(&th, 0, worker, 0) != 0) {
            g_tty = 0;
            uinstall_run(&g_plan, g_flags, &g_sw, &g_prog, &OPS);
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
    printf("Install SaunaOS onto which disk?\n");
    int n = list_disks(esp, mbr);
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
    struct udisk *d = &g_scan.disks[g_pick[choice - 1]];
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
    print_software();
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

    if ((f_migrate || f_bootloader) && (f_without || f_only_system)) {
        refuse("--without and --only-system choose what a NEW install copies; "
               "--bootloader and --migrate copy nothing.");
        return 1;
    }
    if (f_uefi && !f_bootloader) {
        refuse("--uefi adds the UEFI loader to THIS machine's disk; it goes with --bootloader.");
        return 1;
    }
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
    if (choose_software()) return 1;

    udisk_scan(&g_scan);
    uint32_t files = 0;
    uint64_t bytes = 0;
    // What is left out is not needed room: the disks offered follow the choice.
    if (uinstall_measure(&g_sw, &files, &bytes) == 0) g_need = uinstall_root_need(bytes);
    g_tty = sys_isatty(0) && sys_isatty(1);

    if (!f_disk) {
        if (g_tty) return interactive(esp, f_mbr, f_mbr);
        printf("Disks this can install onto:\n");
        list_disks(esp, f_mbr);
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
