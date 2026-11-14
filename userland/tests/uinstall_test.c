// The install engine's plan and refusals (base/uinstall.h), on fixture
// disks -- the arithmetic tools/tests/sweep/installer_test.py and install_test.py
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
#include <sys/stat.h>
#include <unistd.h>
#include "partition_abi.h"
#include "base/ufileop.h"
#include "base/uinstall.h"
#include "base/utest.h"

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

// ---- the software choice (base/uinstall.h's uinstall_software) -------------
//
// Against definitions of its own, in a directory of their own: an engine,
// a browser that needs it, demos whose directory holds a file of the
// engine's (the /tests/nsmonkey shape), and one removed "on this system".
// What a broken version would still pass: closures checked one way only
// (untick the engine, never tick the browser); a directory skipped whole
// when a longer path inside it goes in -- the copy is read back, not just
// the predicate; and sums checked only for "smaller", not for adding up.

#define SW_DEFS  "/tmp/uit.defs"
#define SW_FILES "/tmp/uit.files"
#define SW_COPY  "/tmp/uit.copy"
#define SW_ROOT  "/tmp/uit.root"

static int put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int there(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *const DEFS[][2] = {
    { "uiteng",  "Name=Engine\nLicense=GPL-2.0-only\nPaths=" SW_FILES "/eng/ " SW_FILES "/demo/nsmonkey\n" },
    { "uitbrw",  "Name=Browser\nLicense=MIT\nNeeds=uiteng\nPaths=" SW_FILES "/brw\n" },
    { "uitdem",  "Name=Demos\nLicense=MIT\nPaths=" SW_FILES "/demo/\n" },
    { "uitsolo", "Name=Solo\nLicense=MIT\nPaths=" SW_FILES "/solo\n" },
};

static void software_fixture(int make) {
    static const char *const FILES[] = { SW_FILES "/eng/a", SW_FILES "/brw", SW_FILES "/demo/x",
                                         SW_FILES "/demo/nsmonkey", SW_FILES "/base" };
    char path[96];
    if (make) {
        mkdir(SW_DEFS, 0755);
        mkdir(SW_FILES, 0755);
        mkdir(SW_FILES "/eng", 0755);
        mkdir(SW_FILES "/demo", 0755);
        for (size_t i = 0; i < sizeof DEFS / sizeof DEFS[0]; i++) {
            snprintf(path, sizeof path, SW_DEFS "/%s.component", DEFS[i][0]);
            put(path, DEFS[i][1]);
        }
        for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) put(FILES[i], "x\n");
        return;
    }
    for (size_t i = 0; i < sizeof DEFS / sizeof DEFS[0]; i++) {
        snprintf(path, sizeof path, SW_DEFS "/%s.component", DEFS[i][0]);
        remove(path);
    }
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        remove(FILES[i]);
        snprintf(path, sizeof path, SW_COPY "%s", FILES[i] + sizeof SW_FILES - 1);
        remove(path);
    }
    static const char *const DIRS[] = { SW_FILES "/eng", SW_FILES "/demo", SW_FILES, SW_COPY "/eng",
                                        SW_COPY "/demo", SW_COPY, SW_DEFS };
    for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) rmdir(DIRS[i]);
}

static int ix(const struct uinstall_software *sw, const char *id) { return ucomponent_find(&sw->set, id); }

static int copy_leave_out(void *ctx, const char *src, int is_dir) {
    return uinstall_software_skips(ctx, src, is_dir);
}

static struct ufileop g_copy;    // tens of KB: never on a frame

