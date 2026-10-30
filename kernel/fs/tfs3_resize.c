// TFS3: resizing a volume nothing has mounted -- the filesystem half of a
// partition resize (SYS_FS_RESIZE): after the table has grown, or before
// it shrinks. ext4's resize.c and resize2fs are the shape;
// tfs3_internal.h has the map.
//
// GROWING IS CHEAP because a group's position is fixed by its number: the
// old groups stay where they are, the new ones go in after them
// UNINITIALISED (their bitmaps derived, their inode tables unused -- the
// lazy format's descriptors), the old short last group gets its tail, and
// the backups move to the new last group. Nothing that holds a file moves.
// SHRINKING moves what lies past the new end below it, and renumbers the
// inodes of the groups it drops -- its own section below says how.
//
// TWO THINGS ARE REFUSED rather than done:
//  * a size whose DESCRIPTOR TABLE would differ (t3_v3_gdt_blocks() --
//    16 blocks up to 512 GiB, more past it): group 0 starts right after
//    the table, so another size of it moves every group;
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

// ---- shrinking --------------------------------------------------------------
//
// resize2fs's offline shrink, journalled a step at a time: whole trailing
// groups go, and the new last group gives up the tail its backup region
// now needs. Three phases, at the OLD size until the last:
//
//  1. MOVE BLOCKS. Every inode's block tree is walked; a pointer naming a
//     block past the new end gets a copy in the lowest free block below
//     it. One transaction per pointer-holding block (the inode, or a
//     table) switches its pointers AND carries both bitmaps and their
//     descriptors (t3_txn_stage_alloc()), so an interruption leaves the
//     old pointers or the new, and never a leak: the copies sat in blocks
//     that were free until that commit said otherwise.
//  2. RENUMBER INODES. An inode's number encodes its group, so one in a
//     dropped group takes a free slot below the cut, and every entry
//     naming it -- `.` and `..` included -- is rewritten, in the same
//     transaction as the inode's two slots and the inode bitmaps. A
//     DIRECTORY with more subdirectories than one transaction holds has
//     its names done first and the remaining `..`s in further ones: an
//     interruption there leaves `..`s that fsck repair rewrites. A FILE
//     whose names do not fit one transaction is refused -- a name is the
//     one thing nothing could put back.
//  3. SWITCH, as grow() does: the superblock, the last group's descriptor
//     block and bitmap in one transaction; then the backups.
//
// A read-only check runs first and refuses a volume with any problem
// (resize2fs wants a fresh `e2fsck -f` for the same reason): every
// phase trusts the bitmaps and the tree to agree.

static const struct fsck_stage_desc T3_SHRINK_STAGES[] = {
    { "Check the volume", "steps" },
    { "Move blocks", "blocks" },
    { "Renumber files", "files" },
    { "Switch to the new size", "blocks" },
    { "Write the backups", "blocks" },
};

// A directory entry naming an inode that is renumbered.
struct t3_ref {
    uint64_t ino;      // the inode it names
    uint32_t blk;      // the directory block holding it
    uint16_t off;      // the entry's offset in that block
    uint8_t dotdot;    // a `..` -- the one kind fsck repair can rewrite
};

struct shrink {
    struct t3_state *sbi;
    struct fs_resize_info *out;
    struct fs_check_live *live;
    uint32_t nt, ng;          // the new size in blocks, and in groups
    uint32_t last, last_end;  // the new last group, and where its data ends
    uint32_t cur_g, cur_i;    // the low allocator's cursor: it only climbs
    uint32_t done, count, moved;   // progress; inodes past the cut; blocks moved
    uint8_t *buf;
    struct t3_ref *refs;
    uint32_t nrefs, cap;
};

static int fail(struct shrink *s, int rc, const char *what) {
    if (!s->out->why[0]) why(s->out, what);
    return rc;
}

// Is `blk` past the new end -- in a dropped group, or in the new last
// group's tail? Metadata below a group's data is never named by a pointer.
static int doomed(const struct shrink *s, uint32_t blk) {
    if (blk < s->sbi->group0) return 0;
    uint32_t g = (blk - s->sbi->group0) / T3_BPG, i = (blk - s->sbi->group0) % T3_BPG;
    return g > s->last || (g == s->last && i >= s->last_end);
}

