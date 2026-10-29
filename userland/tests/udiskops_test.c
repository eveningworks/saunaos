// Disks' pending changes (lib/udiskops.h) on fixture disks: what a step
// may follow, how steps merge, and what goes when one is removed. No
// step is RUN here -- tools/disks_test.py and disks_resize_test.py apply
// them to a real disk.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - A check against the disk as it IS, not as the steps leave it,
//     passes every single-step case. "Delete p2, then grow p1 into it"
//     is refused before the delete and allowed after it, and removing
//     the delete takes the grow with it.
//   - A projection that appends a new partition instead of placing it
//     would draw right with one partition and number wrong with two:
//     the order and the slots are compared.
//   - Merging tested only by the count would pass with the FIRST resize
//     kept: the size the projection shows is compared.
//   - The fixtures are named nothing a test machine has.
#include <stdio.h>
#include <string.h>
#include "query_abi.h"
#include "lib/udiskops.h"
#include "lib/utest.h"

#define MIB 2048ull   // sectors
#define GIB (1024 * MIB)

static struct udisk_scan g_scan;
static struct udiskops g_q;
static struct udisk g_v;
static int g_m[UDISK_PARTS_MAX];

static struct udisk_part *part(struct udisk *d, uint64_t start, uint64_t count, const char *dev, const char *fs) {
    struct udisk_part *p = &d->parts[d->nparts++];
    memset(p, 0, sizeof *p);
    p->number = d->nparts;
    p->start = start;
    p->count = count;
    p->role = UDISK_ROLE_DATA;
    snprintf(p->dev, sizeof p->dev, "%s", dev);
    snprintf(p->type, sizeof p->type, "%s", fs);
    return p;
}

static struct udisk *disk(const char *name, uint64_t sectors) {
    struct udisk *d = udisk_add(&g_scan);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->sectors = sectors;
    d->block_size = 512;
    d->kind = QUERY_PART_GPT;
    d->persistent = 1;
    return d;
}

static struct udiskop op(int kind, uint64_t start, uint64_t sectors) {
    struct udiskop o;
    memset(&o, 0, sizeof o);
    o.kind = kind;
    o.start = start;
    o.sectors = sectors;
    if (kind == UDISKOP_FORMAT) snprintf(o.fs, sizeof o.fs, "fat32");
    return o;
}

static const char *add(const char *name, struct udiskop o) {
    return udiskops_add(&g_q, udisk_named(&g_scan, name), &o, 0);
}

static void view(const char *name) { udiskops_project(&g_q, udisk_named(&g_scan, name), &g_v, g_m); }

