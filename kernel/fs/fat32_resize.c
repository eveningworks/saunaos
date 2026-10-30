// FAT32: growing or shrinking a volume nothing has mounted -- the
// filesystem half of a partition resize (SYS_FS_RESIZE): after the table
// has grown, or before it shrinks.
// fat32_internal.h has the map; libparted's fat/resize.c and fatresize
// are what real systems use for the same job.
//
// TWO WAYS TO GROW, chosen by fat32_grow_plan():
//  * THE FATS ALREADY COVER THE NEW CLUSTERS. A FAT is a whole number of
//    sectors, so its last sector usually has room for entries past the
//    volume's last cluster (and another formatter may leave more). Then
//    the grow is the BPB's total, FSInfo's free count and those entries
//    zeroed -- nothing moves.
//  * THE FATS MUST GROW. The data region starts right after them, so it
//    SHIFTS by the growth times the number of FATs. A cluster's number
//    is its distance from the data start, so no FAT entry and no
//    directory entry changes: only the clusters IN USE are copied (the
//    FAT says which -- a directory is clusters too), back to front,
//    since each one moves up over a higher one's old place. The cost is
//    the used data, not the partition.
//
// FAT32 HAS NO JOURNAL, SO THE SHIFT IS NOT CRASH-SAFE. From the first
// copied cluster that lands on an old one in use until the boot sector
// names the new layout, neither the old geometry nor the new one reads
// every file right; libparted's FAT resize has the same exposure.
// `part` and Disks say so before they ask (FS_RESIZE_UNSAFE). The order
// keeps that window to the copy and the FAT writes: the data, then the
// FATs, then the boot sector and FSInfo, then their backups. Where each
// crash lands is said at each step in grow().
//
// ONE WAY TO SHRINK (shrink()): the FATs and the data start stay, the
// end comes in. Every cluster in use past the new end is copied to a free
// one below it, and everything naming it is patched: the FAT links into
// it, its own FAT entry at its new number, the directory entry or "."/".."
// starting at it, BPB_RootClus. libparted's and fatresize's shrink do the
// same relocation. The FATs keep their size -- entries past the new last
// cluster are zeroed and never read. Not crash-safe either: once the
// first directory is patched, neither layout reads every file.
//
// THE SMALLEST SIZE is the used clusters, and never under 65525 for a
// volume that has at least that many: fewer makes it FAT16 by Microsoft's
// definition, which every other system's mount follows.
//
// REFUSED rather than done: a volume whose FATs are not mirrored (only
// one is current, so copying FAT 0 could copy a stale one), one marked
// dirty (another system did not finish with it -- check it first), a
// shift over clusters marked bad (a mark names a place on the media,
// which the shift would hand to another cluster), a shrink below the
// smallest size, and a size past FAT32's cluster limit or its 32-bit
// sector count.

#include "fat32_internal.h"
#include "errno.h"
#include "heap.h"
#include "kfmt.h"
#include "klog.h"
#include "string.h"

// The copy buffer, in 512-byte sectors: a multiple of every block size.
#define MOVE_SECTORS 256u

// What a grow reports to Disks' stage dialog and `part` (fs_format_live()).
static const struct fsck_stage_desc GROW_STAGES[] = {
    { "Move the data", "clusters" },
    { "Write the allocation tables", "sectors" },
    { "Switch to the new size", "steps" },
};

// What a shrink reports.
static const struct fsck_stage_desc SHRINK_STAGES[] = {
    { "Move the data", "clusters" },
    { "Point the folders at it", "folders" },
    { "Write the allocation tables", "sectors" },
    { "Switch to the new size", "steps" },
};

// The fewest clusters FAT32 is FAT32 at (Microsoft's FAT type rule).
#define FAT32_MIN_CLUSTERS 65525u

static void why(struct fs_resize_info *out, const char *s) { k_strlcpy(out->why, s, sizeof out->why); }

// ---- the plan -----------------------------------------------------------------