// The lowest free block below the new end. Never one a move freed: those
// are all past it.
static uint32_t low_block(struct shrink *s) {
    struct t3_state *sbi = s->sbi;
    t3_trim_flush(sbi);   // every allocator does (tfs3_alloc.c's TRIM note)
    for (; s->cur_g <= s->last; s->cur_g++, s->cur_i = sbi->meta_off) {
        uint32_t g = s->cur_g, end = g == s->last ? s->last_end : t3_group_data_end(sbi, g);
        if (!sbi->gd[g].free_blocks) continue;
        if (s->cur_i < sbi->meta_off) s->cur_i = sbi->meta_off;
        for (; s->cur_i < end; s->cur_i++) {
            if (t3_bbm_test(sbi, g, s->cur_i)) continue;
            t3_bbm_set(sbi, g, s->cur_i, 1);
            sbi->gd[g].free_blocks--;
            t3_mark_dirty(sbi->gdt_dirty, g);
            return t3_group_base(sbi, g) + s->cur_i++;
        }
    }
    return 0;
}

// The lowest free inode slot in groups [g0, g1), taken.
static uint64_t take_inode(struct t3_state *sbi, uint32_t g0, uint32_t g1) {
    for (uint32_t g = g0; g < g1 && g < sbi->sb.gc; g++) {
        if (!sbi->gd[g].free_inodes) continue;
        for (uint32_t i = 0; i < sbi->sb.ipg; i++) {
            if (t3_ibm_test(sbi, g, i)) continue;
            if (!t3_itable_reserve(sbi, g, i)) return 0;
            t3_ibm_set(sbi, g, i, 1);
            sbi->gd[g].free_inodes--;
            t3_mark_dirty(sbi->gdt_dirty, g);
            return (uint64_t)g * sbi->sb.ipg + i;
        }
    }
    return 0;
}

// One transaction: the pointers `v` (inode `ino`'s, or table `table`'s)
// and every bitmap and descriptor the batch changed.
static int commit_ptrs(struct shrink *s, const uint32_t *v, uint64_t ino, const struct t3_inode *node,
                       uint32_t table) {
    struct t3_state *sbi = s->sbi;
    // The copies reach the media before the commit's header does
    // (txn_commit_raw()'s barrier): a replayed pointer to an unwritten
    // copy would be a file full of whatever the block held before.
    sbi->alloc_unfenced = 1;
    if (!t3_txn_begin(sbi, (int)sbi->jslots)) return fail(s, -EIO, "the journal cannot be written");
    int ok;
    if (table) {
        uint8_t *p = t3_txn_stage(sbi, table);
        if ((ok = p != 0))
            for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) wr32(p + i * 4, v[i]);
    } else {
        ok = t3_txn_stage_inode(sbi, ino, node);
    }
    if (!ok || !t3_txn_stage_alloc(sbi)) {
        t3_txn_reset(sbi);
        return fail(s, -EIO, "a move did not fit one journal step -- the volume is as it was");
    }
    if (!t3_txn_commit(sbi)) return fail(s, -EIO, "a write failed -- the volume is as it was");
    t3_alloc_committed(sbi);
    return 0;
}

// Move the doomed blocks among v[0..n), a batch per transaction. A batch
// ends before the bitmaps and descriptors it dirties could outgrow the
// journal: the holder's block, and two to spare for a cursor that moves
// on to the next group mid-batch.
static int move_ptrs(struct shrink *s, uint32_t *v, uint32_t n, uint64_t ino, struct t3_inode *node,
                     uint32_t table) {
    struct t3_state *sbi = s->sbi;
    for (uint32_t k = 0; k < n; ) {
        int moved = 0;
        for (; k < n; k++) {
            uint32_t e = v[k];
            if (!e || !doomed(s, e)) continue;
            int need = t3_alloc_dirty_if(sbi, s->cur_g, T3_BMAP_BLOCK) +
                       t3_alloc_dirty_if(sbi, (e - sbi->group0) / T3_BPG, T3_BMAP_BLOCK);
            if (moved && t3_alloc_dirty_blocks(sbi) + need + 3 > (int)sbi->jslots) break;
            uint32_t t = low_block(s);
            if (!t) return fail(s, -ENOSPC, "there is no room left below the new end");
            if (!t3_read_block(sbi, e, s->buf) || !t3_write_block(sbi, t, s->buf))
                return fail(s, -EIO, "a block could not be copied -- the volume is as it was");
            t3_free_block_bit(sbi, e);
            v[k] = t;
            moved++;
            s->moved++;
            fsck_step(s->live, ++s->done, 0);
        }
        if (!moved) break;
        int rc = commit_ptrs(s, v, ino, node, table);
        if (rc) return rc;
    }
    return 0;
}

