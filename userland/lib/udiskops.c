// See udiskops.h.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "mount_abi.h"
#include "lib/human.h"
#include "lib/udiskops.h"
#include "lib/ugrow.h"

#define MIB UDISK_MIB

const char *udiskop_verb(int kind) {
    switch (kind) {
    case UDISKOP_CREATE: return "Create";
    case UDISKOP_DELETE: return "Delete";
    case UDISKOP_RESIZE: return "Resize";
    case UDISKOP_FORMAT: return "Format";
    default:             return "?";
    }
}

static int find(const struct udisk *d, uint64_t start) {
    for (int i = 0; i < d->nparts; i++)
        if (d->parts[i].start == start) return i;
    return -1;
}

static int same_part(const struct udiskop *a, const char *disk, uint64_t start) {
    return a->start == start && !strcmp(a->disk, disk);
}

const char *udiskops_check(const struct udisk *d, const int *marks, const struct udiskop *op) {
    if (d->system) return "The system disk's partitions cannot be changed while it runs";
    if (!d->persistent) return "This disk is in memory";
    if (op->kind == UDISKOP_CREATE) {
        for (int i = 0; i < d->nparts; i++) {
            if (d->parts[i].point[0]) return "Unmount this disk's volumes first";
            if (d->parts[i].role == UDISK_ROLE_OTHER) return "A partition here has a type Disks cannot keep -- use mkpart";
        }
        if (d->nparts >= MKPART_MAX_ENTRIES) return "A table here holds at most four partitions";
        if (op->sectors < MIB) return "A partition needs at least 1 MiB";
        uint64_t first = d->kind == QUERY_PART_MBR ? 1 : UDISK_GPT_TAIL;
        uint64_t limit = d->kind == QUERY_PART_MBR ? d->sectors : d->sectors - UDISK_GPT_TAIL;
        uint64_t end = op->start + op->sectors;
        if (op->start < first || end > limit || end < op->start) return "It runs past the space the table may use";
        for (int i = 0; i < d->nparts; i++) {
            const struct udisk_part *p = &d->parts[i];
            if (op->start < p->start + p->count && p->start < end) return "It overlaps another partition";
        }
        return 0;
    }
    int i = find(d, op->start);
    if (i < 0) return "That partition is not there any more";
    const struct udisk_part *p = &d->parts[i];
    int m = marks ? marks[i] : 0;
    if (p->point[0]) return "Unmount it first";
    if (!(m & UDISKOP_MARK_NEW) && !p->dev[0]) return "This partition has no device until the next boot";
    switch (op->kind) {
    case UDISKOP_DELETE:
        if (!p->number) return "A volume with no partition table cannot be deleted";
        return 0;
    case UDISKOP_RESIZE:
        if (!p->number) return "A volume with no partition table cannot be resized";
        if (p->role != UDISK_ROLE_DATA) return "This partition holds the firmware's boot data";
        if (m & (UDISKOP_MARK_NEW | UDISKOP_MARK_FORMAT))
            return "A pending step creates or formats it: resize it before that step, or remove the step";
        if (op->sectors < MIB) return "A partition needs at least 1 MiB";
        if (op->sectors > p->count + udisk_room_after(d, p)) return "There is not that much free space after it";
        return 0;
    case UDISKOP_FORMAT:
        if (!p->number) return "Select a partition";
        if (p->role != UDISK_ROLE_DATA) return "This partition holds the firmware's boot data";
        if (strcmp(op->fs, "tfs3") && strcmp(op->fs, "fat32")) return "Disks formats tfs3 and FAT32";
        return 0;
    default:
        return "Not a step Disks knows";
    }
}