int fat32_grow_plan(const struct fat_vol *v, uint64_t want, struct fat_grow_plan *p) {
    const uint64_t k = v->k ? v->k : 1, spc = v->sectors_per_cluster, R = v->reserved, N = v->num_fats;
    const uint64_t per_fat_sector = SECTOR * k / 4;   // entries in one FAT sector
    k_memset(p, 0, sizeof *p);
    if (!spc || !N) return 0;
    if (want > 0xFFFFFFFFull) want = 0xFFFFFFFFull;   // BPB_TotSec32, in 512s since fat32_parse_bpb()
    want -= want % k;

    // In place: as many clusters as the FATs already have entries for.
    uint64_t c1 = want > v->first_data_sector ? (want - v->first_data_sector) / spc : 0;
    uint64_t holds = (uint64_t)v->fat_sectors / k * per_fat_sector - 2;
    if (c1 > holds) c1 = holds;
    if (c1 > FAT32_MAX_CLUSTERS) c1 = FAT32_MAX_CLUSTERS;

    // Bigger FATs: the smallest that describes what is left after them.
    // Growing a FAT only removes clusters, so one pass settles it; the
    // loop is a bound, not a search.
    uint64_t f2 = v->fat_sectors, c2 = 0;
    for (int pass = 0; pass < 4; pass++) {
        if (R + N * f2 + spc > want) { c2 = 0; break; }
        c2 = (want - R - N * f2) / spc;
        if (c2 > FAT32_MAX_CLUSTERS) c2 = FAT32_MAX_CLUSTERS;
        uint64_t need = (c2 + 2 + per_fat_sector - 1) / per_fat_sector * k;
        if (need <= f2) break;
        f2 = need;
    }
    // The shift keeps the data start's alignment, to the cluster or 4 KiB
    // whichever is less: mkfs.fat aligns clusters, and a flash page or a
    // 4K-sector disk splits every cluster a shift misaligns.
    if (f2 > v->fat_sectors) {
        uint64_t align = spc < 8 ? spc : 8;
        while ((N * (f2 - v->fat_sectors)) % align) f2 += k;
        c2 = R + N * f2 + spc <= want ? (want - R - N * f2) / spc : 0;
        if (c2 > FAT32_MAX_CLUSTERS) c2 = FAT32_MAX_CLUSTERS;
    }

    int moves = f2 > v->fat_sectors && c2 > c1;
    uint64_t c = moves ? c2 : c1, fat = moves ? f2 : v->fat_sectors;
    uint64_t data = R + N * fat;
    if (c <= v->cluster_count) return 0;   // no room gained
    p->moves = moves;
    p->fat_sectors = (uint32_t)fat;
    p->first_data = (uint32_t)data;
    p->clusters = (uint32_t)c;
    // The partition's end when it is all clusters (and the slack under
    // one); the last cluster's end when a limit stopped short of it.
    p->total = (uint32_t)((want - data) / spc == c ? want : data + c * spc);
    return 1;
}

// ---- I/O --------------------------------------------------------------------

// `n` sectors from `lba`, in transfers the device takes.
static int xfer(const struct block_device *dev, uint64_t lba, uint32_t n, void *buf, int write) {
    uint32_t k = blkdev_block_sectors(dev);
    uint32_t most = (uint32_t)blkdev_max_sectors_per_xfer(dev);
    most -= most % k;
    if (!most) most = k;
    uint8_t *p = buf;
    while (n) {
        uint32_t c = n < most ? n : most;
        int ok = write ? blkdev_write_sectors(dev, lba, (int)c, p) : blkdev_read_sectors(dev, lba, (int)c, p);
        if (!ok) return 0;
        lba += c;
        p += (uint64_t)c * SECTOR;
        n -= c;
    }
    return 1;
}

// ---- what the FAT holds ---------------------------------------------------------

struct census {
    uint8_t *used;          // a bit per cluster in use (NULL: only count)
    uint32_t free, bad, in_use;
};

static int census_entry(void *ctx, uint32_t c, uint32_t v) {
    struct census *s = ctx;
    if (v == FAT_FREE) { s->free++; return 0; }
    if (v == FAT_BAD) s->bad++;
    s->in_use++;
    if (s->used) s->used[c >> 3] |= (uint8_t)(1u << (c & 7));
    return 0;
}

static int used_bit(const uint8_t *bm, uint32_t c) { return (bm[c >> 3] >> (c & 7)) & 1; }

// ---- the FATs -------------------------------------------------------------------

// Entries past the old last cluster are zeroed in what is written: they
// name clusters the old volume did not have, and the new one reads them
// as allocated otherwise. Another formatter need not have zeroed them.
static void clear_past(uint8_t *buf, uint32_t first_sector, uint32_t n, uint32_t last_cluster) {
    uint32_t per = SECTOR / 4;
    for (uint32_t i = 0; i < n * per; i++) {
        uint32_t e = first_sector * per + i;
        if (e > last_cluster) wr32(buf + (uint64_t)i * 4, 0);
    }
}

// Every copy of the FAT at its NEW place (copy i at reserved + i * nfat),
// from FAT 0 -- which nothing here writes until the last copy, and whose
// old sectors no new copy overlaps (copy 1 starts at or past its old end).
// Copies 1 and up are written whole when the FATs move, else from the
// sector holding the first new entry; the sectors past the old size are
// zeroed. Highest copy first, so FAT 0 is the last thing changed.
static int write_fats(struct fat32_state *sbi, uint32_t nfat, uint8_t *buf, uint32_t *done) {
    const struct block_device *dev = sbi->v.dev;
    struct fs_check_live *live = fs_format_live();
    const uint32_t k = sbi->v.k, R = sbi->v.reserved, F = sbi->v.fat_sectors, last = sbi->v.cluster_count + 1;
    uint32_t tail = (last + 1) * 4 / SECTOR;          // the sector holding the first new entry
    tail -= tail % k;
    for (int i = (int)sbi->v.num_fats - 1; i >= 0; i--) {
        uint32_t from = (nfat > F && i > 0) ? 0 : tail;
        uint64_t to_base = (uint64_t)R + (uint64_t)i * nfat;
        for (uint32_t s = from; s < F; ) {
            uint32_t n = F - s < MOVE_SECTORS ? F - s : MOVE_SECTORS;
            if (!xfer(dev, R + s, n, buf, 0)) return 0;
            clear_past(buf, s, n, last);
            if (!xfer(dev, to_base + s, n, buf, 1)) return 0;
            s += n;
            *done += n;
            fsck_step(live, *done, 0);
        }
        if (nfat > F) {
            k_memset(buf, 0, MOVE_SECTORS * SECTOR);
            for (uint32_t s = F; s < nfat; ) {
                uint32_t n = nfat - s < MOVE_SECTORS ? nfat - s : MOVE_SECTORS;
                if (!xfer(dev, to_base + s, n, buf, 1)) return 0;
                s += n;
                *done += n;
                fsck_step(live, *done, 0);
            }
        }
    }
    return 1;
}