// A pointer table at `depth` (0: it names data), after its own move: its
// entries, then the tables under it.
static int move_table(struct shrink *s, uint32_t table, int depth) {
    uint32_t *v = kmalloc(T3_BLOCK);
    if (!v) return fail(s, -ENOMEM, "out of memory");
    int rc;
    if (!t3_read_block(s->sbi, table, v)) {
        rc = fail(s, -EIO, "a pointer table cannot be read");
    } else {
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) v[i] = rd32((const uint8_t *)&v[i]);
        rc = move_ptrs(s, v, T3_PTRS_PER_BLOCK, 0, 0, table);
        for (uint32_t i = 0; !rc && depth && i < T3_PTRS_PER_BLOCK; i++)
            if (v[i]) rc = move_table(s, v[i], depth - 1);
    }
    kfree(v);
    return rc;
}

static int move_inode_blocks(struct shrink *s, uint64_t ino) {
    struct t3_inode node;
    if (!t3_read_inode(s->sbi, ino, &node)) return fail(s, -EIO, "an inode cannot be read");
    int rc = move_ptrs(s, node.ptrs, 15, ino, &node, 0);
    for (int p = 12; !rc && p <= 14; p++)
        if (node.ptrs[p]) rc = move_table(s, node.ptrs[p], p - 12);
    return rc;
}

// Every inode in use, in number order. Slots above a group's high-water
// mark were never written (tfs3_internal.h's itable_used).
static int each_inode(struct shrink *s, int (*fn)(struct shrink *, uint64_t)) {
    struct t3_state *sbi = s->sbi;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        if ((sbi->gd[g].flags & T3_BG_INODE_UNINIT) || sbi->gd[g].free_inodes == sbi->sb.ipg) continue;
        uint32_t top = sbi->gd[g].itable_used < sbi->sb.ipg ? sbi->gd[g].itable_used : sbi->sb.ipg;
        for (uint32_t i = 0; i < top; i++) {
            uint64_t ino = (uint64_t)g * sbi->sb.ipg + i;
            if (!ino || !t3_ibm_test(sbi, g, i)) continue;
            int rc = fn(s, ino);
            if (rc) return rc;
        }
    }
    return 0;
}

// The entries naming inodes in [lo, hi), from every directory.
static uint64_t g_ref_lo, g_ref_hi;   // each_inode()'s callback takes no context
static int collect_refs(struct shrink *s, uint64_t dir) {
    struct t3_state *sbi = s->sbi;
    struct t3_inode node;
    if (!t3_read_inode(sbi, dir, &node)) return fail(s, -EIO, "an inode cannot be read");
    if (node.type != T3_TYPE_DIR) return 0;
    uint32_t nb = (uint32_t)((node.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, &node, b, &blk) || (blk && !t3_read_block(sbi, blk, s->buf)))
            return fail(s, -EIO, "a folder cannot be read");
        for (uint32_t off = 0; blk && off + 8 <= T3_BLOCK; ) {
            uint32_t e = rd32(s->buf + off);
            uint16_t rec = rd16(s->buf + off + 4);
            uint8_t nl = s->buf[off + 6];
            if (rec < 8 || off + rec > T3_BLOCK) return fail(s, -EIO, "a folder is damaged");
            if (e && nl && e >= g_ref_lo && e < g_ref_hi) {
                if (s->nrefs == s->cap) {
                    uint32_t cap = s->cap ? s->cap * 2 : 64;
                    struct t3_ref *grown = krealloc(s->refs, sizeof *grown * cap);
                    if (!grown) return fail(s, -ENOMEM, "out of memory");
                    s->refs = grown;
                    s->cap = cap;
                }
                s->refs[s->nrefs++] = (struct t3_ref){
                    e, blk, (uint16_t)off, nl == 2 && s->buf[off + 7] == '.' && s->buf[off + 8] == '.' };
            }
            off += rec;
        }
    }
    return 0;
}

static int ref_before(const struct t3_ref *a, const struct t3_ref *b) {
    return a->ino != b->ino ? a->ino < b->ino : a->blk < b->blk;
}