// `op`, already checked, done to the projection `d`; `m` follows its
// partitions as they move.
static void apply(struct udisk *d, int *m, const struct udiskop *op) {
    if (op->kind == UDISKOP_CREATE) {
        int at = 0;
        while (at < d->nparts && d->parts[at].start < op->start) at++;
        memmove(&d->parts[at + 1], &d->parts[at], (size_t)(d->nparts - at) * sizeof d->parts[0]);
        memmove(&m[at + 1], &m[at], (size_t)(d->nparts - at) * sizeof m[0]);
        d->nparts++;
        struct udisk_part *p = &d->parts[at];
        memset(p, 0, sizeof *p);
        p->start = op->start;
        p->count = op->sectors;
        p->role = UDISK_ROLE_DATA;
        snprintf(p->name, sizeof p->name, "%s", op->name);
        m[at] = UDISKOP_MARK_NEW;
        if (!d->kind) d->kind = QUERY_PART_GPT;
        // SYS_MKPART writes the entries in disk order, so each takes the
        // slot of its place.
        for (int i = 0; i < d->nparts; i++) d->parts[i].number = i + 1;
        return;
    }
    int i = find(d, op->start);
    if (i < 0) return;
    struct udisk_part *p = &d->parts[i];
    switch (op->kind) {
    case UDISKOP_DELETE:
        memmove(&d->parts[i], &d->parts[i + 1], (size_t)(d->nparts - i - 1) * sizeof d->parts[0]);
        memmove(&m[i], &m[i + 1], (size_t)(d->nparts - i - 1) * sizeof m[0]);
        d->nparts--;
        break;
    case UDISKOP_RESIZE:
        if (op->has_fs && p->total && op->sectors > p->count) p->total += (op->sectors - p->count) * 512;
        p->count = op->sectors;
        m[i] |= UDISKOP_MARK_RESIZED;
        break;
    case UDISKOP_FORMAT:
        snprintf(p->type, sizeof p->type, "%s", op->fs);
        p->label[0] = 0;
        p->fs[0] = 0;
        p->used = p->total = 0;
        p->readable = 1;
        m[i] |= UDISKOP_MARK_FORMAT;
        break;
    default:
        break;
    }
}

void udiskops_project(const struct udiskops *q, const struct udisk *real, struct udisk *out, int *marks) {
    int m[UDISK_PARTS_MAX];
    memset(m, 0, sizeof m);
    *out = *real;
    udisk_sort(out);
    for (int i = 0; i < q->count; i++) {
        const struct udiskop *op = &q->ops[i];
        if (strcmp(op->disk, real->name) || udiskops_check(out, m, op)) continue;
        apply(out, m, op);
    }
    if (marks) memcpy(marks, m, sizeof m);
}

// Keep the steps on `real`'s disk that still hold, in order; the number dropped.
static int replay(struct udiskops *q, const struct udisk *real) {
    struct udisk *d = malloc(sizeof *d);
    if (!d) return 0;
    int m[UDISK_PARTS_MAX], keep = 0, dropped = 0;
    memset(m, 0, sizeof m);
    *d = *real;
    udisk_sort(d);
    for (int i = 0; i < q->count; i++) {
        struct udiskop op = q->ops[i];
        if (!strcmp(op.disk, real->name)) {
            if (udiskops_check(d, m, &op)) { dropped++; continue; }
            apply(d, m, &op);
        }
        q->ops[keep++] = op;
    }
    q->count = keep;
    free(d);
    return dropped;
}

static void drop(struct udiskops *q, int i) {
    memmove(&q->ops[i], &q->ops[i + 1], (size_t)(q->count - i - 1) * sizeof q->ops[0]);
    q->count--;
}

// "usb1p2 (Photos, tfs3, 24.0 GiB)", or where a partition a step makes
// will be: it has no device to be named by.
static void part_label(const struct udisk *d, const struct udisk_part *p, int mark, char *out, size_t cap) {
    char size[24], title[sizeof p->label];
    human_size_iec(size, sizeof size, p->count * 512);
    if (mark & UDISKOP_MARK_NEW) {
        char at[24];
        human_size_iec(at, sizeof at, p->start * 512);
        snprintf(out, cap, "The new %s partition at %s on %s", size, at, d->name);
        return;
    }
    udisk_part_title(p, title, sizeof title);
    const char *fs = udisk_part_fs(p);
    snprintf(out, cap, "%s (%s%s%s, %s)", p->dev, title, fs[0] ? ", " : "", fs, size);
}

