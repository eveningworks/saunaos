// TFS3: growing a volume nothing has mounted -- the filesystem half of a
// partition resize (SYS_FS_RESIZE), after the table has grown. ext4's
// resize.c is the shape; tfs3_internal.h has the map.
//
// GROWING IS CHEAP because a group's position is fixed by its number: the
// old groups stay where they are, the new ones go in after them
// UNINITIALISED (their bitmaps derived, their inode tables unused -- the
// lazy format's descriptors), the old short last group gets its tail, and
// the backups move to the new last group. Nothing that holds a file moves.
//
// TWO THINGS ARE REFUSED rather than done:
//  * a grow that would make the DESCRIPTOR TABLE bigger
//    (t3_v3_gdt_blocks() -- 16 blocks up to 512 GiB, more past it):
//    group 0 starts right after the table, so a bigger one moves every
//    group, which is a shrink's work (roadmap: "Shrink a TFS3 volume");
//  * v1/v2 volumes, whose descriptors cannot say "uninitialised": every
//    new group would need its bitmaps and a zeroed inode table written.
//    A volume Disks or mkfs made is v3.
//
// THE ORDER IS THE POINT -- an interruption anywhere leaves a volume that
// mounts and checks clean at the OLD size or the NEW one, never neither.
// The commit is ONE JOURNAL TRANSACTION holding the three blocks the two
// sizes disagree about (the superblock, the descriptor-table block with
// the old last group's entry, and that group's block bitmap), so no
// reader ever sees one of them changed without the others -- ext4's
// online resize journals its superblock and descriptors for the same
// reason. Everything the new size needs and the old one never reads goes
// to the media BEFORE it; what the old size still uses is changed AFTER.
// Where each crash lands is said at each step in grow().

#include "tfs3_internal.h"
#include "errno.h"
#include "heap.h"
#include "kfmt.h"
#include "klog.h"
#include "string.h"

// What a grow reports to Disks' stage dialog and `part` (fs_format_live()).
static const struct fsck_stage_desc T3_GROW_STAGES[] = {
    { "Write the new groups", "blocks" },
    { "Switch to the new size", "blocks" },
    { "Move the backups", "blocks" },
};

#define T3_GD_PER_BLOCK (T3_BLOCK / 16u)

static void why(struct fs_resize_info *out, const char *s) { k_strlcpy(out->why, s, sizeof out->why); }

// The largest volume, in blocks, whose descriptor table is the size this
// one's already is: t3_v3_gdt_blocks() counts groups from block
// T3_V3_GDT, so `n` table blocks hold every group of a volume up to
// T3_V3_GDT + n * 256 groups. One block more needs table block n + 1.
static uint32_t grow_limit(const struct t3_state *sbi) {
    uint64_t max = (uint64_t)T3_V3_GDT + (uint64_t)sbi->gdt_blocks * T3_GD_PER_BLOCK * T3_BPG;
    return max > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)max;
}

static uint32_t groups_for(const struct t3_state *sbi, uint32_t blocks) {
    return (blocks - sbi->group0 + T3_BPG - 1) / T3_BPG;
}

// Where group g's backup superblock is, under the geometry sbi holds now.
static uint32_t backup_sb_block(struct t3_state *sbi, uint32_t g) {
    uint32_t span = t3_group_span(sbi, g);
    return span ? t3_group_base(sbi, g) + span - 1 : 0;
}

// One table block of the descriptors sbi->gd holds, as format writes it.
static void pack_gdt_block(struct t3_state *sbi, uint32_t tb, uint8_t *img) {
    k_memset(img, 0, T3_BLOCK);
    for (uint32_t i = 0; i < T3_GD_PER_BLOCK; i++) {
        uint32_t g = tb * T3_GD_PER_BLOCK + i;
        if (g >= sbi->sb.gc) break;
        t3_gd_pack(sbi, g, img + i * 16);
    }
}

// A backup region whose superblock is at `tail`: the descriptor table's
// snapshot in the blocks before it, then the superblock -- format's layout.
static int write_backup(struct t3_state *sbi, uint32_t tail, uint8_t *img) {
    for (uint32_t tb = 0; tb < sbi->gdt_blocks; tb++) {
        pack_gdt_block(sbi, tb, img);
        if (!t3_write_block(sbi, tail - sbi->gdt_blocks + tb, img)) return 0;
    }
    t3_pack_superblock(sbi, img);
    return t3_write_block(sbi, tail, img);
}