// Heapsort: by inode, then block -- a name's group of entries is one run.
static void sort_refs(struct t3_ref *r, uint32_t n) {
    for (uint32_t i = n / 2; i-- > 0; ) {
        for (uint32_t p = i; ; ) {
            uint32_t c = 2 * p + 1;
            if (c >= n) break;
            if (c + 1 < n && ref_before(&r[c], &r[c + 1])) c++;
            if (!ref_before(&r[p], &r[c])) break;
            struct t3_ref t = r[p]; r[p] = r[c]; r[c] = t;
            p = c;
        }
    }
    for (uint32_t end = n; end-- > 1; ) {
        struct t3_ref t = r[0]; r[0] = r[end]; r[end] = t;
        for (uint32_t p = 0; ; ) {
            uint32_t c = 2 * p + 1;
            if (c >= end) break;
            if (c + 1 < end && ref_before(&r[c], &r[c + 1])) c++;
            if (!ref_before(&r[p], &r[c])) break;
            struct t3_ref t2 = r[p]; r[p] = r[c]; r[c] = t2;
            p = c;
        }
    }
}

static int staged(const struct t3_state *sbi, uint32_t blk) {
    for (int i = 0; i < sbi->txn_count; i++) if (sbi->txn_target[i] == blk) return 1;
    return 0;
}

static uint32_t inode_block(struct t3_state *sbi, uint64_t ino) {
    uint64_t lba;
    uint32_t off;
    return t3_inode_pos(sbi, ino, &lba, &off) ? (uint32_t)(lba / T3_SPB) : 0;
}

// Inode `old` becomes `nu` (taken already), and each of the n entries
// naming it says so. The names and both inode slots in the first
// transaction; `..`s that do not fit it in more (the header says why).
static int renumber(struct shrink *s, uint64_t old, uint64_t nu, const struct t3_ref *refs, uint32_t n) {
    struct t3_state *sbi = s->sbi;
    struct t3_inode node;
    if (!t3_read_inode(sbi, old, &node)) return fail(s, -EIO, "an inode past the new end cannot be read");
    t3_free_inode_bit(sbi, old);
    sbi->alloc_unfenced = 1;   // t3_itable_reserve() wrote table blocks directly

    // Names first, then `..`s; and the names must fit the first step.
    uint32_t *order = kmalloc(sizeof *order * (n ? n : 1));
    if (!order) return fail(s, -ENOMEM, "out of memory");
    uint32_t k = 0, names = 0;
    for (uint32_t i = 0; i < n; i++) if (!refs[i].dotdot) order[k++] = i;
    names = k;
    for (uint32_t i = 0; i < n; i++) if (refs[i].dotdot) order[k++] = i;
    int need = t3_alloc_dirty_blocks(sbi) + 1 + (inode_block(sbi, old) != inode_block(sbi, nu));
    for (uint32_t i = 0; i < names; i++) {
        int seen = 0;
        for (uint32_t j = 0; j < i && !seen; j++) seen = refs[order[j]].blk == refs[order[i]].blk;
        need += !seen;
    }
    int rc = 0;
    if (need > (int)sbi->jslots) {
        rc = fail(s, -E2BIG, "A file past the new end has names in more folders than one step can rewrite");
        goto out;
    }
    int steps = 0;
    for (k = 0; steps == 0 || k < n; steps++) {
        if (!t3_txn_begin(sbi, (int)sbi->jslots)) { rc = fail(s, -EIO, "the journal cannot be written"); goto out; }
        if (steps == 0 && (!t3_txn_stage_inode(sbi, nu, &node) || !t3_txn_stage_inode(sbi, old, 0) ||
                           !t3_txn_stage_alloc(sbi))) {
            t3_txn_reset(sbi);
            rc = fail(s, -EIO, "a renumbering did not fit one journal step -- the volume is as it was");
            goto out;
        }
        for (; k < n; k++) {
            const struct t3_ref *r = &refs[order[k]];
            if (!staged(sbi, r->blk) && sbi->txn_count >= (int)sbi->jslots) break;
            uint8_t *p = t3_txn_stage(sbi, r->blk);
            if (!p || rd32(p + r->off) != (uint32_t)old) {
                t3_txn_reset(sbi);
                rc = fail(s, -EIO, "a folder entry is not what it was a moment ago");
                goto out;
            }
            wr32(p + r->off, (uint32_t)nu);
        }
        if (!t3_txn_commit(sbi)) { rc = fail(s, -EIO, "a write failed -- the volume is as it was"); goto out; }
        if (steps == 0) t3_alloc_committed(sbi);
    }
    if (steps > 1)
        klog_printf("tfs3: shrink: folder inode %llu -> %llu took %d journal steps for its subfolders' `..`\n",
                    (unsigned long long)old, (unsigned long long)nu, steps);
out:
    kfree(order);
    t3_ncache_flush(sbi);
    t3_icache_drop(sbi);
    return rc;
}

