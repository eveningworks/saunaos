// The install engine's plan and refusals (lib/uinstall.h), on fixture
// disks -- the arithmetic tools/installer_test.py and install_test.py
// only ever see one size of.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - A plan checked only by "three partitions" would pass with the ESP
//     at partition 1 -- and GRUB's core image finds its config only when
//     the ESP is partition 2 on GPT and 1 on MBR (the layout trap). The
//     start of every partition is compared, and the device names.
//   - A GPT root that ran to the disk's last sector would pass every
//     check but this one: the backup header lives in the last 33.
//   - A refusal tested only on the disk the system runs from would pass
//     with no other check at all; a live image, 4K sectors, a disk too
//     small for the layout and one too small for the SYSTEM each refuse,
//     and an ordinary disk does not.
//   - The fixtures are named nothing a test machine has, since a refusal
//     also asks the kernel which disk the root is on.
#include <stdio.h>
#include <string.h>
#include "partition_abi.h"
#include "lib/uinstall.h"
#include "lib/utest.h"

#define MIB 2048ull   // sectors

static struct udisk_scan g_scan;

static struct udisk *add(const char *name, uint64_t sectors, int persistent, int block) {
    struct udisk *d = udisk_add(&g_scan);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->sectors = sectors;
    d->persistent = persistent;
    d->block_size = (uint64_t)block;
    return d;
}

int main(void) {
    utest_begin("uinstall_test", "the install plan: layouts and refusals", 0);

    add("fixt0", 512 * MIB, 1, 512);
    add("fixtram", 171 * MIB, 0, 512);
    add("fixt4k", 512 * MIB, 1, 4096);
    add("fixttiny", 40 * MIB, 1, 512);
    struct udisk *sys = add("fixtsys", 512 * MIB, 1, 512);
    sys->system = 1;

    struct uinstall_plan p;
    const char *why = uinstall_plan(&p, &g_scan, "fixt0", 64, 0, 0);
    utest_check(!why, "an ordinary 512 MiB disk takes a GPT install");
    utest_check(p.nparts == 3 && p.boot == 1 && p.root == 2, "GPT: BIOS boot, then the ESP, then the root");
    utest_check(p.part[0].start == 2048 && p.part[0].sectors == 2048 && p.part[0].role == MKPART_ROLE_BIOS_BOOT,
                "the BIOS boot partition is 1 MiB at the 1 MiB mark");
    utest_check(p.part[1].start == 4096 && p.part[1].sectors == 64 * MIB && p.part[1].role == MKPART_ROLE_ESP,
                "the ESP is partition 2, 64 MiB, straight after it");
    utest_check(p.part[2].start == 4096 + 64 * MIB && p.part[2].start + p.part[2].sectors == 512 * MIB - 33,
                "the root takes the rest, short of GPT's 33-sector backup");
    utest_check(!strcmp(p.part[1].dev, "fixt0p2") && !strcmp(p.part[2].dev, "fixt0p3"),
                "named fixt0p2 and fixt0p3");
    utest_check(p.part[1].fs && !strcmp(p.part[1].fs, "fat32") && p.part[2].fs && !strcmp(p.part[2].fs, "tfs3")
                && !p.part[0].fs, "FAT32 and TFS3; the BIOS boot partition holds no filesystem");

    why = uinstall_plan(&p, &g_scan, "fixt0", 64, 1, 0);
    utest_check(!why && p.mbr && p.nparts == 2 && p.boot == 0 && p.root == 1, "MBR: the ESP, then the root");
    utest_check(p.part[0].start == 2048 && p.part[1].start == 2048 + 64 * MIB &&
                p.part[1].start + p.part[1].sectors == 512 * MIB,
                "MBR: the ESP at 1 MiB as partition 1, the root to the last sector");
    utest_check(!strcmp(p.part[0].dev, "fixt0p1") && !strcmp(p.part[1].dev, "fixt0p2"), "named fixt0p1 and fixt0p2");

    utest_check(uinstall_plan(&p, &g_scan, "fixtsys", 64, 0, 0) != 0, "the disk the system runs from is refused");
    utest_check(uinstall_plan(&p, &g_scan, "fixtram", 64, 0, 0) != 0, "a live image is refused");
    why = uinstall_plan(&p, &g_scan, "fixt4k", 64, 0, 0);
    utest_check(why && strstr(why, "4096"), "4K sectors are refused, by their size");
    utest_check(uinstall_plan(&p, &g_scan, "fixttiny", 64, 0, 0) != 0,
                "40 MiB cannot hold a 64 MiB ESP");
    utest_check(!uinstall_plan(&p, &g_scan, "fixttiny", 8, 0, 0), "...but holds an 8 MiB one");
    why = uinstall_plan(&p, &g_scan, "fixt0", 64, 0, uinstall_root_need(600ull * 1024 * 1024));
    utest_check(why && strstr(why, "needs"), "a system bigger than the disk is refused, saying what it needs");
    utest_check(!uinstall_plan(&p, &g_scan, "fixt0", 64, 0, uinstall_root_need(131ull * 1024 * 1024)),
                "a 131 MiB system fits on 512 MiB");
    utest_check(uinstall_plan(&p, &g_scan, "fixt0", 4, 0, 0) != 0, "an ESP under 8 MiB is refused");
    utest_check(uinstall_plan(&p, &g_scan, "nosuch", 64, 0, 0) != 0, "a disk that is not there is refused");
    utest_check(p.nparts == 0, "...and a refused plan lays out nothing");

    utest_check(!strcmp(uinstall_stage_name(UINSTALL_COPY), "Copy files") &&
                !strcmp(uinstall_stage_name(UINSTALL_BOOTLOADER), "Bootloader"), "the stages' names");

    return utest_end();
}
