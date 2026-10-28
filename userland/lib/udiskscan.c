// See udiskscan.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "lib/udiskscan.h"
#include "lib/ugrow.h"

static int role_of(const struct udisk_part *p, int kind) {
    static const uint8_t BASIC[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                       0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    static const uint8_t BIOS[16]  = { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
                                       0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 };
    static const uint8_t ESP[16]   = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                       0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
    if (kind == QUERY_PART_GPT) {
        if (!memcmp(p->type_guid, BASIC, 16)) return UDISK_ROLE_DATA;
        if (!memcmp(p->type_guid, ESP, 16)) return UDISK_ROLE_ESP;
        if (!memcmp(p->type_guid, BIOS, 16)) return UDISK_ROLE_BIOS;
        return UDISK_ROLE_OTHER;
    }
    return p->mbr_type == 0xEF ? UDISK_ROLE_ESP : UDISK_ROLE_DATA;
}

struct udisk *udisk_named(struct udisk_scan *s, const char *name) {
    for (int i = 0; i < s->ndisks; i++) if (!strcmp(s->disks[i].name, name)) return &s->disks[i];
    return 0;
}

static void set_ident(struct udisk_part *p, const struct query_blkdev *b) {
    snprintf(p->type, sizeof p->type, "%s", b->fstype);
    snprintf(p->label, sizeof p->label, "%s", b->label);
    p->readable = (b->fs_flags & QUERY_BLK_FS_MOUNTABLE) != 0;
}

struct udisk *udisk_add(struct udisk_scan *s) {
    if (!UGROW(s->disks, s->disks_cap, s->ndisks + 1)) {
        fprintf(stderr, "udiskscan: out of memory at disk %d\n", s->ndisks + 1);
        return 0;
    }
    struct udisk *d = &s->disks[s->ndisks++];
    memset(d, 0, sizeof *d);
    return d;
}

void udisk_scan(struct udisk_scan *s) {
    s->ndisks = s->nram = 0;
    struct query_blkdev b;
    QUERY_FOREACH(QUERY_BLKDEV, b, i) {
        if (b.parent[0]) continue;
        struct udisk *d = udisk_add(s);
        if (!d) continue;
        snprintf(d->name, sizeof d->name, "%s", b.name);
        snprintf(d->model, sizeof d->model, "%s", b.model);
        snprintf(d->driver, sizeof d->driver, "%s", b.driver);
        d->sectors = b.sectors;
        d->block_size = b.block_size ? b.block_size : 512;
        d->persistent = (int)b.persistent;
        d->system = (int)b.is_root;
    }
    struct query_parttable t;
    QUERY_FOREACH(QUERY_PARTTABLE, t, i) {
        struct udisk *d = udisk_named(s, t.disk);
        if (!d) continue;
        d->kind = (int)t.kind;
        memcpy(d->guid, t.disk_guid, 16);
    }
    struct query_partition q;
    QUERY_FOREACH(QUERY_PARTITION, q, i) {
        struct udisk *d = udisk_named(s, q.disk);
        if (!d || d->nparts >= UDISK_PARTS_MAX || !q.lba_count) continue;
        struct udisk_part *p = &d->parts[d->nparts++];
        memset(p, 0, sizeof *p);
        p->number = (int)q.number;
        p->start = q.lba_start;
        p->count = q.lba_count;
        p->mbr_type = (int)q.mbr_type;
        memcpy(p->type_guid, q.type_guid, 16);
        snprintf(p->name, sizeof p->name, "%s", q.name);
        p->role = role_of(p, d->kind);
    }
    // Each partition's block device: the window on that disk at that LBA.
    QUERY_FOREACH(QUERY_BLKDEV, b, i) {
        struct udisk *d = b.parent[0] ? udisk_named(s, b.parent) : 0;
        for (int k = 0; d && k < d->nparts; k++)
            if (d->parts[k].start == b.base_lba) {
                snprintf(d->parts[k].dev, sizeof d->parts[k].dev, "%s", b.name);
                set_ident(&d->parts[k], &b);
                if (b.is_root) d->system = 1;
            }
        // A whole disk with no table that holds a filesystem is one volume,
        // mounted or not (below adds the mounted one).
        struct udisk *w = !b.parent[0] && b.fstype[0] ? udisk_named(s, b.name) : 0;
        if (w && !w->kind && !w->nparts) {
            struct udisk_part *p = &w->parts[w->nparts++];
            memset(p, 0, sizeof *p);
            p->count = w->sectors;
            snprintf(p->dev, sizeof p->dev, "%s", w->name);
            set_ident(p, &b);
        }
    }
    // What is mounted where: on a partition, on a whole disk with no
    // table (a stock disk.img), or on nothing at all (ramfs).
    struct query_fsinfo f;
    QUERY_FOREACH(QUERY_FSINFO, f, i) {
        if (!(f.flags & QUERY_FS_MOUNTED)) continue;
        struct udisk_part *p = 0;
        struct udisk *whole = udisk_named(s, f.device);
        for (int k = 0; k < s->ndisks && !p; k++)
            for (int j = 0; j < s->disks[k].nparts && !p; j++)
                if (f.device[0] && !strcmp(s->disks[k].parts[j].dev, f.device)) p = &s->disks[k].parts[j];
        if (!p && whole && whole->nparts < UDISK_PARTS_MAX && !whole->kind) {
            p = &whole->parts[whole->nparts++];
            memset(p, 0, sizeof *p);
            p->count = whole->sectors;
            snprintf(p->dev, sizeof p->dev, "%s", whole->name);
        }
        if (p) {
            snprintf(p->point, sizeof p->point, "%s", f.point);
            snprintf(p->fs, sizeof p->fs, "%s", f.name);
            p->used = f.used_bytes;
            p->total = f.total_bytes;
            p->root = (f.flags & QUERY_FS_ROOT) != 0;
        } else if (!f.device[0] && UGROW(s->ram, s->ram_cap, s->nram + 1)) {
            struct udisk_ram *r = &s->ram[s->nram++];
            snprintf(r->point, sizeof r->point, "%s", f.point);
            snprintf(r->fs, sizeof r->fs, "%s", f.name);
            r->used = f.used_bytes;
            r->total = f.total_bytes;
        }
    }
    // The system disk first, the way a list reads best.
    for (int i = 1; i < s->ndisks; i++)
        if (s->disks[i].system) {
            static struct udisk tmp;   // a few KB: never on the stack
            tmp = s->disks[0];
            s->disks[0] = s->disks[i];
            s->disks[i] = tmp;
        }
}

int udisk_live_root(void) {
    struct query_blkdev b;
    QUERY_FOREACH(QUERY_BLKDEV, b, i)
        if (b.is_root) return !b.persistent;
    return 0;
}

static int by_start(const void *a, const void *b) {
    const struct udisk_part *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

void udisk_sort(struct udisk *d) {
    qsort(d->parts, (size_t)d->nparts, sizeof d->parts[0], by_start);
}

const char *udisk_role_name(const struct udisk_part *p) {
    switch (p->role) {
    case UDISK_ROLE_ESP:  return "EFI";
    case UDISK_ROLE_BIOS: return "BIOS boot";
    default:              return 0;
    }
}

void udisk_part_title(const struct udisk_part *p, char *out, size_t cap) {
    if (p->label[0]) snprintf(out, cap, "%s", p->label);
    else udisk_part_name(p, out, cap);
}

const char *udisk_fs_title(const char *type) {
    static const struct { const char *id, *title; } names[] = {
        { "ntfs", "NTFS" },       { "exfat", "exFAT" },     { "fat32", "FAT32" },
        { "fat16", "FAT16" },     { "fat12", "FAT12" },     { "btrfs", "Btrfs" },
        { "xfs", "XFS" },         { "swap", "Linux swap" }, { "iso9660", "ISO 9660" },
        { "hfsplus", "HFS+" },    { "apfs", "APFS" },       { "crypto_LUKS", "LUKS" },
        { "refs", "ReFS" },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(type, names[i].id)) return names[i].title;
    return type;
}

const char *udisk_part_fs(const struct udisk_part *p) {
    return udisk_fs_title(p->fs[0] ? p->fs : p->type);
}

void udisk_part_name(const struct udisk_part *p, char *out, size_t cap) {
    if (p->name[0]) snprintf(out, cap, "%s", p->name);
    else if (udisk_role_name(p)) snprintf(out, cap, "%s", udisk_role_name(p));
    else if (p->number) snprintf(out, cap, "Partition %d", p->number);
    else snprintf(out, cap, "Whole disk");
}