// ---- the data -------------------------------------------------------------------

// Every cluster in use moves up by `shift` sectors, runs of them at a
// time, back to front: a destination is above its source, so each write
// lands on space whose clusters have already moved or were free.
static int move_data(struct fat32_state *sbi, const uint8_t *used, uint64_t shift, uint8_t *buf) {
    const struct block_device *dev = sbi->v.dev;
    struct fs_check_live *live = fs_format_live();
    const uint64_t D = sbi->v.first_data_sector, spc = sbi->v.sectors_per_cluster;
    uint32_t moved = 0;
    uint32_t c = sbi->v.cluster_count + 1;
    while (c >= 2) {
        if (!used_bit(used, c)) { c--; continue; }
        uint32_t hi = c;
        while (c >= 2 && used_bit(used, c)) c--;
        uint32_t lo = c + 1;
        uint64_t start = D + (uint64_t)(lo - 2) * spc, end = D + (uint64_t)(hi - 1) * spc;
        while (end > start) {
            uint64_t n = end - start < MOVE_SECTORS ? end - start : MOVE_SECTORS;
            uint64_t from = end - n;
            if (!xfer(dev, from, (uint32_t)n, buf, 0)) return 0;
            if (!xfer(dev, from + shift, (uint32_t)n, buf, 1)) return 0;
            end = from;
            moved += (uint32_t)(n / spc ? n / spc : 1);
            fsck_step(live, moved, 0);
        }
    }
    return 1;
}

// ---- the switch -----------------------------------------------------------------

// The boot sector with the new size and FAT size, at `lba` -- the primary
// at 0, or its backup (written from the primary's bytes, so the two agree
// byte for byte, as fsck.fat compares them).
static int write_boot(struct fat32_state *sbi, const uint8_t *boot, uint64_t lba) {
    return blkdev_write_partial(sbi->v.dev, lba, 1, boot);
}

// FSInfo at `lba`, its free count set; left alone when it is not one.
static int write_fsinfo(struct fat32_state *sbi, uint64_t lba, uint32_t free_count) {
    uint8_t *t = sbi->tmpsec;
    if (!blkdev_read_partial(sbi->v.dev, lba, 1, t)) return 0;
    if (rd32(t) != 0x41615252u || rd32(t + 484) != 0x61417272u) return 1;   // "RRaA", "rrAa"
    wr32(t + 488, free_count);
    return blkdev_write_partial(sbi->v.dev, lba, 1, t);
}