// `op` as a person reads it, on `d` as the steps before it leave it.
static void describe(const struct udisk *d, const int *m, struct udiskop *op) {
    char label[UDISKOP_WHAT_MAX], a[24], b[24];
    if (op->kind == UDISKOP_CREATE) {
        human_size_iec(a, sizeof a, op->sectors * 512);
        human_size_iec(b, sizeof b, op->start * 512);
        snprintf(op->what, sizeof op->what, "A %s partition at %s on %s%s%s%s", a, b, op->disk,
                 op->name[0] ? ", named \"" : "", op->name, op->name[0] ? "\"" : "");
        return;
    }
    int i = find(d, op->start);
    if (i < 0) return;
    const struct udisk_part *p = &d->parts[i];
    if (op->kind == UDISKOP_RESIZE) {
        char title[sizeof p->label];
        udisk_part_title(p, title, sizeof title);
        human_size_iec(a, sizeof a, p->count * 512);
        human_size_iec(b, sizeof b, op->sectors * 512);
        snprintf(op->what, sizeof op->what, "%s (%s): %s to %s", p->dev, title, a, b);
        return;
    }
    part_label(d, p, m[i], label, sizeof label);
    if (op->kind == UDISKOP_FORMAT)
        snprintf(op->what, sizeof op->what, "%s as %s", label, udisk_fs_title(op->fs));
    else
        snprintf(op->what, sizeof op->what, "%s", label);
}

// Where the steps on the partition at (disk, start) AS IT IS NOW begin: a
// delete there ends one partition, and a create at the same sector after
// it begins another, which the earlier steps never touched.
static int life_begins(const struct udiskops *q, const char *disk, uint64_t start) {
    int from = 0;
    for (int k = 0; k < q->count; k++)
        if (q->ops[k].kind == UDISKOP_DELETE && same_part(&q->ops[k], disk, start)) from = k + 1;
    return from;
}

const char *udiskops_add(struct udiskops *q, const struct udisk *real, struct udiskop *op, int *dropped) {
    if (dropped) *dropped = 0;
    snprintf(op->disk, sizeof op->disk, "%s", real->name);
    // THE QUEUE AS IT WILL BE, built aside: the steps `op` replaces come
    // out first, so it is checked and described against the disk without
    // them, and goes LAST -- a merged step is never left at an old place
    // where the steps after it had not yet made its room.
    struct udiskops n = { 0, 0, 0 };
    if (!UGROW(n.ops, n.cap, q->count + 1)) return "Out of memory";
    int from = op->kind == UDISKOP_CREATE ? q->count : life_begins(q, op->disk, op->start), created = 0;
    for (int k = 0; k < q->count; k++) {
        const struct udiskop *o = &q->ops[k];
        if (k >= from && same_part(o, op->disk, op->start)) {
            created |= o->kind == UDISKOP_CREATE;
            // A second resize or format replaces the first; a delete takes
            // every step the partition had, its create too.
            if (o->kind == op->kind || op->kind == UDISKOP_DELETE) continue;
        }
        n.ops[n.count++] = *o;
    }
    const char *why = 0;
    int append = !(op->kind == UDISKOP_DELETE && created);   // a partition a step made: gone with its steps
    if (append) {
        struct udisk *d = malloc(sizeof *d);
        int m[UDISK_PARTS_MAX];
        if (!d) { free(n.ops); return "Out of memory"; }
        udiskops_project(&n, real, d, m);
        why = udiskops_check(d, m, op);
        int i = find(d, op->start);
        // A resize to the size it already has is no step at all.
        if (!why && op->kind == UDISKOP_RESIZE && i >= 0 && d->parts[i].count == op->sectors) append = 0;
        if (!why && append) describe(d, m, op);
        free(d);
    }
    if (why) { free(n.ops); return why; }
    if (append) n.ops[n.count++] = *op;
    int lost = replay(&n, real);
    if (dropped) *dropped = lost;
    free(q->ops);
    *q = n;
    return 0;
}