static int count_doomed_inode(struct shrink *s, uint64_t ino) {
    if (ino / s->sbi->sb.ipg >= s->ng) s->count++;
    return 0;
}

static int move_any_blocks(struct shrink *s, uint64_t ino) { return move_inode_blocks(s, ino); }

// Renumber every inode in a dropped group. refs[] is sorted, so each
// inode's entries are the next run of it.
static int renumber_all(struct shrink *s) {
    struct t3_state *sbi = s->sbi;
    uint32_t r = 0;
    for (uint32_t g = s->ng; g < sbi->sb.gc; g++) {
        if ((sbi->gd[g].flags & T3_BG_INODE_UNINIT) || sbi->gd[g].free_inodes == sbi->sb.ipg) continue;
        for (uint32_t i = 0; i < sbi->sb.ipg; i++) {
            if (!t3_ibm_test(sbi, g, i)) continue;
            uint64_t ino = (uint64_t)g * sbi->sb.ipg + i;
            while (r < s->nrefs && s->refs[r].ino < ino) r++;
            uint32_t n = 0;
            while (r + n < s->nrefs && s->refs[r + n].ino == ino) n++;
            uint64_t nu = take_inode(sbi, 0, s->ng);
            if (!nu) return fail(s, -ENOSPC, "there are no free file slots left below the new end");
            int rc = renumber(s, ino, nu, s->refs + r, n);
            if (rc) return rc;
            r += n;
            fsck_step(s->live, ++s->done, 0);
        }
    }
    return 0;
}

