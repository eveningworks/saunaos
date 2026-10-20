// lsblk -- every block device this boot found, disks and partitions.
//
// WHAT IT IS FOR. Every disk driver runs at boot now and registers what
// it finds into a table (kernel/include/kernel/block.h); the root is a
// SEPARATE choice, made by `root=` on the boot line or by driver
// precedence. This is how a person sees the first half of that -- which
// devices exist, what they are called, and which one the root is on.
//
// The names it prints are the names `root=` and `mount` take, and that
// is the whole point of printing them: `mount ahci0p1 /mnt` is not
// something anyone can guess without this.
//
// WHY IT IS NOT `parttable`. That reads a table off a DISK and reports
// what is written there; this reports what the KERNEL found and named.
// They disagree usefully -- a partition whose window the kernel refused
// appears in one and not the other -- and merging them would lose that.
//
// -f is util-linux's: what filesystem each device holds and its label,
// mounted or not -- the kernel reads the signature (kernel/fs_ident.h).
#include <stdint.h>
#include "rt/sys.h"
#include "lib/human.h"
#include "lib/uargs.h"
#include <stdio.h>
#include <string.h>

#define SECTOR_BYTES 512

static int g_fs;

static const struct uargs_opt OPTS[] = {
    { "fs", 'f', 0, "show each device's filesystem and label", &g_fs, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "lsblk",
    .usage = "[-f]",
    .summary = "List every block device this boot found: disks and their partitions.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    char line[224], size[32];
    int n = 0;
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "unexpected argument '%s'", a.argv[0]);

    if (g_fs) sys_print("NAME        SIZE      FSTYPE   LABEL            MOUNTED\n");
    else sys_print("NAME        SIZE      SECTOR TYPE   ROOT  WHERE\n");

    // The loop ends on the record that is NOT THERE rather than on a
    // count read first: a list's length is itself a fact and can change
    // between reads. Same rule every other list consumer here follows.
    struct query_blkdev d;
    QUERY_FOREACH(QUERY_BLKDEV, d, i) {
        n++;

        human_size(size, sizeof size, d.sectors * (unsigned long long)SECTOR_BYTES);

        // A partition says where on its disk it starts, because that is
        // the number that makes it identifiable against `parttable`'s
        // output; a whole disk has nothing useful to say there.
        char where[64];
        if (d.parent[0])
            snprintf(where, sizeof where, "%s at lba %llu",
                     d.parent, (unsigned long long)d.base_lba);
        else
            snprintf(where, sizeof where, "%s",
                     d.persistent ? "disk" : "memory (not persistent)");

        if (g_fs) {
            // Where it is mounted; else "" -- or, for a filesystem this
            // system cannot mount, that, since it is the next question.
            const char *at = d.fstype[0] && !(d.fs_flags & QUERY_BLK_FS_MOUNTABLE) ? "(cannot mount)" : "";
            struct query_fsinfo f;
            QUERY_FOREACH(QUERY_FSINFO, f, k)
                if ((f.flags & QUERY_FS_MOUNTED) && f.device[0] && !strcmp(f.device, d.name)) {
                    snprintf(where, sizeof where, "%s", f.point);
                    at = where;
                    break;
                }
            snprintf(line, sizeof line, "%-11s %-9s %-8s %-16s %s\n", d.name, size, d.fstype, d.label, at);
            sys_print(line);
            continue;
        }

        // SECTOR is the device's logical block -- 4096 on a 4K-sector
        // disk -- while `lba` above stays in 512-byte units, as the
        // kernel counts everywhere.
        snprintf(line, sizeof line, "%-11s %-9s %-6llu %-6s %-5s %s\n",
                 d.name, size, (unsigned long long)(d.block_size ? d.block_size : 512),
                 d.parent[0] ? "part" : "disk",
                 d.is_root ? "yes" : "", where);
        sys_print(line);
    }

    // Never a silent empty table. A machine with no disk at all is a
    // real and supported state (the root is ramfs then), and saying so
    // is different from printing a header and stopping, which reads
    // like the command failed.
    if (n == 0)
        sys_print("no block devices -- this boot found no disk\n");

    return 0;
}