static int bit(const uint8_t *bm, uint32_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static void set_bit(uint8_t *bm, uint32_t i, int v) {
    if (v) bm[i >> 3] |= (uint8_t)(1u << (i & 7)); else bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

// Grow the open volume to `nt` blocks (> its size, <= grow_limit()).
static int grow(struct t3_state *sbi, uint32_t nt, struct fs_resize_info *out) {
    struct fs_check_live *live = fs_format_live();
    const uint32_t ot = sbi->sb.total_blocks, og = sbi->sb.gc, last = og - 1;
    const uint32_t ng = groups_for(sbi, nt);

    // ---- the OLD geometry's facts, before sbi is switched to the new one
    const uint32_t old_end = t3_group_data_end(sbi, last);
    uint32_t old_bk[2], old_bk_sb[2];
    int old_nbk = t3_backup_groups(og, old_bk);
    for (int i = 0; i < old_nbk; i++) old_bk_sb[i] = backup_sb_block(sbi, old_bk[i]);
    const int last_on_disk = !(sbi->gd[last].flags & T3_BG_BLOCK_UNINIT);

    uint8_t *img = kmalloc(T3_BLOCK), *lbm = last_on_disk ? kmalloc(T3_BLOCK) : 0;
    struct t3_gd *ngd = kzalloc(sizeof *ngd * ng);
    struct t3_gd *ogd = sbi->gd;
    int rc = -EIO;
    if (!img || !ngd || (last_on_disk && !lbm)) { why(out, "out of memory"); rc = -ENOMEM; goto free_only; }
    if (lbm && !t3_read_block(sbi, t3_group_base(sbi, last), lbm)) {
        why(out, "the last group's block bitmap cannot be read");
        goto free_only;
    }
    k_memcpy(ngd, ogd, sizeof *ngd * og);

    // ---- the NEW geometry: every helper below answers for the new size
    sbi->sb.total_blocks = nt;
    sbi->sb.gc = ng;
    sbi->gd = ngd;
    const uint32_t new_end = t3_group_data_end(sbi, last);
    for (uint32_t g = og; g < ng; g++) {
        uint32_t end = t3_group_data_end(sbi, g);
        ngd[g].free_blocks = end > sbi->meta_off ? end - sbi->meta_off : 0;
        ngd[g].free_inodes = sbi->sb.ipg;
        ngd[g].flags = T3_BG_BLOCK_UNINIT | T3_BG_INODE_UNINIT;
        ngd[g].itable_used = 0;
    }
    // The old last group gains its tail: the blocks past its old end --
    // its old backup region and, when it was short, the padding that
    // stood for blocks past the volume -- are free now, up to the new
    // backup region or the group's end. Its count is RECOUNTED from the
    // bitmap rather than adjusted, so a wrong count is not carried over.
    {
        uint32_t lo = old_end > sbi->meta_off ? old_end : sbi->meta_off;
        uint32_t data = new_end > sbi->meta_off ? new_end - sbi->meta_off : 0;
        uint32_t used = 0;
        if (lbm) {
            for (uint32_t i = sbi->meta_off; i < lo; i++) used += bit(lbm, i);
            for (uint32_t i = lo; i < T3_BPG; i++) set_bit(lbm, i, i >= new_end);
        }
        ngd[last].free_blocks = data - used;
    }
    uint32_t new_bk[2], new_bk_sb[2];
    int new_nbk = t3_backup_groups(ng, new_bk), late[2] = { 0, 0 };
    for (int i = 0; i < new_nbk; i++) {
        new_bk_sb[i] = backup_sb_block(sbi, new_bk[i]);
        // A region reaching into the OLD volume is written after the
        // switch: before it, those blocks are the old size's -- its own
        // backup, when it grew by less than a backup region's length.
        late[i] = new_bk_sb[i] - sbi->gdt_blocks < ot;
    }

    const uint32_t tb_last = last / T3_GD_PER_BLOCK, tb_end = (ng - 1) / T3_GD_PER_BLOCK;
    uint32_t done = 0, stage0 = tb_end - tb_last;
    for (int i = 0; i < new_nbk; i++) if (!late[i]) stage0 += sbi->gdt_blocks + 1;
    fsck_begin(live, 0, T3_GROW_STAGES, 3);
    fsck_stage(live, 0, stage0);

    // ---- 1. WHAT ONLY THE NEW SIZE READS. The new groups' descriptors in
    // table blocks the old size has no entry in, and the backups that lie
    // past the old end. CRASH HERE: the superblock still says the old
    // size, and an old-size mount reads no descriptor past its own group
    // count and no block past its end -- the OLD size, clean.
    for (uint32_t tb = tb_last + 1; tb <= tb_end; tb++) {
        pack_gdt_block(sbi, tb, img);
        if (!t3_write_block(sbi, sbi->gdt_block + tb, img)) goto unswitch;
        fsck_step(live, ++done, 0);
    }
    for (int i = 0; i < new_nbk; i++) {
        if (late[i]) continue;
        if (!write_backup(sbi, new_bk_sb[i], img)) goto unswitch;
        done += sbi->gdt_blocks + 1;
        fsck_step(live, done, 0);
    }
    // ...all of it on the media before the record that makes it part of
    // the volume: a write cache may otherwise land the commit first.
    if (!blkdev_flush(sbi->vol.dev)) goto unswitch;

    // ---- 2. THE SWITCH: the three blocks the two sizes disagree about, in
    // ONE journal transaction. CRASH BEFORE ITS HEADER IS COMMITTED: replay
    // discards it -- the OLD size. AFTER: the next mount replays all three
    // and re-reads the superblock (tfs3_init()) -- the NEW size, and step
    // 1 is already durable under it.
    fsck_stage(live, 1, 3);
    {
        int credits = lbm ? 3 : 2;
        uint8_t *p;
        if (!t3_txn_begin(sbi, credits)) goto unswitch;
        if (!(p = t3_txn_stage(sbi, sbi->gdt_block + tb_last))) { t3_txn_reset(sbi); goto unswitch; }
        pack_gdt_block(sbi, tb_last, p);
        if (lbm) {
            if (!(p = t3_txn_stage(sbi, t3_group_base(sbi, last)))) { t3_txn_reset(sbi); goto unswitch; }
            k_memcpy(p, lbm, T3_BLOCK);
        }
        if (!(p = t3_txn_stage(sbi, T3_SB_BLOCK))) { t3_txn_reset(sbi); goto unswitch; }
        t3_pack_superblock(sbi, p);
        if (!t3_txn_commit(sbi)) {
            // Read-only now means the commit was durable and only applying
            // it failed: the next mount's replay finishes the grow.
            if (sbi->readonly) {
                why(out, "interrupted after the switch -- it completes when the volume is next mounted");
                rc = -EIO;
                goto out;
            }
            goto unswitch;
        }
        fsck_step(live, 3, 0);
    }

    // ---- 3. THE BACKUPS THE OLD SIZE STILL HAD. The old superblock
    // backups that are not new ones are erased -- a stale one could claim
    // the disk if the partition were ever cut back -- and the new regions
    // that reached into the old volume are written. CRASH HERE: the NEW
    // size, clean (fsck does not read backups); until the rewrite a
    // backup may still hold the old superblock, which only a mount that
    // has lost its primary would read.
    fsck_stage(live, 2, (uint32_t)(old_nbk + new_nbk));
    done = 0;
    {
        uint8_t zero[T3_SECTOR];
        k_memset(zero, 0, sizeof zero);
        for (int i = 0; i < old_nbk; i++) {
            int kept = 0;
            for (int j = 0; j < new_nbk; j++)
                if (old_bk_sb[i] >= new_bk_sb[j] - sbi->gdt_blocks && old_bk_sb[i] <= new_bk_sb[j]) kept = 1;
            if (!kept && !t3_vol_write_sectors(sbi, (uint64_t)old_bk_sb[i] * T3_SPB, 1, zero))
                klog_write(KLOG_ERR "tfs3: grow: an old backup superblock could not be erased\n");
            fsck_step(live, ++done, 0);
        }
        for (int i = 0; i < new_nbk; i++) {
            if (late[i] && !write_backup(sbi, new_bk_sb[i], img))
                klog_write(KLOG_ERR "tfs3: grow: a backup region could not be written -- fsck repair rewrites it\n");
            fsck_step(live, ++done, 0);
        }
    }
    blkdev_flush(sbi->vol.dev);
    klog_printf("tfs3: grew from %u to %u blocks (%u -> %u groups)\n", ot, nt, og, ng);
    rc = 0;
    goto out;

unswitch:
    // Nothing the old size reads was changed: put the old geometry back.
    if (!out->why[0]) why(out, "a write failed -- the volume is as it was");
    sbi->sb.total_blocks = ot;
    sbi->sb.gc = og;
    sbi->gd = ogd;
    ogd = ngd;
    ngd = 0;
    goto free_only;
out:
    ngd = 0;           // sbi->gd now; unmount_state() frees it
free_only:
    fsck_end(live);
    if (sbi->gd != ogd) kfree(ogd);
    kfree(ngd);
    kfree(lbm);
    kfree(img);
    return rc;
}

// Fill `out` from the open volume: its size, its use, how far it grows.
static int describe(struct t3_state *sbi, struct fs_resize_info *out) {
    uint64_t used = 0, total = 0;
    tfs3_disk_usage(sbi, &used, &total);
    out->size_bytes = (uint64_t)sbi->sb.total_blocks * T3_BLOCK;
    out->used_bytes = used;
    out->min_bytes = out->size_bytes;
    out->max_bytes = out->size_bytes;
    // The first reason that applies; each is a different thing to do.
    if (sbi->sb.version < 3) {
        k_snprintf(out->why, sizeof out->why,
                   "This tfs3 volume is format version %u; only version 3 can grow", sbi->sb.version);
        return 0;
    }
    if (sbi->readonly) { why(out, "It is read-only after an error -- check and repair it first"); return 0; }
    if (sbi->mounted_from_backup) { why(out, "Its main superblock is damaged -- repair it first"); return 0; }
    out->max_bytes = (uint64_t)grow_limit(sbi) * T3_BLOCK;
    if (grow_limit(sbi) > sbi->sb.total_blocks) out->flags = FS_RESIZE_CAN_GROW;
    why(out, out->flags ? "Shrinking a tfs3 volume is not built yet"
                        : "It is as large as tfs3 grows without moving every group");
    return 1;
}

int tfs3_resize(void *st, const struct block_device *dev, uint64_t sectors,
                struct fs_resize_info *out) {
    struct t3_state *sbi = st;
    if (!t3_open_volume(sbi, dev)) {
        why(out, "The tfs3 volume on it does not mount");
        return -EIO;
    }
    int can = describe(sbi, out);
    int rc = 0;
    if (!sectors) goto close;

    uint64_t want = sectors / T3_SPB;
    uint32_t dev_blocks = t3_vol_blocks(sbi);
    if (want > dev_blocks) want = dev_blocks;
    if (want < sbi->sb.total_blocks) {
        why(out, "Shrinking a tfs3 volume is not built yet");
        rc = -ENOTSUP;
        goto close;
    }
    if (want == sbi->sb.total_blocks) { out->why[0] = 0; goto close; }   // already that size
    if (!can) { rc = -ENOTSUP; goto close; }
    if (want > grow_limit(sbi)) {
        k_snprintf(out->why, sizeof out->why,
                   "tfs3 grows this volume to %llu MiB at most; past it every group would move",
                   (unsigned long long)((uint64_t)grow_limit(sbi) * T3_BLOCK >> 20));
        rc = -ENOSPC;
        goto close;
    }
    uint32_t nt = (uint32_t)want;
    // A NEW last group too small for its own bitmaps, inode table and
    // backup is left out, as resize2fs leaves one out: the volume ends
    // at the last whole group instead, at most 2 MiB short of the
    // partition. Extending the existing last group needs no such floor.
    if (groups_for(sbi, nt) > sbi->sb.gc) {
        uint32_t rem = (nt - sbi->group0) % T3_BPG;
        if (rem && rem < T3_MIN_GROUP_BLOCKS) nt -= rem;
    }
    if (nt <= sbi->sb.total_blocks) { out->why[0] = 0; goto close; }   // the room is less than a group's floor

    out->why[0] = 0;
    rc = grow(sbi, nt, out);
    if (!rc) {
        describe(sbi, out);
        out->why[0] = 0;
    }
close:
    t3_close_volume(sbi);
    return rc;
}

// ---- KTEST ------------------------------------------------------------------
// In tfs3_resize_test.c: a growable RAM volume, filled, grown, checked,
// and grown again; and a grow cut short at every one of its writes.
