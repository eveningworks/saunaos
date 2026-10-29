// part -- change ONE partition of a disk: delete it, or resize it.
//
// The table is edited in place (SYS_PARTEDIT): every other partition
// keeps its bytes, its number and its device name, which is what
// `sfdisk --delete` and diskpart's `delete partition` do. `mkpart`
// writes a whole new table; `parttable` reads one.
//
// A RESIZE MOVES THE FILESYSTEM TOO, in the safe order: growing, the
// table first and then the filesystem (SYS_FS_RESIZE) -- GParted's and
// KDE Partition Manager's order, sfdisk then resize2fs. A grow cut short
// between the two leaves a partition with room its filesystem does not
// use yet, which mounts as it was; running the same command again
// finishes it. Only growing is built (tfs3, FAT32); the kernel says why
// for anything it refuses, and that reason is what is printed. A FAT32
// grow that moves its data is not safe against an interruption
// (FS_RESIZE_UNSAFE), and both the question and the run say so.
//
// The disk is re-read at once, so the change shows in `lsblk` without a
// reboot. The system disk is refused: its table is what the next boot
// finds the root by.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "knum.h"
#include "lib/human.h"
#include "lib/uargs.h"
#include "lib/udiskops.h"
#include "lib/udiskscan.h"

static const struct uargs_cmd CMDS[] = {
    { "delete", "PARTITION confirm", "remove PARTITION from its disk's table (`lsblk` names it)" },
    { "resize", "PARTITION SIZE confirm",
      "make PARTITION SIZE long (40G, +10G or max), its filesystem with it" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "part",
    .usage = "delete PARTITION confirm | resize PARTITION SIZE confirm",
    .summary = "Change one partition of a disk; the others keep their numbers and names.",
    .cmds = CMDS,
    .notes = "PARTITION must not be mounted, and must not be on the disk the system runs from.\n"
             "SIZE is the new length: 40G, 512M or 40GiB; +10G grows it by that much; max takes\n"
             "all the free space after it. A bare number is sectors, as mkpart reads one.\n"
             "Only growing is built for a filesystem (tfs3, FAT32); exFAT is not resized yet.\n"
             "FAT32 keeps no journal: a grow that moves its data and is cut short loses the volume.\n"
             "Without `confirm` it only says what would change.",
};

// What `lsblk` knows of it, for the sentence that says what is about to go.
static int describe(const char *dev, char *out, int cap) {
    struct query_blkdev b;
    QUERY_FOREACH(QUERY_BLKDEV, b, i) {
        if (strcmp(b.name, dev) != 0) continue;
        if (!b.parent[0]) return -1;   // a disk
        char size[16], label[72] = "";
        human_size(size, sizeof size, b.sectors * 512ull);
        if (b.label[0]) snprintf(label, sizeof label, " \"%s\"", b.label);
        snprintf(out, (size_t)cap, "%s (%s%s%s%s) on %s", dev, size, b.fstype[0] ? " " : "", b.fstype,
                 label, b.parent);
        return 0;
    }
    return 1;
}

static int do_delete(int argc, char **argv) {
    if (argc < 2) return uargs_error(&PROG, "delete needs a PARTITION");
    if (argc > 3 || (argc == 3 && strcmp(argv[2], "confirm") != 0))
        return uargs_error(&PROG, "the last word must be `confirm`");
    const char *dev = argv[1];
    char what[160];
    int d = describe(dev, what, sizeof what);
    if (d < 0) return uargs_error(&PROG, "%s is a disk, not a partition", dev);
    if (d > 0) return uargs_error(&PROG, "no partition %s (`lsblk` lists them)", dev);
    if (argc < 3) {
        fprintf(stderr, "part: this DELETES %s, and whatever is in it.\n", what);
        fprintf(stderr, "part: re-run as `part delete %s confirm` if that is what you want.\n", dev);
        return 1;
    }
    struct partedit_request req;
    memset(&req, 0, sizeof req);
    req.op = PARTEDIT_DELETE;
    req.flags = PARTEDIT_CONFIRM;
    snprintf(req.device, sizeof req.device, "%s", dev);
    if (sys_partedit(&req) < 0) {
        fprintf(stderr, "part: %s not deleted -- %s\n", dev, req.why[0] ? req.why : strerror(errno));
        return 1;
    }
    printf("Deleted %s. The other partitions keep their names.\n", what);
    return 0;
}

// --- resize ----------------------------------------------------------------

static struct udisk_scan g_scan;

// SIZE as sectors, given the partition's length now and the most it may
// have. 0 for anything not understood -- a parser refuses rather than
// guesses, and a misread size is a partition somewhere it was not asked.
static uint64_t parse_new_size(const char *s, uint64_t now, uint64_t most) {
    if (!strcmp(s, "max")) return most;
    int plus = s[0] == '+';
    uint64_t bytes;
    if (!k_parse_size(s + plus, 512, &bytes) || bytes < 512) return 0;
    return plus ? now + bytes / 512 : bytes / 512;
}

static int fs_info(const char *dev, struct fsresize_request *req) {
    memset(req, 0, sizeof *req);
    snprintf(req->source, sizeof req->source, "%s", dev);
    req->flags = FSRESIZE_INFO;
    return sys_fs_resize(req) < 0 ? -errno : 0;
}

static int do_resize(int argc, char **argv) {
    if (argc < 3) return uargs_error(&PROG, "resize needs a PARTITION and a SIZE");
    if (argc > 4 || (argc == 4 && strcmp(argv[3], "confirm") != 0))
        return uargs_error(&PROG, "the last word must be `confirm`");
    const char *dev = argv[1];
    udisk_scan(&g_scan);
    struct udisk *d = 0;
    struct udisk_part *p = udisk_part_named(&g_scan, dev, &d);
    if (!p) {
        if (udisk_named(&g_scan, dev)) return uargs_error(&PROG, "%s is a disk, not a partition", dev);
        return uargs_error(&PROG, "no partition %s (`lsblk` lists them)", dev);
    }
    if (!p->number) return uargs_error(&PROG, "%s fills its disk with no partition table", dev);

    struct fsresize_request fi;
    int rc = fs_info(dev, &fi);
    const char *fs = fi.info.fstype[0] ? fi.info.fstype : 0;
    if (rc < 0 && (rc != -ENOTSUP || !fi.info.why[0])) {
        fprintf(stderr, "part: %s cannot be resized -- %s\n", dev, fi.info.why[0] ? fi.info.why : strerror(-rc));
        return 1;
    }

    uint64_t spb = d->block_size > 512 ? d->block_size / 512 : 1;
    uint64_t now = p->count, most = now + udisk_room_after(d, p);
    // A filesystem caps it too: tfs3 where its groups would move, FAT32
    // at its cluster limit.
    int fs_capped = fs && fi.info.max_bytes / 512 < most;
    if (fs_capped) most = fi.info.max_bytes / 512;
    most -= most % spb;
    if (most < now) most = now;
    uint64_t want = parse_new_size(argv[2], now, most);
    if (!want) return uargs_error(&PROG, "SIZE must be like 40G, +10G or max, not '%s'", argv[2]);
    want -= want % spb;

    char snow[24], swant[24], smost[24];
    human_size_iec(snow, sizeof snow, now * 512);
    human_size_iec(swant, sizeof swant, want * 512);
    human_size_iec(smost, sizeof smost, most * 512);
    // Its filesystem may still be short of it: a grow cut off between the
    // two steps. The same command then finishes it.
    // (tfs3 leaves out a last group under 2 MiB, so less than that is no gap.)
    int fs_short = fs && rc == 0 && (fi.info.flags & FS_RESIZE_CAN_GROW) &&
                   fi.info.size_bytes + (2u << 20) + 4096 < now * 512;
    if (want == now && !fs_short) {
        printf("%s is already %s.\n", dev, snow);
        return 0;
    }
    if (rc < 0) {   // -ENOTSUP: a filesystem nothing here resizes
        fprintf(stderr, "part: %s cannot be resized -- %s\n", dev, fi.info.why);
        return 1;
    }
    if (want < now && !(fi.info.flags & FS_RESIZE_CAN_SHRINK)) {
        fprintf(stderr, "part: %s cannot shrink -- %s\n", dev, fi.info.why[0] ? fi.info.why : "not built yet");
        return 1;
    }
    if (want > now && !(fi.info.flags & FS_RESIZE_CAN_GROW)) {
        fprintf(stderr, "part: %s cannot grow -- %s\n", dev, fi.info.why[0] ? fi.info.why : "not built yet");
        return 1;
    }
    if (want > most) {
        fprintf(stderr, "part: %s can grow to %s at most -- %s\n", dev, smost,
                fs_capped ? "its filesystem grows no further" : "that is all the free space after it");
        return 1;
    }
    // A filesystem with no journal to make its grow atomic (FAT32).
    int unsafe = fs && (want > now || fs_short) && (fi.info.flags & FS_RESIZE_UNSAFE);
    if (argc < 4) {
        printf("part: this resizes %s from %s to %s%s%s%s.\n", dev, snow, swant, fs ? "; its " : "",
               fs ? fs : "", fs ? " filesystem follows" : "");
        if (unsafe)
            printf("part: %s keeps no journal -- if this is interrupted while its data moves (a power cut,\n"
                   "      the disk pulled out), the volume is lost. Back it up first.\n", udisk_fs_title(fs));
        printf("part: re-run as `part resize %s %s confirm` if that is what you want.\n", dev, argv[2]);
        return 1;
    }

    if (unsafe)
        printf("part: growing its %s filesystem -- do not power off or pull the disk until this ends.\n",
               udisk_fs_title(fs));
    struct fs_resize_info info;
    char why[FS_RESIZE_WHY_MAX];
    int stage;
    if (udiskops_resize_part(dev, now, want, fs != 0, &info, why, sizeof why, &stage) < 0) {
        if (stage == 1) {
            fprintf(stderr, "part: %s not resized -- %s\n", dev, why);
            return 1;
        }
        fprintf(stderr, "part: %s is %s now, but its %s filesystem did not grow -- %s\n", dev, swant, fs, why);
        fprintf(stderr, "part: `part resize %s %s confirm` again finishes it.\n", dev, argv[2]);
        return 1;
    }
    if (fs) {
        char sfs[24];
        human_size_iec(sfs, sizeof sfs, info.size_bytes);
        printf("Resized %s from %s to %s; its %s filesystem is %s now.\n", dev, snow, swant, fs, sfs);
        return 0;
    }
    printf("Resized %s from %s to %s.\n", dev, snow, swant);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc == 0) return uargs_error(&PROG, "a command is needed");
    if (!strcmp(a.argv[0], "delete")) return do_delete(a.argc, a.argv);
    if (!strcmp(a.argv[0], "resize")) return do_resize(a.argc, a.argv);
    return uargs_error(&PROG, "unknown command '%s'", a.argv[0]);
}