// Phase 3: everything is below the new end; make it the end.
static int switch_size(struct shrink *s) {
    struct t3_state *sbi = s->sbi;
    const uint32_t ot = sbi->sb.total_blocks, og = sbi->sb.gc, L = s->last;
    const uint32_t old_end = t3_group_data_end(sbi, L);

    // Nothing may be left past it: the phases above say so, and this is
    // the last moment a wrong "so" costs nothing.
    for (uint32_t g = L + 1; g < og; g++) {
        uint32_t end = t3_group_data_end(sbi, g), data = end > sbi->meta_off ? end - sbi->meta_off : 0;
        if (sbi->gd[g].free_inodes != sbi->sb.ipg || sbi->gd[g].free_blocks != data)
            return fail(s, -EIO, "something is still past the new end -- the volume is as it was");
    }
    for (uint32_t i = s->last_end; i < old_end; i++)
        if (t3_bbm_test(sbi, L, i)) return fail(s, -EIO, "something is still past the new end -- the volume is as it was");

    uint32_t old_bk[2], old_bk_sb[2];
    int old_nbk = t3_backup_groups(og, old_bk);
    for (int i = 0; i < old_nbk; i++) old_bk_sb[i] = backup_sb_block(sbi, old_bk[i]);
    const int last_on_disk = !(sbi->gd[L].flags & T3_BG_BLOCK_UNINIT);
    uint8_t *img = kmalloc(T3_BLOCK), *lbm = last_on_disk ? kmalloc(T3_BLOCK) : 0;
    struct t3_gd *ngd = kzalloc(sizeof *ngd * s->ng), *ogd = sbi->gd;
    int rc = -EIO;
    if (!img || !ngd || (last_on_disk && !lbm)) { rc = fail(s, -ENOMEM, "out of memory"); goto free_only; }
    if (lbm && !t3_read_block(sbi, t3_group_base(sbi, L), lbm)) {
        fail(s, -EIO, "the last group's block bitmap cannot be read");
        goto free_only;
    }
    k_memcpy(ngd, ogd, sizeof *ngd * s->ng);
    uint32_t used = 0;
    if (lbm) {
        for (uint32_t i = sbi->meta_off; i < s->last_end; i++) used += bit(lbm, i);
        for (uint32_t i = s->last_end; i < T3_BPG; i++) set_bit(lbm, i, 1);
    }
    ngd[L].free_blocks = s->last_end - sbi->meta_off - used;

    sbi->sb.total_blocks = s->nt;
    sbi->sb.gc = s->ng;
    sbi->gd = ngd;
    fsck_stage(s->live, 3, 3);
    {
        uint8_t *p;
        if (!t3_txn_begin(sbi, 3)) goto unswitch;
        if (!(p = t3_txn_stage(sbi, sbi->gdt_block + L / T3_GD_PER_BLOCK))) { t3_txn_reset(sbi); goto unswitch; }
        pack_gdt_block(sbi, L / T3_GD_PER_BLOCK, p);
        if (lbm) {
            if (!(p = t3_txn_stage(sbi, t3_group_base(sbi, L)))) { t3_txn_reset(sbi); goto unswitch; }
            k_memcpy(p, lbm, T3_BLOCK);
        }
        if (!(p = t3_txn_stage(sbi, T3_SB_BLOCK))) { t3_txn_reset(sbi); goto unswitch; }
        t3_pack_superblock(sbi, p);
        if (!t3_txn_commit(sbi)) {
            if (sbi->readonly) {
                why(s->out, "interrupted after the switch -- it completes when the volume is next mounted");
                goto out;
            }
            goto unswitch;
        }
        fsck_step(s->live, 3, 0);
    }
    // The cached bitmaps describe the old geometry's last group.
    t3_bmap_drop(sbi);

    // The backups, all rewritten: each holds a superblock that still says
    // the old size. The old ones outside every new region are erased --
    // past the new end now, where a stale one could claim the disk.
    uint32_t new_bk[2];
    int new_nbk = t3_backup_groups(s->ng, new_bk);
    fsck_stage(s->live, 4, (uint32_t)(old_nbk + new_nbk));
    {
        uint32_t done = 0;
        uint8_t zero[T3_SECTOR];
        k_memset(zero, 0, sizeof zero);
        for (int i = 0; i < new_nbk; i++) {
            if (!write_backup(sbi, backup_sb_block(sbi, new_bk[i]), img))
                klog_write(KLOG_ERR "tfs3: shrink: a backup region could not be written -- fsck repair rewrites it\n");
            fsck_step(s->live, ++done, 0);
        }
        for (int i = 0; i < old_nbk; i++) {
            int kept = 0;
            for (int j = 0; j < new_nbk; j++) {
                uint32_t sb = backup_sb_block(sbi, new_bk[j]);
                if (old_bk_sb[i] >= sb - sbi->gdt_blocks && old_bk_sb[i] <= sb) kept = 1;
            }
            if (!kept && !t3_vol_write_sectors(sbi, (uint64_t)old_bk_sb[i] * T3_SPB, 1, zero))
                klog_write(KLOG_ERR "tfs3: shrink: an old backup superblock could not be erased\n");
            fsck_step(s->live, ++done, 0);
        }
    }
    blkdev_flush(sbi->vol.dev);
    klog_printf("tfs3: shrank from %u to %u blocks (%u -> %u groups), %u blocks moved, %u inodes renumbered\n",
                ot, s->nt, og, s->ng, s->moved, s->count);
    rc = 0;
    goto out;

unswitch:
    fail(s, -EIO, "a write failed -- the volume is as it was");
    sbi->sb.total_blocks = ot;
    sbi->sb.gc = og;
    sbi->gd = ogd;
    ogd = ngd;
    ngd = 0;
    goto free_only;
out:
    ngd = 0;           // sbi->gd now; unmount_state() frees it
free_only:
    if (sbi->gd != ogd) kfree(ogd);
    kfree(ngd);
    kfree(lbm);
    kfree(img);
    return rc;
}

static void shrink_geometry(struct shrink *s, struct t3_state *sbi, uint32_t nt) {
    s->sbi = sbi;
    s->nt = nt;
    s->ng = groups_for(sbi, nt);
    s->last = s->ng - 1;
    s->last_end = nt - t3_group_base(sbi, s->last) - t3_backup_blocks(sbi);
    s->cur_g = 0;
    s->cur_i = sbi->meta_off;
}