static void software_choice(void) {
    software_fixture(1);
    static struct uinstall_software sw;
    utest_check(uinstall_software_load_dir(&sw, SW_DEFS) == 0 && sw.set.count == 4, "four test components load");
    int eng = ix(&sw, "uiteng"), brw = ix(&sw, "uitbrw"), dem = ix(&sw, "uitdem"), solo = ix(&sw, "uitsolo");
    if (eng < 0 || brw < 0 || dem < 0 || solo < 0) { uinstall_software_free(&sw); software_fixture(0); return; }
    utest_check(uinstall_software_count_in(&sw) == 4, "...each starting in: none is removed here");

    utest_checkf(uinstall_software_choose(&sw, eng, 0) == 2 && !uinstall_software_in(&sw, brw),
                 "leaving the engine out leaves the browser that needs it out too");
    utest_checkf(uinstall_software_choose(&sw, brw, 1) == 2 && uinstall_software_in(&sw, eng),
                 "putting the browser in puts the engine back");
    utest_check(uinstall_software_choose(&sw, brw, 0) == 1 && uinstall_software_in(&sw, eng),
                "...and leaving the browser out keeps the engine");
    uinstall_software_choose(&sw, brw, 1);

    // One removed on this system: nothing to copy, so it cannot go in.
    sw.absent[solo] = 1;
    sw.set.c[solo].removed = 1;
    utest_check(uinstall_software_choose(&sw, solo, 1) < 0 && !uinstall_software_in(&sw, solo),
                "a component removed here cannot be put in");
    uinstall_software_all(&sw, 0);
    utest_check(uinstall_software_count_in(&sw) == 0, "The system only leaves every one out");
    uinstall_software_all(&sw, 1);
    utest_check(uinstall_software_count_in(&sw) == 3 && !uinstall_software_in(&sw, solo),
                "Select all puts every one here in, and not the one removed here");

    char said[128];
    uinstall_software_choose(&sw, dem, 0);
    uinstall_software_describe(&sw, said, sizeof said);
    utest_checkf(!strcmp(said, "2 of 4 components -- Demos, Solo left out"), "the choice in words ('%s')", said);

    // Demos out, the engine in: its directory stays for the engine's file.
    utest_check(!uinstall_software_skips(&sw, SW_FILES "/demo", 1),
                "a left-out directory holding a file of a component going in is entered");
    utest_check(uinstall_software_skips(&sw, SW_FILES "/demo/x", 0), "...its own files are left out");
    utest_check(!uinstall_software_skips(&sw, SW_FILES "/demo/nsmonkey", 0), "...the longer path's file is not");
    utest_check(!uinstall_software_skips(&sw, SW_FILES "/base", 0) && !uinstall_software_skips(&sw, SW_FILES, 1),
                "the base system is never left out");

    // The copy itself, through ufileop's leave_out: read back, not trusted.
    struct ufileop_policy pol = { .ctx = &sw, .leave_out = copy_leave_out };
    int rc = ufileop_copy(SW_FILES, SW_COPY, &g_copy, &pol);
    utest_check(rc == UFILEOP_OK && there(SW_COPY "/demo/nsmonkey") && !there(SW_COPY "/demo/x") &&
                there(SW_COPY "/brw") && there(SW_COPY "/eng/a") && there(SW_COPY "/base"),
                "a copy with Demos out carries everything but Demos' own files");
    uinstall_software_choose(&sw, eng, 0);
    utest_check(uinstall_software_skips(&sw, SW_FILES "/demo", 1) && uinstall_software_skips(&sw, SW_FILES "/eng", 1),
                "with the engine out too, both directories are left out whole");
    software_fixture(0);
    uinstall_software_free(&sw);
}

// The sums: a component over a real directory of the root (/tmp is not
// measured), in and then out. They must ADD UP to the whole.
static void software_sums(void) {
    mkdir(SW_DEFS, 0755);
    put(SW_DEFS "/uitfnt.component", "Name=Fonts\nLicense=MIT\nPaths=/usr/share/fonts/\n");
    static struct uinstall_software sw;
    uint32_t whole_f = 0, in_f = 0, out_f = 0;
    uint64_t whole_b = 0, in_b = 0, out_b = 0, left = 0;
    int ok = uinstall_measure(0, &whole_f, &whole_b) == 0 && uinstall_software_load_dir(&sw, SW_DEFS) == 0 &&
             uinstall_measure(&sw, &in_f, &in_b) == 0;
    int f = ix(&sw, "uitfnt");
    // Files, not bytes, across two walks: a log grows between them.
    utest_checkf(ok && f >= 0 && in_f == whole_f,
                 "with nothing left out the copy is the whole root (%u of %u files)", in_f, whole_f);
    if (f >= 0) {
        uinstall_software_choose(&sw, f, 0);
        uinstall_software_totals(&sw, &out_f, &out_b, &left);
        utest_checkf(sw.files[f] > 0 && out_f + sw.files[f] == in_f && out_b + left == in_b &&
                     left == sw.bytes[f], "leaving a component out takes exactly its files off (%u - %u = %u)",
                     in_f, sw.files[f], out_f);
        uint32_t again_f = 0;
        uint64_t again_b = 0;
        uinstall_measure(&sw, &again_f, &again_b);
        utest_check(again_f == out_f, "...and a new measure says the same");
    }
    uinstall_software_free(&sw);
    remove(SW_DEFS "/uitfnt.component");
    rmdir(SW_DEFS);
}