static int grow(struct fat32_state *sbi, const struct fat_grow_plan *p, struct fs_resize_info *out) {
    const struct block_device *dev = sbi->v.dev;
    struct fs_check_live *live = fs_format_live();
    const uint32_t k = sbi->v.k, F = sbi->v.fat_sectors, N = sbi->v.num_fats;
    const uint32_t nclusters = sbi->v.cluster_count + 2;
    uint8_t boot[SECTOR];
    struct census cs = { 0, 0, 0, 0 };
    uint32_t bk, free_new, done = 0;
    uint8_t *buf = kmalloc(MOVE_SECTORS * SECTOR);
    int rc = -EIO;
    if (!buf) { why(out, "out of memory"); return -ENOMEM; }
    if (p->moves) {
        cs.used = kzalloc((nclusters + 7) / 8);
        if (!cs.used) { why(out, "out of memory"); rc = -ENOMEM; goto out; }
    }
    if (fat32_scan(sbi, census_entry, &cs) != 0) { why(out, "its allocation table cannot be read"); goto out; }
    if (p->moves && cs.bad) {
        k_snprintf(out->why, sizeof out->why,
                   "%u of its clusters are marked bad, and growing its FATs would move the data over them", cs.bad);
        rc = -ENOTSUP;
        goto out;
    }
    if (!blkdev_read_partial(dev, 0, 1, boot)) { why(out, "its boot sector cannot be read"); goto out; }
    bk = rd16(boot + 50);
    // The new fields, in the volume's own sectors.
    wr32(boot + 32, p->total / k);
    wr32(boot + 36, p->fat_sectors / k);
    free_new = cs.free + (p->clusters - sbi->v.cluster_count);

    if (p->moves) {
        // ---- 1. THE DATA, back to front. CRASH HERE: the boot sector
        // still names the OLD layout, and any old cluster in use that a
        // lower one's copy has landed on reads the wrong bytes -- the
        // volume is CORRUPT. The window opens at the first such landing
        // (the topmost runs often move into free space first).
        fsck_stage(live, 0, cs.in_use);
        if (!move_data(sbi, cs.used, (uint64_t)N * (p->fat_sectors - F), buf)) {
            why(out, "a write failed while the data moved -- the volume is damaged; restore it from a backup");
            goto out;
        }
        if (!blkdev_flush(dev)) { why(out, "the disk did not flush the moved data -- the volume is damaged"); goto out; }
    }

    // ---- 2. THE FATS at their new places (or their new entries zeroed,
    // in place). CRASH HERE, MOVING: still the old boot sector over moved
    // data -- CORRUPT, as above. IN PLACE: the OLD size, clean; entries
    // past its last cluster are not read at that size.
    {
        uint32_t tail = (sbi->v.cluster_count + 2) * 4 / SECTOR;
        tail -= tail % k;
        uint32_t steps = p->moves ? (N - 1) * F + (F - tail) + N * (p->fat_sectors - F) : N * (F - tail);
        fsck_stage(live, 1, steps);
        if (!write_fats(sbi, p->fat_sectors, buf, &done)) {
            why(out, p->moves ? "a write failed on its allocation tables -- the volume is damaged"
                              : "a write failed -- the volume is as it was");
            goto out;
        }
        if (!blkdev_flush(dev)) { why(out, "the disk did not flush the allocation tables"); goto out; }
    }

    // ---- 3. THE SWITCH: the boot sector, then FSInfo. CRASH BEFORE THE
    // BOOT SECTOR: as step 2. AFTER IT: the NEW size, every file where
    // the new layout looks for it; until FSInfo is written its free count
    // is the old one, a hint this driver recounts and fsck.fat reports.
    fsck_stage(live, 2, 4);
    if (!write_boot(sbi, boot, 0)) {
        why(out, p->moves ? "its boot sector could not be written -- the volume is damaged"
                          : "its boot sector could not be written -- the volume is as it was");
        goto out;
    }
    fsck_step(live, 1, 0);
    if (sbi->v.fsinfo_sector && !write_fsinfo(sbi, sbi->v.fsinfo_sector, free_new))
        klog_write(KLOG_ERR "fat32: grow: FSInfo could not be written -- its free count is stale\n");
    fsck_step(live, 2, 0);
    blkdev_flush(dev);

    // ---- 4. THE BACKUPS at sector 6 (BPB_BkBootSec) and the FSInfo copy
    // after it. CRASH HERE: the NEW size, clean; the backup still names
    // the old layout, which only a repair that lost the primary reads --
    // fsck.fat reports the two differing.
    if (bk && bk != 0xFFFFu && (uint64_t)(bk + 1) * k < sbi->v.reserved) {
        if (!write_boot(sbi, boot, (uint64_t)bk * k))
            klog_write(KLOG_ERR "fat32: grow: the backup boot sector could not be written\n");
        fsck_step(live, 3, 0);
        if (!write_fsinfo(sbi, (uint64_t)(bk + 1) * k, free_new))
            klog_write(KLOG_ERR "fat32: grow: the backup FSInfo could not be written\n");
    }
    fsck_step(live, 4, 0);
    blkdev_flush(dev);
    klog_printf("fat32: grew from %u to %u sectors, %u -> %u clusters%s\n", sbi->v.total_sectors, p->total,
                sbi->v.cluster_count, p->clusters, p->moves ? ", its data moved" : ", in place");
    rc = 0;
out:
    kfree(cs.used);
    kfree(buf);
    return rc;
}

// ---- shrinking --------------------------------------------------------------------

// The clusters a shrink to `cut` (the first cluster number past the new
// end) relocates: (old, new) in ascending order of BOTH, since the free
// ones below are taken lowest first while the old ones are walked upward.
struct move { uint32_t from, to, val; };

struct shrink_census {
    uint8_t *used;          // a bit per cluster in use, bad ones included
    uint8_t *bad;           // a bit per cluster past the cut marked bad: dropped, not moved
    uint32_t cut, in_use, bad_past, free;
};

static int shrink_entry(void *ctx, uint32_t c, uint32_t v) {
    struct shrink_census *s = ctx;
    if (v == FAT_FREE) { s->free++; return 0; }
    s->in_use++;
    if (v == FAT_BAD && c >= s->cut) {
        s->bad_past++;
        s->bad[(c - s->cut) >> 3] |= (uint8_t)(1u << ((c - s->cut) & 7));
    }
    s->used[c >> 3] |= (uint8_t)(1u << (c & 7));
    return 0;
}

// The relocation's new number for `c`, or `c` itself when it stays.
static uint32_t moved_to(const struct move *m, uint32_t n, uint32_t c) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m[mid].from == c) return m[mid].to;
        if (m[mid].from < c) lo = mid + 1; else hi = mid;
    }
    return c;
}

// The move whose destination is `c`, or NULL.
static const struct move *move_into(const struct move *m, uint32_t n, uint32_t c) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m[mid].to == c) return &m[mid];
        if (m[mid].to < c) lo = mid + 1; else hi = mid;
    }
    return 0;
}