// Shrink the open volume to `nt` blocks (>= shrink_floor(), normalised).
static int shrink(struct t3_state *sbi, uint32_t nt, struct fs_resize_info *out) {
    struct shrink s;
    k_memset(&s, 0, sizeof s);
    shrink_geometry(&s, sbi, nt);
    s.out = out;
    s.live = fs_format_live();
    s.buf = kmalloc(T3_BLOCK);
    if (!s.buf) return fail(&s, -ENOMEM, "out of memory");
    fsck_begin(s.live, 0, T3_SHRINK_STAGES, 5);
    int rc;

    // 0. The check: every phase below trusts the bitmaps and the tree.
    fsck_stage(s.live, 0, 1);
    struct fs_check_result cr;
    int checked = tfs3_check(sbi, 0, &cr, 0);
    if (checked != 1 || fs_check_problems(&cr)) {
        rc = fail(&s, -EIO, checked == 1 ? "It has errors -- check and repair it first"
                                         : "It could not be checked -- nothing was changed");
        goto out;
    }
    fsck_step(s.live, 1, 0);

    // 1. Blocks. How many lie past the end, from the counts and the last
    // group's bitmap: no walk at all when there are none.
    uint32_t to_move = 0;
    for (uint32_t g = s.last; g < sbi->sb.gc; g++) {
        uint32_t end = t3_group_data_end(sbi, g);
        if (g > s.last) to_move += (end > sbi->meta_off ? end - sbi->meta_off : 0) - sbi->gd[g].free_blocks;
        else for (uint32_t i = s.last_end; i < end; i++) to_move += t3_bbm_test(sbi, g, i);
    }
    fsck_stage(s.live, 1, to_move);
    if (to_move && (rc = each_inode(&s, move_any_blocks))) goto out;

    // 2. Inodes: the entries naming them first -- every block is where
    // it stays now -- then each renumbered with its run of them.
    each_inode(&s, count_doomed_inode);
    uint32_t to_renumber = s.count;
    fsck_stage(s.live, 2, to_renumber);
    s.done = 0;
    if (to_renumber) {
        g_ref_lo = (uint64_t)s.ng * sbi->sb.ipg;
        g_ref_hi = (uint64_t)sbi->sb.gc * sbi->sb.ipg;
        if ((rc = each_inode(&s, collect_refs))) goto out;
        sort_refs(s.refs, s.nrefs);
        if ((rc = renumber_all(&s))) goto out;
    }

    // 3. The switch, and the backups.
    rc = switch_size(&s);
out:
    fsck_end(s.live);
    kfree(s.refs);
    kfree(s.buf);
    return rc;
}

// KTEST: put inode `ino` in the lowest free slot of group `g`, its names
// following -- how a test gets files past a shrink's new end without
// filling group 0's thousands of inode slots first. On a MOUNTED volume
// the caller keeps still.
int t3_move_inode_to_group(struct t3_state *sbi, uint64_t ino, uint32_t g) {
    struct fs_resize_info info;
    struct shrink s;
    k_memset(&info, 0, sizeof info);
    k_memset(&s, 0, sizeof s);
    shrink_geometry(&s, sbi, sbi->sb.total_blocks);
    s.out = &info;
    s.buf = kmalloc(T3_BLOCK);
    if (!s.buf || !t3_txn_flush_deferred(sbi)) { kfree(s.buf); return -ENOMEM; }
    g_ref_lo = ino;
    g_ref_hi = ino + 1;
    int rc = each_inode(&s, collect_refs);
    uint64_t nu = rc ? 0 : take_inode(sbi, g, g + 1);
    if (!rc && !nu) rc = -ENOSPC;
    if (!rc) rc = renumber(&s, ino, nu, s.refs, s.nrefs);
    if (rc) klog_printf("tfs3: moving inode %llu to group %u: %d %s\n", (unsigned long long)ino, g, rc, info.why);
    t3_rcache_drop(sbi);
    kfree(s.refs);
    kfree(s.buf);
    return rc;
}