int udiskops_revalidate(struct udiskops *q, struct udisk_scan *scan) {
    int dropped = 0;
    for (int i = 0; i < q->count; i++) {
        // One replay a disk, at its first step.
        int seen = 0;
        for (int k = 0; k < i && !seen; k++) seen = !strcmp(q->ops[k].disk, q->ops[i].disk);
        if (seen) continue;
        struct udisk *real = udisk_named(scan, q->ops[i].disk);
        if (real) { dropped += replay(q, real); continue; }
        // The disk is gone, and every step on it.
        char name[16];
        snprintf(name, sizeof name, "%s", q->ops[i].disk);
        for (int k = q->count - 1; k >= i; k--)
            if (!strcmp(q->ops[k].disk, name)) drop(q, k), dropped++;
        i--;
    }
    return dropped;
}

void udiskops_ran(struct udiskops *q, int n) {
    if (n > q->count) n = q->count;
    if (n <= 0) return;
    memmove(&q->ops[0], &q->ops[n], (size_t)(q->count - n) * sizeof q->ops[0]);
    q->count -= n;
}

int udiskops_remove(struct udiskops *q, struct udisk_scan *scan, int i) {
    if (i < 0 || i >= q->count) return 0;
    drop(q, i);
    return udiskops_revalidate(q, scan);
}

int udiskops_count_on(const struct udiskops *q, const char *name) {
    int n = 0;
    for (int i = 0; i < q->count; i++) n += !strcmp(q->ops[i].disk, name);
    return n;
}

void udiskops_clear(struct udiskops *q) { q->count = 0; }

// --- running a step ------------------------------------------------------------

// The whole table rewritten with `add` placed: every other entry as it
// is, in disk order, its role and MBR type kept. A disk with no table
// gets a GPT.
static int write_table(const struct udisk *d, const struct mkpart_entry *add, char *why, size_t cap) {
    struct mkpart_request req;
    memset(&req, 0, sizeof req);
    req.kind = d->kind == QUERY_PART_MBR ? MKPART_KIND_MBR : MKPART_KIND_GPT;
    snprintf(req.device, sizeof req.device, "%s", d->name);
    for (int i = 0; i < d->nparts; i++) {
        const struct udisk_part *p = &d->parts[i];
        if (!p->number) continue;
        if (add && req.count < MKPART_MAX_ENTRIES && add->start_lba < p->start) req.entries[req.count++] = *add, add = 0;
        if (req.count >= MKPART_MAX_ENTRIES) { snprintf(why, cap, "A table here holds at most four partitions"); return -EINVAL; }
        struct mkpart_entry *e = &req.entries[req.count++];
        e->start_lba = p->start;
        e->sectors = p->count;
        e->mbr_type = (uint8_t)p->mbr_type;
        e->role = p->role == UDISK_ROLE_ESP ? MKPART_ROLE_ESP : p->role == UDISK_ROLE_BIOS ? MKPART_ROLE_BIOS_BOOT
                                                                                          : MKPART_ROLE_DATA;
        snprintf(e->name, sizeof e->name, "%s", p->name);
    }
    if (add) {
        if (req.count >= MKPART_MAX_ENTRIES) { snprintf(why, cap, "A table here holds at most four partitions"); return -EINVAL; }
        req.entries[req.count++] = *add;
    }
    int rc = sys_mkpart(&req) < 0 ? -errno : 0;
    if (rc < 0) snprintf(why, cap, "Could not write the table: %s", strerror(-rc));
    return rc;
}