// A target's state file, under its own root; this machine's untouched.
static void software_state(void) {
    utest_check(ucomponent_state_set_at(SW_ROOT, "uitx", 1) == 0, "a state file is written under another root");
    FILE *fp = fopen(SW_ROOT "/var/lib/update/components", "r");
    char buf[256] = "";
    size_t n = fp ? fread(buf, 1, sizeof buf - 1, fp) : 0;
    if (fp) fclose(fp);
    buf[n] = '\0';
    utest_checkf(strstr(buf, "removed uitx\n") != 0, "...saying `removed uitx` ('%.60s')", buf);
    utest_check(!ucomponent_state_removed("uitx"), "...and this machine's own state does not");
    remove(SW_ROOT "/var/lib/update/components");
    rmdir(SW_ROOT "/var/lib/update");
    rmdir(SW_ROOT "/var/lib");
    rmdir(SW_ROOT "/var");
    rmdir(SW_ROOT);
}

int main(void) {
    utest_begin("uinstall_test", "the install plan: layouts and refusals, and the software choice", 0);

    add("fixt0", 512 * MIB, 1, 512);
    add("fixtram", 171 * MIB, 0, 512);
    add("fixt4k", 512 * MIB, 1, 4096);
    add("fixttiny", 40 * MIB, 1, 512);
    add("fixtbig", 3ull * 1024 * 1024 * MIB, 1, 512);   // 3 TiB
    add("fixt2t", 1ull << 32, 1, 512);                  // exactly 2 TiB
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
    why = uinstall_plan(&p, &g_scan, "fixtbig", 64, 1, 0);
    utest_check(why && strstr(why, "2 TiB"), "MBR past 2 TiB is refused, saying why");
    utest_check(!uinstall_plan(&p, &g_scan, "fixtbig", 64, 0, 0), "...and GPT takes the same disk");
    utest_check(!uinstall_plan(&p, &g_scan, "fixt2t", 64, 1, 0), "MBR on exactly 2 TiB fits");
    utest_check(!uinstall_esp_refusal(64, 3u << 20, 1u << 20), "a 64 MiB ESP holds two 3 MiB kernels");
    why = uinstall_esp_refusal(8, 3u << 20, 1u << 20);
    utest_check(why && strstr(why, "too small"), "an 8 MiB ESP does not, with room to grow");

    // The disk re-check: same name, a different table or model.
    struct udisk *f0 = udisk_named(&g_scan, "fixt0");
    uint64_t print0 = uinstall_disk_print(f0);
    utest_check(!uinstall_plan(&p, &g_scan, "fixt0", 64, 0, 0) && p.seen == print0,
                "the plan records the disk as it was chosen");
    f0->nparts = 1;
    f0->parts[0].start = 2048;
    f0->parts[0].count = 4096;
    uint64_t print1 = uinstall_disk_print(f0);
    f0->parts[0].count = 8192;
    uint64_t print2 = uinstall_disk_print(f0);
    f0->nparts = 0;
    snprintf(f0->model, sizeof f0->model, "Another stick");
    uint64_t print3 = uinstall_disk_print(f0);
    memset(f0->model, 0, sizeof f0->model);
    utest_check(print1 != print0 && print2 != print1 && print3 != print0 &&
                uinstall_disk_print(f0) == print0,
                "a partition, its size or the model changes the fingerprint; putting it back restores it");
    utest_check(uinstall_plan(&p, &g_scan, "nosuch", 64, 0, 0) != 0, "a disk that is not there is refused");
    utest_check(p.nparts == 0, "...and a refused plan lays out nothing");

    utest_check(!strcmp(uinstall_stage_name(UINSTALL_COPY), "Copy files") &&
                !strcmp(uinstall_stage_name(UINSTALL_BOOTLOADER), "Bootloader"), "the stages' names");

    software_choice();
    software_sums();
    software_state();
    return utest_end();
}