// A FAT link through the relocation: a chain value naming a moved cluster
// names its new place; EOC, free and bad stay as they are.
static uint32_t relink(const struct move *m, uint32_t n, uint32_t v) {
    uint32_t x = v & FAT_MASK;
    if (x < 2 || x >= FAT_BAD) return v;
    return (v & 0xF0000000u) | moved_to(m, n, x);
}

struct moves { struct move *m; uint32_t n; };

static int collect_val(void *ctx, uint32_t c, uint32_t v) {
    struct moves *mv = ctx;
    uint32_t lo = 0, hi = mv->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (mv->m[mid].from == c) { mv->m[mid].val = v; return 0; }
        if (mv->m[mid].from < c) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static uint64_t clus_lba(const struct fat_vol *v, uint32_t c) {
    return v->first_data_sector + (uint64_t)(c - 2) * v->sectors_per_cluster;
}

// The OLD FAT's entry for `c`, from FAT 0 on disk (not yet rewritten while
// the directories are walked), through a one-sector cache of its own.
struct fat_reader { uint8_t sec[SECTOR]; uint64_t lba; };

static int old_link(struct fat32_state *sbi, struct fat_reader *r, uint32_t c, uint32_t *out) {
    uint64_t byte = (uint64_t)c * 4;
    uint64_t lba = sbi->v.reserved + byte / SECTOR;
    if (r->lba != lba) {
        if (!xfer(sbi->v.dev, lba, 1, r->sec, 0)) return 0;
        r->lba = lba;
    }
    *out = rd32(r->sec + byte % SECTOR) & FAT_MASK;
    return 1;
}

// Every directory, from the root down: each entry's first cluster -- and
// "." and ".." -- through the relocation, in the cluster's NEW place (its
// data is already there). A directory's chain is followed in the OLD FAT,
// whose numbers are the old ones the relocation maps.
static int patch_dirs(struct fat32_state *sbi, const struct move *m, uint32_t nm, uint8_t *buf,
                      uint32_t *folders) {
    const uint32_t spc = sbi->v.sectors_per_cluster, last = sbi->v.cluster_count + 1;
    const uint32_t per = spc * SECTOR / 32;
    struct fs_check_live *live = fs_format_live();
    struct fat_reader *rd = kmalloc(sizeof *rd);
    uint32_t cap = 64, top = 0, steps = 0;
    uint32_t *stack = kmalloc(cap * sizeof *stack);
    int ok = rd && stack;
    if (ok) { rd->lba = ~0ull; stack[top++] = sbi->v.root_cluster; }
    while (ok && top) {
        uint32_t c = stack[--top];
        (*folders)++;
        fsck_step(live, *folders, 0);
        // A chain no longer than the volume: a loop in a damaged FAT ends.
        for (uint32_t hops = 0; ok && c >= 2 && c <= last && hops <= last; hops++) {
            uint64_t lba = clus_lba(&sbi->v, moved_to(m, nm, c));
            if (!xfer(sbi->v.dev, lba, spc, buf, 0)) { ok = 0; break; }
            int changed = 0, end = 0;
            for (uint32_t i = 0; i < per && !end; i++) {
                uint8_t *e = buf + (uint64_t)i * 32;
                if (e[0] == 0x00) { end = 1; break; }
                if (e[0] == 0xE5 || (e[11] & 0x0F) == 0x0F || (e[11] & 0x08)) continue;
                uint32_t fc = (uint32_t)rd16(e + 20) << 16 | rd16(e + 26);
                uint32_t nc = fc >= 2 && fc <= last ? moved_to(m, nm, fc) : fc;
                if (nc != fc) {
                    wr16(e + 20, (uint16_t)(nc >> 16));
                    wr16(e + 26, (uint16_t)nc);
                    changed = 1;
                }
                // A subdirectory to walk -- by its OLD number, as the chain
                // is followed in the old FAT. "." and ".." are not.
                if ((e[11] & 0x10) && e[0] != '.' && fc >= 2 && fc <= last) {
                    if (top == cap) {
                        uint32_t *g = kmalloc(cap * 2 * sizeof *g);
                        if (!g) { ok = 0; break; }
                        k_memcpy(g, stack, cap * sizeof *g);
                        kfree(stack);
                        stack = g;
                        cap *= 2;
                    }
                    stack[top++] = fc;
                }
            }
            if (ok && changed && !xfer(sbi->v.dev, lba, spc, buf, 1)) ok = 0;
            if (!ok || end) break;
            uint32_t next;
            if (!old_link(sbi, rd, c, &next)) { ok = 0; break; }
            if (next >= FAT_EOC_MIN || next < 2) break;
            c = next;
            if (++steps > last) { ok = 0; break; }   // more directory clusters than the volume has
        }
    }
    kfree(stack);
    kfree(rd);
    return ok;
}

// Every FAT copy through the relocation, highest copy first so FAT 0 --
// what each sector is read from -- changes last, a sector at a time.
static int relink_fats(struct fat32_state *sbi, const struct move *m, uint32_t nm, uint32_t cut,
                       uint8_t *buf, uint32_t *done) {
    const struct block_device *dev = sbi->v.dev;
    struct fs_check_live *live = fs_format_live();
    const uint32_t R = sbi->v.reserved, F = sbi->v.fat_sectors, per = SECTOR / 4;
    for (int i = (int)sbi->v.num_fats - 1; i >= 0; i--) {
        for (uint32_t s = 0; s < F; ) {
            uint32_t n = F - s < MOVE_SECTORS ? F - s : MOVE_SECTORS;
            if (!xfer(dev, R + s, n, buf, 0)) return 0;
            for (uint32_t j = 0; j < n * per; j++) {
                uint32_t c = s * per + j;
                if (c < 2) continue;                              // the media byte and the flags
                uint8_t *e = buf + (uint64_t)j * 4;
                if (c >= cut) { wr32(e, 0); continue; }            // past the new end
                const struct move *into = move_into(m, nm, c);
                uint32_t v = into ? (rd32(e) & 0xF0000000u) | (into->val & FAT_MASK) : rd32(e);
                wr32(e, relink(m, nm, v));
            }
            if (!xfer(dev, (uint64_t)R + (uint64_t)i * F + s, n, buf, 1)) return 0;
            s += n;
            *done += n;
            fsck_step(live, *done, 0);
        }
    }
    return 1;
}

// FSInfo at `lba`: its free count, and its next-free hint cleared (it may
// name a cluster past the new end).
static int write_fsinfo_shrunk(struct fat32_state *sbi, uint64_t lba, uint32_t free_count) {
    uint8_t *t = sbi->tmpsec;
    if (!blkdev_read_partial(sbi->v.dev, lba, 1, t)) return 0;
    if (rd32(t) != 0x41615252u || rd32(t + 484) != 0x61417272u) return 1;
    wr32(t + 488, free_count);
    wr32(t + 492, 0xFFFFFFFFu);
    return blkdev_write_partial(sbi->v.dev, lba, 1, t);
}

static int shrink(struct fat32_state *sbi, uint32_t total, struct fs_resize_info *out) {
    const struct block_device *dev = sbi->v.dev;
    struct fs_check_live *live = fs_format_live();
    const uint32_t k = sbi->v.k, spc = sbi->v.sectors_per_cluster, last = sbi->v.cluster_count + 1;
    const uint32_t clusters = (total - sbi->v.first_data_sector) / spc, cut = clusters + 2;
    struct shrink_census cs = { 0, 0, cut, 0, 0, 0 };
    struct moves mv = { 0, 0 };
    uint8_t boot[SECTOR];
    uint8_t *buf = kmalloc(MOVE_SECTORS * SECTOR > spc * SECTOR ? MOVE_SECTORS * SECTOR : spc * SECTOR);
    uint32_t folders = 0, done = 0, bk;
    int rc = -EIO;
    cs.used = kzalloc((last + 8) / 8);
    cs.bad = kzalloc((last - cut + 9) / 8);
    if (!buf || !cs.used || !cs.bad) { why(out, "out of memory"); rc = -ENOMEM; goto out; }
    if (fat32_scan(sbi, shrink_entry, &cs) != 0) { why(out, "its allocation table cannot be read"); goto out; }

    // The relocation: each cluster in use past the end (bad marks there
    // simply go with the space) to the lowest free one below it.
    {
        uint32_t want_moves = 0;
        for (uint32_t c = cut; c <= last; c++) want_moves += used_bit(cs.used, c);
        want_moves -= cs.bad_past;
        mv.m = want_moves ? kmalloc(want_moves * sizeof *mv.m) : 0;
        if (want_moves && !mv.m) { why(out, "out of memory"); rc = -ENOMEM; goto out; }
        uint32_t low = 2;
        for (uint32_t c = cut; c <= last && mv.n < want_moves; c++) {
            if (!used_bit(cs.used, c) || used_bit(cs.bad, c - cut)) continue;
            while (low < cut && used_bit(cs.used, low)) low++;
            if (low >= cut) break;
            mv.m[mv.n++] = (struct move){ c, low, 0 };
            cs.used[low >> 3] |= (uint8_t)(1u << (low & 7));
        }
        // Bad clusters past the end are not moves; any used cluster left
        // over means the plan's smallest size was wrong.
        uint32_t still = 0;
        for (uint32_t c = cut; c <= last; c++) still += used_bit(cs.used, c);
        if (still - cs.bad_past != mv.n) {
            why(out, "its files do not fit in that size");
            rc = -ENOSPC;
            goto out;
        }
    }
    // Each moved cluster's own FAT entry, to write at its new number.
    if (mv.n && fat32_scan(sbi, collect_val, &mv) != 0) { why(out, "its allocation table cannot be read"); goto out; }
    if (!blkdev_read_partial(dev, 0, 1, boot)) { why(out, "its boot sector cannot be read"); goto out; }
    bk = rd16(boot + 50);

    // ---- 1. THE DATA into free clusters below the new end. CRASH HERE:
    // the old layout, intact -- every copy landed on a cluster it calls free.
    // A RUN of moves whose sources and targets both follow on is one
    // transfer each way: cluster by cluster, a 171 MiB shrink on a USB
    // stick took eleven minutes.
    fsck_stage(live, 0, mv.n);
    for (uint32_t i = 0, r; i < mv.n; i += r) {
        uint32_t room = MOVE_SECTORS / spc ? MOVE_SECTORS / spc : 1;
        for (r = 1; i + r < mv.n && r < room && mv.m[i + r].from == mv.m[i].from + r &&
                    mv.m[i + r].to == mv.m[i].to + r; r++) {}
        if (!xfer(dev, clus_lba(&sbi->v, mv.m[i].from), r * spc, buf, 0) ||
            !xfer(dev, clus_lba(&sbi->v, mv.m[i].to), r * spc, buf, 1)) {
            why(out, "a write failed while the data moved -- the volume is as it was");
            goto out;
        }
        fsck_step(live, i + r, 0);
    }
    if (!blkdev_flush(dev)) { why(out, "the disk did not flush the moved data -- the volume is as it was"); goto out; }

    // ---- 2. THE DIRECTORIES, in place. CRASH HERE: entries naming new
    // places the old FAT calls free -- the volume is DAMAGED.
    fsck_stage(live, 1, 0);
    if (!patch_dirs(sbi, mv.m, mv.n, buf, &folders)) {
        why(out, "a folder could not be read or written -- the volume is damaged");
        goto out;
    }
    if (!blkdev_flush(dev)) { why(out, "the disk did not flush the folders -- the volume is damaged"); goto out; }

    // ---- 3. THE FATS. CRASH HERE: damaged, as above.
    fsck_stage(live, 2, sbi->v.num_fats * sbi->v.fat_sectors);
    if (!relink_fats(sbi, mv.m, mv.n, cut, buf, &done)) {
        why(out, "a write failed on its allocation tables -- the volume is damaged");
        goto out;
    }
    if (!blkdev_flush(dev)) { why(out, "the disk did not flush the allocation tables"); goto out; }

    // ---- 4. THE SWITCH: the boot sector (size, root), FSInfo, backups.
    // CRASH BEFORE THE BOOT SECTOR: the old size over relinked FATs, whose
    // entries past the new end are free -- fsck.fat reports lost space,
    // and a second shrink to the same size finishes it.
    fsck_stage(live, 3, 4);
    wr32(boot + 32, total / k);
    wr32(boot + 44, moved_to(mv.m, mv.n, sbi->v.root_cluster));
    uint32_t free_new = clusters - (cs.in_use - cs.bad_past);
    if (!write_boot(sbi, boot, 0)) { why(out, "its boot sector could not be written -- the volume is damaged"); goto out; }
    fsck_step(live, 1, 0);
    if (sbi->v.fsinfo_sector && !write_fsinfo_shrunk(sbi, sbi->v.fsinfo_sector, free_new))
        klog_write(KLOG_ERR "fat32: shrink: FSInfo could not be written -- its free count is stale\n");
    fsck_step(live, 2, 0);
    blkdev_flush(dev);
    if (bk && bk != 0xFFFFu && (uint64_t)(bk + 1) * k < sbi->v.reserved) {
        if (!write_boot(sbi, boot, (uint64_t)bk * k))
            klog_write(KLOG_ERR "fat32: shrink: the backup boot sector could not be written\n");
        fsck_step(live, 3, 0);
        if (!write_fsinfo_shrunk(sbi, (uint64_t)(bk + 1) * k, free_new))
            klog_write(KLOG_ERR "fat32: shrink: the backup FSInfo could not be written\n");
    }
    fsck_step(live, 4, 0);
    blkdev_flush(dev);
    klog_printf("fat32: shrank from %u to %u sectors, %u -> %u clusters, %u moved, %u folders patched\n",
                sbi->v.total_sectors, total, sbi->v.cluster_count, clusters, mv.n, folders);
    rc = 0;
out:
    kfree(mv.m);
    kfree(cs.bad);
    kfree(cs.used);
    kfree(buf);
    return rc;
}

// The smallest size, in 512-byte sectors, a shrink can make of the volume
// with `in_use` clusters taken (bad ones included): all of them below the
// end, and never under FAT32's floor for a volume that is above it.
static uint64_t shrink_floor(const struct fat_vol *v, uint32_t in_use) {
    uint64_t c = in_use;
    if (v->cluster_count >= FAT32_MIN_CLUSTERS && c < FAT32_MIN_CLUSTERS) c = FAT32_MIN_CLUSTERS;
    if (c < 1) c = 1;
    uint64_t s = v->first_data_sector + c * v->sectors_per_cluster;
    uint64_t k = v->k ? v->k : 1;
    return (s + k - 1) / k * k;
}

// ---- describing, and the op ----------------------------------------------------------

// Fill `out` from the volume sbi->v describes: its size, its use, how
// far it grows. 1 when it can grow at all; `out->why` the reason when not.
static int describe(struct fat32_state *sbi, struct fs_resize_info *out) {
    const uint64_t cb = (uint64_t)sbi->v.sectors_per_cluster * SECTOR;
    struct census cs = { 0, 0, 0, 0 };
    out->size_bytes = (uint64_t)sbi->v.total_sectors * SECTOR;
    out->min_bytes = out->max_bytes = out->size_bytes;
    out->flags = 0;
    if (fat32_scan(sbi, census_entry, &cs) != 0) { why(out, "Its allocation table cannot be read"); return 0; }
    out->used_bytes = (uint64_t)cs.in_use * cb;

    // The first reason that applies; each is a different thing to do.
    uint8_t *t = sbi->tmpsec;
    if (!blkdev_read_partial(sbi->v.dev, 0, 1, t)) { why(out, "Its boot sector cannot be read"); return 0; }
    if (rd16(t + 40) & 0x80) {
        why(out, "Only one of its FATs is kept current -- SaunaOS grows a volume whose FATs are mirrored");
        return 0;
    }
    // BS_Reserved1's bit 0 (Linux's FAT_STATE_DIRTY) and FAT entry 1's
    // ClnShutBitMask and HrdErrBitMask (Windows'): either says a system
    // did not finish with it.
    int dirty = t[65] & 1;
    if (!dirty && blkdev_read_partial(sbi->v.dev, sbi->v.reserved, 1, t))
        dirty = (rd32(t + 4) & 0x0C000000u) != 0x0C000000u;
    if (dirty) { why(out, "It was not unmounted cleanly -- check it first"); return 0; }

    struct fat_grow_plan p;
    out->max_bytes = fat32_grow_plan(&sbi->v, 0xFFFFFFFFull, &p) ? (uint64_t)p.total * SECTOR : out->size_bytes;
    if (out->max_bytes > out->size_bytes) out->flags = FS_RESIZE_CAN_GROW | FS_RESIZE_UNSAFE;
    uint64_t floor = shrink_floor(&sbi->v, cs.in_use);
    if (floor + sbi->v.sectors_per_cluster <= sbi->v.total_sectors) {
        out->min_bytes = floor * SECTOR;
        out->flags |= FS_RESIZE_CAN_SHRINK | FS_RESIZE_UNSAFE;
    }
    if (!(out->flags & FS_RESIZE_CAN_SHRINK))
        why(out, sbi->v.cluster_count >= FAT32_MIN_CLUSTERS && cs.in_use < FAT32_MIN_CLUSTERS
                     ? "It is as small as FAT32 allows: fewer clusters would make it FAT16"
                     : "Its files fill it: there is no room to shrink into");
    else if (!(out->flags & FS_RESIZE_CAN_GROW))
        why(out, "It is as large as a FAT32 volume grows");
    else
        out->why[0] = 0;
    return out->flags != 0;
}

int fat32_resize(void *st, const struct block_device *dev, uint64_t sectors,
                 struct fs_resize_info *out) {
    struct fat32_state *sbi = st;
    if (fat32_parse_bpb(sbi, dev) != 1) {
        why(out, "The FAT32 volume on it cannot be read");
        return -EIO;
    }
    int can = describe(sbi, out);
    if (!sectors) return 0;

    uint64_t dev_sectors = blkdev_sector_count(dev);
    uint64_t want = sectors < dev_sectors ? sectors : dev_sectors;
    if (want < sbi->v.total_sectors) {
        want -= want % sbi->v.k;
        if (!(out->flags & FS_RESIZE_CAN_SHRINK)) return -ENOTSUP;
        if (want * SECTOR < out->min_bytes) {
            k_snprintf(out->why, sizeof out->why, "Its files need %llu MiB at least",
                       (unsigned long long)((out->min_bytes + (1u << 20) - 1) >> 20));
            return -ENOSPC;
        }
        // Whole clusters: the slack under one past them stays the
        // partition's, as a format leaves it.
        uint32_t clusters = (uint32_t)((want - sbi->v.first_data_sector) / sbi->v.sectors_per_cluster);
        if (clusters >= sbi->v.cluster_count) { out->why[0] = 0; return 0; }   // less than a cluster
        out->why[0] = 0;
        fsck_begin(fs_format_live(), 0, SHRINK_STAGES, 4);
        int rc = shrink(sbi, (uint32_t)want, out);
        fsck_end(fs_format_live());
        if (!rc) {
            if (fat32_parse_bpb(sbi, dev) == 1) describe(sbi, out);
            out->why[0] = 0;
        }
        return rc;
    }
    if (want == sbi->v.total_sectors) { out->why[0] = 0; return 0; }   // already that size
    if (!can) return -ENOTSUP;
    if (want > out->max_bytes / SECTOR) {
        k_snprintf(out->why, sizeof out->why,
                   "FAT32 grows this volume to %llu MiB at most: its limit on clusters or sectors",
                   (unsigned long long)(out->max_bytes >> 20));
        return -ENOSPC;
    }
    struct fat_grow_plan p;
    if (!fat32_grow_plan(&sbi->v, want, &p)) { out->why[0] = 0; return 0; }   // less room than one cluster gains

    out->why[0] = 0;
    fsck_begin(fs_format_live(), 0, GROW_STAGES, 3);
    int rc = grow(sbi, &p, out);
    fsck_end(fs_format_live());
    if (!rc) {
        if (fat32_parse_bpb(sbi, dev) == 1) describe(sbi, out);
        out->why[0] = 0;
    }
    return rc;
}

// ---- KTEST ------------------------------------------------------------------
// In fat32_resize_test.c: a growable RAM volume grown in place and with a
// shift, every file re-read, both FATs and the backups compared, and the
// plan's arithmetic at FAT32's limits.