// What the volume's blocks and inodes need, at size `nt` in blocks: does
// it all fit? The new last group holds a backup region, as every last
// group does.
static int fits(struct t3_state *sbi, uint32_t nt, uint64_t used_blocks, uint64_t used_inodes) {
    uint32_t ng = groups_for(sbi, nt), bk[2];
    if ((uint64_t)ng * sbi->sb.ipg < used_inodes) return 0;
    int nbk = t3_backup_groups(ng, bk);
    uint64_t room = 0;
    for (uint32_t g = 0; g < ng; g++) {
        uint32_t span = nt - t3_group_base(sbi, g);
        if (span > T3_BPG) span = T3_BPG;
        for (int i = 0; i < nbk; i++) if (bk[i] == g) span = span > t3_backup_blocks(sbi) ? span - t3_backup_blocks(sbi) : 0;
        room += span > sbi->meta_off ? span - sbi->meta_off : 0;
    }
    return room >= used_blocks;
}

// A size the format allows: a last group shorter than T3_MIN_GROUP_BLOCKS
// is left out, as a grow leaves one out.
static uint32_t whole(const struct t3_state *sbi, uint32_t nt) {
    uint32_t rem = (nt - sbi->group0) % T3_BPG;
    return nt - sbi->group0 > T3_BPG && rem && rem < T3_MIN_GROUP_BLOCKS ? nt - rem : nt;
}

// The smallest size, in blocks, that holds everything with the descriptor
// table this volume has (group 0 starts after it, so it cannot change) --
// resize2fs -M's question. The volume's own size when nothing is smaller.
static uint32_t shrink_floor(struct t3_state *sbi) {
    uint64_t used_blocks = 0, used_inodes = (uint64_t)sbi->sb.gc * sbi->sb.ipg;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        uint32_t end = t3_group_data_end(sbi, g);
        used_blocks += (end > sbi->meta_off ? end - sbi->meta_off : 0) - sbi->gd[g].free_blocks;
        used_inodes -= sbi->gd[g].free_inodes;
    }
    uint32_t lo = sbi->group0 + T3_MIN_GROUP_BLOCKS, hi = sbi->sb.total_blocks;
    if (sbi->gdt_blocks > T3_GDT_BLOCKS) {
        uint64_t past = (uint64_t)T3_V3_GDT + (uint64_t)(sbi->gdt_blocks - 1) * T3_GD_PER_BLOCK * T3_BPG + 1;
        if (past > lo) lo = past > hi ? hi : (uint32_t)past;
    }
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (fits(sbi, whole(sbi, mid), used_blocks, used_inodes)) hi = mid; else lo = mid + 1;
    }
    return whole(sbi, lo);
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
                   "This tfs3 volume is format version %u; only version 3 can be resized", sbi->sb.version);
        return 0;
    }
    if (sbi->readonly) { why(out, "It is read-only after an error -- check and repair it first"); return 0; }
    if (sbi->mounted_from_backup) { why(out, "Its main superblock is damaged -- repair it first"); return 0; }
    out->max_bytes = (uint64_t)grow_limit(sbi) * T3_BLOCK;
    if (grow_limit(sbi) > sbi->sb.total_blocks) out->flags = FS_RESIZE_CAN_GROW;
    uint32_t floor = shrink_floor(sbi);
    if (floor < sbi->sb.total_blocks) {
        out->min_bytes = (uint64_t)floor * T3_BLOCK;
        out->flags |= FS_RESIZE_CAN_SHRINK;
    }
    if (!(out->flags & FS_RESIZE_CAN_SHRINK))
        why(out, "Its files fill it: there is no room to shrink into");
    else if (!(out->flags & FS_RESIZE_CAN_GROW))
        why(out, "It is as large as tfs3 grows without moving every group");
    else
        out->why[0] = 0;
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
        if (!can || !(out->flags & FS_RESIZE_CAN_SHRINK)) { rc = -ENOTSUP; goto close; }
        if (want * T3_BLOCK < out->min_bytes) {
            k_snprintf(out->why, sizeof out->why, "Its files need %llu MiB at least",
                       (unsigned long long)((out->min_bytes + (1u << 20) - 1) >> 20));
            rc = -ENOSPC;
            goto close;
        }
        uint32_t nt = whole(sbi, (uint32_t)want);
        if (nt < out->min_bytes / T3_BLOCK) nt = (uint32_t)(out->min_bytes / T3_BLOCK);
        out->why[0] = 0;
        rc = shrink(sbi, nt, out);
        if (!rc) {
            describe(sbi, out);
            out->why[0] = 0;
        }
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
// and grown again; shrunk with files and inodes past the new end; and a
// grow and a shrink cut short at every one of their writes.
