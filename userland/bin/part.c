// part -- change ONE partition of a disk: delete it.
//
// The table is edited in place (SYS_PARTEDIT): every other partition
// keeps its bytes, its number and its device name, which is what
// `sfdisk --delete` and diskpart's `delete partition` do. `mkpart`
// writes a whole new table; `parttable` reads one.
//
// The disk is re-read at once, so the change shows in `lsblk` without a
// reboot. The system disk is refused: its table is what the next boot
// finds the root by.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "lib/human.h"
#include "lib/uargs.h"

static const struct uargs_cmd CMDS[] = {
    { "delete", "PARTITION confirm", "remove PARTITION from its disk's table (`lsblk` names it)" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "part",
    .usage = "delete PARTITION confirm",
    .summary = "Change one partition of a disk; the others keep their numbers and names.",
    .cmds = CMDS,
    .notes = "PARTITION must not be mounted, and must not be on the disk the system runs from.\n"
             "Without `confirm` it only says what would go.",
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

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc == 0) return uargs_error(&PROG, "a command is needed");
    if (!strcmp(a.argv[0], "delete")) return do_delete(a.argc, a.argv);
    return uargs_error(&PROG, "unknown command '%s'", a.argv[0]);
}