static int run_on(const struct udisk *d, const struct udiskop *op, char *why, size_t cap,
                  void (*started)(void *ctx, const char *dev), void *ctx) {
    if (op->kind == UDISKOP_CREATE) {
        struct mkpart_entry e;
        memset(&e, 0, sizeof e);
        e.start_lba = op->start;
        e.sectors = op->sectors;
        e.role = MKPART_ROLE_DATA;
        snprintf(e.name, sizeof e.name, "%s", op->name);
        if (started) started(ctx, "");
        return write_table(d, &e, why, cap);
    }
    int i = find(d, op->start);
    if (i < 0) { snprintf(why, cap, "The partition is not there any more"); return -ENOENT; }
    const struct udisk_part *p = &d->parts[i];
    if (!p->dev[0]) { snprintf(why, cap, "It has no device yet; it gets one at the next boot"); return -ENODEV; }
    if (p->point[0]) { snprintf(why, cap, "%s is mounted at %s", p->dev, p->point); return -EBUSY; }
    if (started) started(ctx, p->dev);
    int rc = 0;
    if (op->kind == UDISKOP_FORMAT) {
        struct mkfs_request m;
        memset(&m, 0, sizeof m);
        snprintf(m.source, sizeof m.source, "%s", p->dev);
        snprintf(m.fstype, sizeof m.fstype, "%s", op->fs);
        m.flags = MKFS_CONFIRM;
        rc = sys_mkfs(&m) < 0 ? -errno : 0;
        if (rc < 0) snprintf(why, cap, "Could not format %s: %s", p->dev, strerror(-rc));
        return rc;
    }
    if (op->kind == UDISKOP_DELETE) {
        struct partedit_request req;
        memset(&req, 0, sizeof req);
        req.op = PARTEDIT_DELETE;
        req.flags = PARTEDIT_CONFIRM;
        snprintf(req.device, sizeof req.device, "%s", p->dev);
        rc = sys_partedit(&req) < 0 ? -errno : 0;
        if (rc < 0) snprintf(why, cap, "Could not delete %s: %s", p->dev, req.why[0] ? req.why : strerror(-rc));
        return rc;
    }
    char reason[FS_RESIZE_WHY_MAX];
    int stage = 0;
    rc = udiskops_resize_part(p->dev, p->count, op->sectors, op->has_fs, 0, reason, sizeof reason, &stage);
    if (rc < 0)
        snprintf(why, cap, stage == 2 && op->sectors != p->count ? "%s grew, but its filesystem did not: %s"
                                                                  : "Could not resize %s: %s", p->dev, reason);
    return rc;
}

int udiskops_resize_part(const char *dev, uint64_t now, uint64_t want, int has_fs, struct fs_resize_info *info,
                         char *why, size_t cap, int *stage) {
    *stage = 0;
    why[0] = 0;
    if (want != now) {
        struct partedit_request req;
        memset(&req, 0, sizeof req);
        req.op = PARTEDIT_RESIZE;
        req.flags = PARTEDIT_CONFIRM;
        req.sectors = want;
        snprintf(req.device, sizeof req.device, "%s", dev);
        if (sys_partedit(&req) < 0) {
            int rc = -errno;
            *stage = 1;
            snprintf(why, cap, "%s", req.why[0] ? req.why : strerror(-rc));
            return rc;
        }
    }
    if (!has_fs) return 0;
    struct fsresize_request fr;
    memset(&fr, 0, sizeof fr);
    snprintf(fr.source, sizeof fr.source, "%s", dev);
    fr.flags = FSRESIZE_CONFIRM;
    int rc = sys_fs_resize(&fr) < 0 ? -errno : 0;
    if (info) *info = fr.info;
    if (rc < 0) {
        *stage = 2;
        snprintf(why, cap, "%s", fr.info.why[0] ? fr.info.why : strerror(-rc));
    }
    return rc;
}

int udiskops_run(const struct udiskop *op, char *why, size_t cap,
                 void (*started)(void *ctx, const char *dev), void *ctx) {
    struct udisk_scan s;
    memset(&s, 0, sizeof s);
    udisk_scan(&s);
    why[0] = 0;
    int rc;
    struct udisk *d = udisk_named(&s, op->disk);
    if (!d) {
        snprintf(why, cap, "%s is not connected", op->disk);
        rc = -ENODEV;
    } else {
        udisk_sort(d);
        rc = run_on(d, op, why, cap, started, ctx);
    }
    free(s.disks);
    free(s.ram);
    return rc;
}