int main(void) {
    utest_begin("udiskops_test", "Disks' pending changes: order, merging, removal", 0);

    // fixq0: p1 8 GiB at 1 MiB, p2 24 GiB right after it, then free space.
    struct udisk *d = disk("fixq0", 64 * GIB);
    const uint64_t P1 = MIB, P2 = MIB + 8 * GIB;
    part(d, P1, 8 * GIB, "fixq0p1", "fat32");
    part(d, P2, 24 * GIB, "fixq0p2", "tfs3");
    const uint64_t END = 64 * GIB - UDISK_GPT_TAIL;

    // --- a grow, a create in the rest, a format of the new one --------------
    utest_check(!add("fixq0", op(UDISKOP_RESIZE, P2, 36 * GIB)), "p2 grows into the free space after it");
    const uint64_t P3 = P2 + 36 * GIB;
    utest_check(!add("fixq0", op(UDISKOP_CREATE, P3, END - P3)), "a new partition takes the rest");
    utest_check(!add("fixq0", op(UDISKOP_FORMAT, P3, 0)), "the new partition is formatted, before it exists");
    view("fixq0");
    utest_check(g_v.nparts == 3 && g_v.parts[1].count == 36 * GIB && g_v.parts[2].start == P3,
                "the projection: p2 at 36 GiB, the new one third");
    utest_check(g_m[0] == 0 && g_m[1] == UDISKOP_MARK_RESIZED && g_m[2] == (UDISKOP_MARK_NEW | UDISKOP_MARK_FORMAT),
                "marks: p1 untouched, p2 resized, the third new and formatted");
    utest_check(g_v.parts[2].number == 3 && !strcmp(g_v.parts[2].type, "fat32"), "it is slot 3, holding FAT32");
    utest_check(strstr(g_q.ops[2].what, "new") && strstr(g_q.ops[2].what, "FAT32"),
                "the format names the new partition by where it will be, not a device it lacks");
    utest_check(strstr(g_q.ops[0].what, "fixq0p2") && strstr(g_q.ops[0].what, "36.0 GiB"),
                "the resize names the device and the size it goes to");

    // --- each step checked against the disk as the steps leave it ------------
    utest_check(add("fixq0", op(UDISKOP_CREATE, P3 + GIB, GIB)) != 0, "a create over the queued one is refused");
    utest_check(add("fixq0", op(UDISKOP_RESIZE, P2, 40 * GIB)) != 0, "p2 cannot grow into the queued partition");
    utest_check(add("fixq0", op(UDISKOP_RESIZE, P3, 4 * GIB)) != 0,
                "a partition a step creates and formats is not resized after that");

    // --- merging -------------------------------------------------------------
    utest_check(!add("fixq0", op(UDISKOP_RESIZE, P2, 30 * GIB)) && g_q.count == 3,
                "a second resize of p2 replaces the first: still three steps");
    view("fixq0");
    utest_check(g_v.parts[1].count == 30 * GIB, "...and the projection has the second size, not the first");
    utest_check(!add("fixq0", op(UDISKOP_RESIZE, P2, 24 * GIB)) && g_q.count == 2 && g_q.ops[0].kind == UDISKOP_CREATE,
                "resized back to the size it has: the step goes");

    // --- removal takes what depended on it -----------------------------------
    utest_check(udiskops_remove(&g_q, &g_scan, 0) == 1 && g_q.count == 0,
                "removing the create removes the format of what it made");
    utest_check(!add("fixq0", op(UDISKOP_CREATE, P3, GIB)) && !add("fixq0", op(UDISKOP_FORMAT, P3, 0)) &&
                !add("fixq0", op(UDISKOP_DELETE, P3, 0)) && g_q.count == 0,
                "deleting a partition a step creates removes its steps and queues nothing");

    utest_check(add("fixq0", op(UDISKOP_RESIZE, P1, 20 * GIB)) != 0, "p1 cannot grow: p2 is right after it");
    utest_check(!add("fixq0", op(UDISKOP_DELETE, P2, 0)), "p2 is deleted");
    utest_check(!add("fixq0", op(UDISKOP_RESIZE, P1, 20 * GIB)), "...and then p1 grows into its room");
    utest_check(add("fixq0", op(UDISKOP_FORMAT, P2, 0)) != 0, "a deleted partition takes no more steps");
    utest_check(udiskops_remove(&g_q, &g_scan, 0) == 1 && g_q.count == 0,
                "removing the delete removes the grow that needed its room");
    utest_check(!add("fixq0", op(UDISKOP_RESIZE, P2, 30 * GIB)) && !add("fixq0", op(UDISKOP_DELETE, P2, 0)) &&
                g_q.count == 1 && g_q.ops[0].kind == UDISKOP_DELETE,
                "a delete takes the partition's earlier steps with it");
    udiskops_clear(&g_q);

    // --- the review's cases (2026-10-29) --------------------------------------
    // fixq4: p1 4 GiB, a 2 GiB gap, p2 4 GiB, free after.
    struct udisk *g = disk("fixq4", 32 * GIB);
    const uint64_t G1 = MIB, G2 = MIB + 6 * GIB;
    part(g, G1, 4 * GIB, "fixq4p1", "tfs3");
    part(g, G2, 4 * GIB, "fixq4p2", "tfs3");
    utest_check(!add("fixq4", op(UDISKOP_RESIZE, G1, 6 * GIB)) && !add("fixq4", op(UDISKOP_DELETE, G2, 0)) &&
                !add("fixq4", op(UDISKOP_RESIZE, G1, 10 * GIB)),
                "p1 grown into the gap, p2 deleted, then p1 grown again into p2's room");
    view("fixq4");
    utest_check(g_q.count == 2 && g_q.ops[1].kind == UDISKOP_RESIZE && g_v.parts[0].count == 10 * GIB,
                "the merged resize goes after the delete that made its room, and holds (it was dropped in place)");
    utest_check(strstr(g_q.ops[1].what, "4.0 GiB to 10.0 GiB") != 0,
                "it says the size it really goes from, not the replaced step's");
    udiskops_clear(&g_q);

    utest_check(!add("fixq4", op(UDISKOP_DELETE, G2, 0)) && !add("fixq4", op(UDISKOP_CREATE, G2, 2 * GIB)) &&
                !add("fixq4", op(UDISKOP_DELETE, G2, 0)),
                "p2 deleted, a new partition at the same sector, and that one deleted");
    utest_check(g_q.count == 1 && g_q.ops[0].kind == UDISKOP_DELETE,
                "only the new partition's steps went: p2 is still to be deleted");
    udiskops_clear(&g_q);

    int lost = -1;
    struct udiskop sh = op(UDISKOP_RESIZE, G2, 2 * GIB);   // no filesystem said: the table alone
    utest_check(!udiskops_add(&g_q, udisk_named(&g_scan, "fixq4"), &sh, 0) &&
                !add("fixq4", op(UDISKOP_CREATE, G2 + 2 * GIB, GIB)), "p2 shrunk, a partition in what it gave up");
    struct udiskop del = op(UDISKOP_DELETE, G2, 0);
    utest_check(!udiskops_add(&g_q, udisk_named(&g_scan, "fixq4"), &del, &lost) && lost == 1,
                "deleting p2 takes its shrink, and the create that needed it is counted, not lost silently");

    udiskops_clear(&g_q);
    utest_check(!add("fixq4", op(UDISKOP_CREATE, 20 * GIB, GIB)) && !add("fixq4", op(UDISKOP_FORMAT, 20 * GIB, 0)) &&
                !add("fixq4", op(UDISKOP_RESIZE, G1, 6 * GIB)), "a create, its format and a grow queued");
    udiskops_ran(&g_q, 2);
    utest_check(g_q.count == 1 && g_q.ops[0].kind == UDISKOP_RESIZE,
                "the two that ran come off, and the grow is kept without a replay against the old disk");
    udiskops_clear(&g_q);

    struct udisk_part *lp = &udisk_named(&g_scan, "fixq4")->parts[0];
    memset(lp->label, 'L', sizeof lp->label - 1);
    lp->label[sizeof lp->label - 1] = 0;
    utest_check(!add("fixq4", op(UDISKOP_FORMAT, G1, 0)) && strstr(g_q.ops[0].what, " as FAT32") != 0,
                "a longest label still leaves the step's whole text");
    lp->label[0] = 0;
    udiskops_clear(&g_q);

    // --- what the machine allows --------------------------------------------
    struct udisk *m = disk("fixq1", 16 * GIB);
    struct udisk_part *mp = part(m, MIB, 4 * GIB, "fixq1p1", "tfs3");
    part(m, MIB + 4 * GIB, 4 * GIB, "fixq1p2", "tfs3");
    utest_check(!add("fixq1", op(UDISKOP_RESIZE, MIB + 4 * GIB, 6 * GIB)), "fixq1p2 grows");
    snprintf(mp->point, sizeof mp->point, "/mnt/fixq1p1");
    utest_check(add("fixq1", op(UDISKOP_DELETE, MIB, 0)) != 0, "a mounted partition is not deleted");
    utest_check(add("fixq1", op(UDISKOP_CREATE, 12 * GIB, GIB)) != 0,
                "nothing is created while the disk has a volume mounted: the table is rewritten whole");
    utest_check(udiskops_revalidate(&g_q, &g_scan) == 0 && g_q.count == 1,
                "a sibling mounted later does not stop a resize");
    snprintf(udisk_named(&g_scan, "fixq1")->parts[1].point, sizeof mp->point, "/mnt/fixq1p2");
    utest_check(udiskops_revalidate(&g_q, &g_scan) == 1 && g_q.count == 0,
                "the partition itself mounted after it was queued: the step goes");

    struct udisk *sys = disk("fixq2", 16 * GIB);
    sys->system = 1;
    utest_check(add("fixq2", op(UDISKOP_CREATE, MIB, GIB)) != 0, "the system disk takes no steps");

    struct udisk *full = disk("fixq3", 16 * GIB);
    for (int i = 0; i < MKPART_MAX_ENTRIES; i++) part(full, MIB + (uint64_t)i * GIB, GIB, "", "");
    for (int i = 0; i < full->nparts; i++) snprintf(full->parts[i].dev, sizeof full->parts[i].dev, "fixq3p%d", i + 1);
    utest_check(add("fixq3", op(UDISKOP_CREATE, 8 * GIB, GIB)) != 0, "a table already holding four takes no fifth");
    utest_check(!add("fixq3", op(UDISKOP_DELETE, MIB, 0)) && !add("fixq3", op(UDISKOP_CREATE, 8 * GIB, GIB)),
                "...until a delete makes room for it");
    view("fixq3");
    utest_check(g_v.nparts == 4 && g_v.parts[3].start == 8 * GIB && g_v.parts[0].start == MIB + GIB,
                "the projection in disk order: the first gone, the new one last");

    return utest_end();
}
