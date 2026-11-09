// TFS3's write side: the write core, partial truncation, dirent editing,
// and the mutating fs_ops built on them. Split out of tfs3.c;
// tfs3_internal.h has the map.

#include "tfs3_internal.h"
#include "string.h"
#include "storage_config.h" // storage.sync -- whether the barriers are real
#include "klog.h"
#include "heap.h"
#include "errno.h"   // -EEXIST from create_excl

// ---- write core ------------------------------------------------------------

// Write [offset, offset+len) into the file behind *node (whose inode
// number is `ino`), allocating as needed, then commit the updated
// inode through the journal. The order is the leak-safe one: bitmap
// state flushes BEFORE the inode transaction makes anything
// reachable. Returns 1/0.
// Can a partially-written block hold bytes worth preserving? The
// question is about the BLOCK, not about where the write starts.
//
// Asking `file_off >= size` looks equivalent and is catastrophically
// not: an APPEND starts exactly at size, so that test is true every
// time and zeroes the whole block, destroying what is already in it.
// The skip is only safe when the block is freshly allocated or BEGINS
// past end-of-file. Both write paths ask here -- the stepped one
// carried its own copy of the wrong test.
static int block_has_live_bytes(int fresh, uint32_t bi, uint64_t size) {
    return !fresh && (uint64_t)bi * T3_BLOCK < size;
}

// The block holding end-of-file, read for a partial write: zero what lies
// past `size` and outside [within, within + chunk). A write that failed
// after landing here, or bytes a shrink left, must not read back as data
// once the file grows over them.
static void zero_past_eof(uint8_t *blk, uint32_t bi, uint64_t size, uint32_t within, uint32_t chunk) {
    uint64_t start = (uint64_t)bi * T3_BLOCK;
    if (start + T3_BLOCK <= size) return;
    uint32_t eof = size > start ? (uint32_t)(size - start) : 0;
    if (within > eof) k_memset(blk + eof, 0, within - eof);
    uint32_t end = within + chunk > eof ? within + chunk : eof;
    if (end < T3_BLOCK) k_memset(blk + end, 0, T3_BLOCK - end);
}

// The file grows from its size to past `from`: the tail of the block
// holding EOF becomes file data and must read as zeros. A write that
// STARTS in that block zeroes it itself (zero_past_eof()), so `from` in
// that block skips it; a truncate, which writes nothing, passes its size.
static int zero_old_tail(struct t3_state *sbi, const struct t3_inode *node, uint64_t from, int writes) {
    uint32_t tail = (uint32_t)(node->size % T3_BLOCK);
    uint32_t bi = (uint32_t)(node->size / T3_BLOCK);
    if (!tail || from <= node->size || (writes && from / T3_BLOCK == bi)) return 1;
    uint32_t blk;
    if (!t3_block_for_index(sbi, node, bi, &blk)) return 0;
    if (!blk) return 1;
    if (!t3_read_block(sbi, blk, sbi->blk)) return 0;
    k_memset(sbi->blk + tail, 0, T3_BLOCK - tail);
    return t3_write_block(sbi, blk, sbi->blk);
}


static int cut_past_eof(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint64_t grow_to);

// Which pointer table maps file-block `idx` -- 0 the inode itself.
static inline uint32_t leaf_of(uint32_t idx) {
    return idx < 12 ? 0 : 1 + (idx - 12) / T3_PTRS_PER_BLOCK;
}

static int do_write_inner(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint64_t offset,
                          const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t prefer_group = (uint32_t)(ino / sbi->sb.ipg);
    uint32_t last_alloc = 0;
    uint32_t total = 0;
    if (len && !zero_old_tail(sbi, node, offset, 1)) return 0;

    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > len - total) chunk = len - total;

        uint32_t leaf_blk, leaf_slot, existing;
        if (!t3_map_get_or_alloc_tables(sbi, node, bi, prefer_group, &leaf_blk, &leaf_slot, &existing))
            return 0;
        uint32_t blk = existing;
        int fresh = 0;
        if (!blk) {
            blk = t3_alloc_block(sbi, prefer_group, last_alloc);
            if (!blk) return 0;
            t3_map_set_block(sbi, node, leaf_blk, leaf_slot, blk);
            fresh = 1;
        }
        last_alloc = blk;

        if (chunk == T3_BLOCK) {
            // Run coalescing, the write half of what took TFS2 from 18
            // to 25 MB/s: gather the CONTIGUOUS on-disk run of full
            // blocks this write covers (the adjacent-first allocator
            // makes long runs the common case) and issue it as one
            // multi-sector transfer straight from the caller's buffer
            // -- no bounce, no per-block ATA round trip. Measured on
            // tfs3 before this existed: 4.6 MB/s; the per-block loop
            // was the whole gap.
            uint32_t run = 1;
            uint32_t want = (len - total) / T3_BLOCK; // whole blocks left
            // One transfer must fit the ATA path's per-command cap
            // (128 sectors on DMA, 8 on PIO -- ask, don't assume,
            // same rule TFS2's batching follows).
            uint32_t cap = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
            if (cap < 1) cap = 1;
            if (want > cap) want = cap;
            while (run < want) {
                // NOT INTO THE NEXT TABLE: mapping there flushes this one,
                // which names the run, and a table the journal cannot take
                // is written in place -- ahead of the data, were the run
                // still unwritten.
                if (leaf_of(bi + run) != leaf_of(bi)) break;
                uint32_t nleaf, nslot, nexist;
                if (!t3_map_get_or_alloc_tables(sbi, node, bi + run, prefer_group, &nleaf, &nslot, &nexist))
                    return 0;
                uint32_t nblk = nexist;
                if (!nblk) {
                    nblk = t3_alloc_block(sbi, prefer_group, last_alloc);
                    if (!nblk) return 0;
                    t3_map_set_block(sbi, node, nleaf, nslot, nblk);
                }
                if (nblk != last_alloc + 1) {
                    // Not contiguous: it's allocated and recorded, the
                    // next loop iteration will write it as its own run.
                    last_alloc = 0; // don't bias the next adjacency try
                    break;
                }
                last_alloc = nblk;
                run++;
            }
            if (!t3_vol_write_sectors(sbi, blk * T3_SPB, (int)(run * T3_SPB), src + total)) return 0;
            total += run * T3_BLOCK;
        } else {
            if (!block_has_live_bytes(fresh, bi, node->size)) k_memset(sbi->blk, 0, T3_BLOCK);
            else if (!t3_read_block(sbi, blk, sbi->blk)) return 0;
            else zero_past_eof(sbi->blk, bi, node->size, within, chunk);
            k_memcpy(sbi->blk + within, src + total, chunk);
            if (!t3_write_block(sbi, blk, sbi->blk)) return 0;
            total += chunk;
        }
    }
    // FLUSHED, NOT DROPPED. The next write syscall to this file walks
    // the same leaf and middle tables, and re-reading them was ~3 of
    // the ~6 metadata reads every 64 KiB write was issuing.
    if (!t3_pcache_flush(sbi)) return 0;

    if (offset + len > node->size) node->size = offset + len;
    t3_touch(node);
    return t3_stage_inode_update(sbi, ino, node);
}

// The inode half of a write: land the updated inode through the journal
// (or leave it staged under `batched`), after the allocation state it
// depends on. Shared by do_write_inner() and do_overwrite().
int t3_stage_inode_update(struct t3_state *sbi, uint64_t ino, struct t3_inode *node) {
    // SET-BEFORE-USE, and under `batched` it rides the next commit
    // instead. Deferring is strictly SAFER than flushing per write: a
    // crash mid-batch then leaves the bitmap saying `free` and the
    // inode unchanged -- consistent -- where a per-write flush leaves
    // blocks marked used by an inode update that never landed, which is
    // the leak fsck exists to reclaim. t3_txn_commit() keeps the
    // ordering by doing it before the commit.
    // ONE read of the mode: a `config set storage.sync` between two would
    // skip the flush here and then commit -- an inode before its bitmaps.
    int batched = storage_sync_batched();
    if (!batched && !t3_flush_alloc_state(sbi)) return 0;

    // BATCHED: the handle STOPS rather than commits, so many writes share
    // one commit. Re-staging the same inode block returns the SAME image
    // (txn_stage), which t3_txn_stage_inode() patches in place -- so a
    // second write to the same file needs no slot of its own.
    // Under `batched` staging is this handle's LAST step that can fail, so
    // nothing it patched ever needs putting back: no undo copy, which would
    // otherwise be a 4 KiB copy on every write to a file whose inode is
    // staged. NOT otherwise: the commit below can abort, and a block an
    // earlier op staged must then get its own image back.
    if (!t3_txn_begin_or_join(sbi, 1)) return 0;   // pointer tables may have opened it
    if (batched) sbi->txn_hnoundo = 1;
    if (!t3_txn_stage_inode(sbi, ino, node)) { t3_txn_reset(sbi); return 0; }
    if (batched) return t3_txn_stop(sbi);   // durable at the next commit
    return t3_txn_commit_last(sbi);                  // the commit point: file grows atomically
}

// Is [offset, offset+len) entirely blocks the file already has, inside
// its size? Then the write allocates nothing and changes no pointer --
// the one shape that may drop the lock (do_overwrite()).
static int is_overwrite(struct t3_state *sbi, const struct t3_inode *node,
                        uint64_t offset, uint32_t len) {
    if (!len || offset + len > node->size) return 0;
    uint32_t first = (uint32_t)(offset / T3_BLOCK);
    uint32_t last = (uint32_t)((offset + len - 1) / T3_BLOCK);
    for (uint32_t bi = first; bi <= last; bi++) {
        uint32_t b;
        if (!t3_block_for_index(sbi, node, bi, &b) || !b) return 0;
    }
    return 1;
}

// AN OVERWRITE IN PLACE, with the mount's lock dropped for each whole-
// block run (t3_vol_write_run()) -- ext4's direct-I/O overwrite, which it
// runs under a SHARED inode lock for the same reason: it allocates
// nothing and changes no pointer, so there is no half-built state for
// another call to see. Partial blocks go through blk, under the lock.
//
// **WHAT IT MUST NOT DO IS COMMIT ITS INODE COPY**, which another call
// may have changed while the lock was down -- an append's size, or a
// truncate. So it re-reads the inode at the end and changes only the
// time. And after each gap a block freed means this copy may be stale,
// so it re-reads and checks the rest of the range is still an overwrite,
// or fails. The inode itself cannot be freed meanwhile: this op holds it
// exclusive, and a delete and an exclusion both wait for that.
static int do_overwrite(struct t3_state *sbi, uint64_t ino, struct t3_inode *node,
                        uint64_t offset, const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t freed = sbi->free_gen, staged = sbi->ino_gen;
    uint32_t total = 0;
    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > len - total) chunk = len - total;
        uint32_t blk;
        if (!t3_block_for_index(sbi, node, bi, &blk) || !blk) return 0;
        if (chunk == T3_BLOCK) {
            uint32_t run = 1;
            uint32_t want = (len - total) / T3_BLOCK;
            uint32_t cap = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
            if (cap < 1) cap = 1;
            if (want > cap) want = cap;
            while (run < want) {
                uint32_t nxt;
                if (!t3_block_for_index(sbi, node, bi + run, &nxt) || nxt != blk + run) break;
                run++;
            }
            int r = t3_vol_write_run(sbi, blk * T3_SPB, (int)(run * T3_SPB), src + total);
            if (r <= 0) return 0;          // failed, or unmounted: sbi may be gone
            total += run * T3_BLOCK;
            if (sbi->free_gen != freed) {
                freed = sbi->free_gen;
                if (!t3_read_inode(sbi, ino, node) || node->type != T3_TYPE_FILE) return 0;
                if (total < len && !is_overwrite(sbi, node, offset + total, len - total)) return 0;
            }
        } else {
            if (!t3_read_block(sbi, blk, sbi->blk)) return 0;
            k_memcpy(sbi->blk + within, src + total, chunk);
            if (!t3_write_block(sbi, blk, sbi->blk)) return 0;
            total += chunk;
        }
    }
    if (sbi->ino_gen != staged && (!t3_read_inode(sbi, ino, node) || node->type != T3_TYPE_FILE))
        return 0;
    t3_touch(node);
    return t3_stage_inode_update(sbi, ino, node);
}

// The rollback shell: a runtime failure (refused write, out of space
// midway) frees everything this call allocated -- see the alloc-log
// comment. Only a CRASH is allowed to cost a leak.
static int do_write(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint64_t offset,
                    const void *buf, uint32_t len) {
    // BEFORE anything is allocated: a refusal that came after would
    // leave the blocks behind for a write that never happened.
    if (!t3_range_fits(offset, len)) return 0;
    // NOTHING TO ROLL BACK, AND NO SHARED LOG TO HOLD across the gaps it
    // may open: alog is per mount, and another call would reset it.
    if (is_overwrite(sbi, node, offset, len)) return do_overwrite(sbi, ino, node, offset, buf, len);
    if (len && !cut_past_eof(sbi, ino, node, offset)) return 0;
    // ROOM FOR THE TABLES IT MAY JOURNAL (t3_txn_stage_table()), made now:
    // mid-walk nothing may commit, and a table that does not fit costs a
    // barrier instead. A file with no indirect table has none to journal.
    if ((node->ptrs[12] | node->ptrs[13] | node->ptrs[14]) && sbi->txn_deferred &&
        sbi->txn_count + T3_TABLE_CREDITS > (int)sbi->jslots)
        t3_txn_flush_deferred(sbi);   // a failure leaves the barrier path
    t3_alog_begin(sbi);
    if (do_write_inner(sbi, ino, node, offset, buf, len)) {
        t3_alog_commit(sbi);
        return 1;
    }
    t3_alog_rollback(sbi);   // out of space with deletes pending: run again
    return 0;
}

// Free every data + pointer block behind *node (for truncate/delete),
// TRIMming as it goes, and leave the pointer fields zeroed. The
// caller must have ALREADY committed the inode/dirent transaction
// that makes these blocks unreachable -- clear-after-persist.
//
// THE POINTER-TABLE READS DROP THE VOLUME'S LOCK. A 512 MiB file has
// ~130 tables, and reading them locked made every other call on the
// volume wait out ~130 disk commands: a stat on / waited 28-41 ms on
// the laptops (docs/bugs.md). The file is past its commit and its inode
// locked, and its blocks stay allocated until freed here, so nobody
// writes a table while it is read -- the argument fslock stage 3a makes
// for data reads. ext4 and XFS likewise hold only the inode's and the
// allocation group's locks across a truncate, never the volume's.
//
// Each table is read in a gap and freed straight after, under the lock.
// THE QUEUED TRIMS MAY CROSS A GAP: a block freed here and reallocated
// in one could only be reused through t3_alloc_block()/alloc_block_at(),
// and both flush the queue before handing a block out -- so a discard
// never lands on a reused block, and a big delete stays one TRIM list.
// After a gap that reports the mount gone (-1), sbi is not touched again.
// Returns 1, or -1 for that.
static void free_tree_level(struct t3_state *sbi, uint32_t table_blk, int depth);

// A table block, after what it maps. `tbl` NULL: it could not be read,
// so what it maps leaks -- fsck reclaims -- as free_tree_level() does.
static void free_leaf(struct t3_state *sbi, uint32_t blk, const uint8_t *tbl) {
    uint32_t run_start = 0, run_len = 0;
    for (uint32_t i = 0; tbl && i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(tbl + i * 4);
        if (!e) continue;
        t3_free_block_bit(sbi, e);
        if (run_len && e == run_start + run_len) run_len++;
        else { t3_trim_run(sbi, run_start, run_len); run_start = e; run_len = 1; }
    }
    t3_trim_run(sbi, run_start, run_len);
    t3_free_block_bit(sbi, blk);
    t3_trim_run(sbi, blk, 1);
}

// Every leaf a depth-1 table `mid` lists. 1, or -1: the mount went.
static int free_leaves_of(struct t3_state *sbi, const uint8_t *mid, uint8_t *leaf) {
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(mid + i * 4);
        if (!e) continue;
        int g = t3_read_block_unlocked(sbi, e, leaf);
        if (g < 0) return -1;
        free_leaf(sbi, e, g ? leaf : 0);
    }
    return 1;
}

// The table `blk` at `depth` (0 a leaf, 1 a middle table, 2 the top) and
// everything under it, each table read in a lock gap. `buf` is three
// blocks: a leaf, a middle table, the top. 1, or -1: the mount went.
static int free_level_unlocked(struct t3_state *sbi, uint32_t blk, int depth, uint8_t *buf) {
    uint8_t *leaf = buf, *mid = buf + T3_BLOCK, *top = buf + 2 * T3_BLOCK;
    int g;
    if (depth == 0) {
        if ((g = t3_read_block_unlocked(sbi, blk, leaf)) < 0) return -1;
        free_leaf(sbi, blk, g ? leaf : 0);
        return 1;
    }
    if (depth == 1) {
        if ((g = t3_read_block_unlocked(sbi, blk, mid)) < 0) return -1;
        if (g && free_leaves_of(sbi, mid, leaf) < 0) return -1;
        free_leaf(sbi, blk, 0);
        return 1;
    }
    if ((g = t3_read_block_unlocked(sbi, blk, top)) < 0) return -1;
    for (uint32_t i = 0; g && i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(top + i * 4);
        if (!e) continue;
        int gm = t3_read_block_unlocked(sbi, e, mid);
        if (gm < 0) return -1;
        if (gm && free_leaves_of(sbi, mid, leaf) < 0) return -1;
        free_leaf(sbi, e, 0);
    }
    free_leaf(sbi, blk, 0);
    return 1;
}

static int free_all_blocks(struct t3_state *sbi, struct t3_inode *node) {
    uint32_t run_start = 0, run_len = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t blk = node->ptrs[i];
        if (blk) {
            t3_free_block_bit(sbi, blk);
            if (run_len && blk == run_start + run_len) run_len++;
            else { t3_trim_run(sbi, run_start, run_len); run_start = blk; run_len = 1; }
        }
        node->ptrs[i] = 0;
    }
    t3_trim_run(sbi, run_start, run_len);

    uint8_t *buf = kmalloc(3 * T3_BLOCK);   // a leaf, a middle table, the top
    if (!buf) {
        // No memory: the old walk, every read locked.
        if (node->ptrs[12]) free_tree_level(sbi, node->ptrs[12], 0);
        if (node->ptrs[13]) free_tree_level(sbi, node->ptrs[13], 1);
        if (node->ptrs[14]) free_tree_level(sbi, node->ptrs[14], 2);
        k_memset(node->ptrs + 12, 0, 3 * sizeof node->ptrs[0]);
        t3_trim_flush(sbi);
        return 1;
    }
    uint32_t top[3] = { node->ptrs[12], node->ptrs[13], node->ptrs[14] };
    k_memset(node->ptrs + 12, 0, 3 * sizeof node->ptrs[0]);
    for (int d = 0; d < 3; d++) {
        if (top[d] && free_level_unlocked(sbi, top[d], d, buf) < 0) {
            kfree(buf);   // the mount went while unlocked: sbi is freed, touch nothing
            return -1;
        }
    }
    t3_trim_flush(sbi);
    kfree(buf);
    return 1;
}

// WHAT free_all_blocks() WOULD FREE, listed into `rec` instead, for a
// batched delete whose frees wait for its commit (struct t3_pfree). The
// tables are read with the lock dropped, as there -- the inode is locked
// and its blocks still marked used, so nobody writes one -- which is why
// it runs BEFORE the op opens its handle, and why the op must not trust
// its per-mount path buffers afterwards. A table that cannot be read
// leaks what it maps, as there. 1; 0 out of memory (the caller commits
// and frees the old way); -1 the mount went.
static int collect_leaf(struct t3_pfree *rec, uint32_t blk, const uint8_t *tbl) {
    for (uint32_t i = 0; tbl && i < T3_PTRS_PER_BLOCK; i++)
        if (!t3_pf_add(rec, rd32(tbl + i * 4))) return 0;
    return t3_pf_add(rec, blk);
}

static int collect_leaves_of(struct t3_state *sbi, struct t3_pfree *rec, const uint8_t *mid,
                             uint8_t *leaf) {
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(mid + i * 4);
        if (!e) continue;
        int g = t3_read_block_unlocked(sbi, e, leaf);
        if (g < 0) return -1;
        if (!collect_leaf(rec, e, g ? leaf : 0)) return 0;
    }
    return 1;
}

static int collect_frees(struct t3_state *sbi, const struct t3_inode *node, struct t3_pfree *rec) {
    for (int i = 0; i < 12; i++)
        if (!t3_pf_add(rec, node->ptrs[i])) return 0;
    uint32_t single = node->ptrs[12], dbl = node->ptrs[13], tri = node->ptrs[14];
    if (!single && !dbl && !tri) return 1;
    uint8_t *buf = kmalloc(3 * T3_BLOCK);   // a leaf, a middle table, the top
    if (!buf) return 0;
    uint8_t *leaf = buf, *mid = buf + T3_BLOCK, *top = buf + 2 * T3_BLOCK;
    int r = 1, g;
    if (single) {
        if ((g = t3_read_block_unlocked(sbi, single, leaf)) < 0) r = -1;
        else if (!collect_leaf(rec, single, g ? leaf : 0)) r = 0;
    }
    if (r > 0 && dbl) {
        if ((g = t3_read_block_unlocked(sbi, dbl, mid)) < 0) r = -1;
        else if (g) r = collect_leaves_of(sbi, rec, mid, leaf);
        if (r > 0 && !t3_pf_add(rec, dbl)) r = 0;
    }
    if (r > 0 && tri) {
        if ((g = t3_read_block_unlocked(sbi, tri, top)) < 0) r = -1;
        for (uint32_t i = 0; r > 0 && g > 0 && i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(top + i * 4);
            if (!e) continue;
            int gm = t3_read_block_unlocked(sbi, e, mid);
            if (gm < 0) { r = -1; break; }
            if (gm) r = collect_leaves_of(sbi, rec, mid, leaf);
            if (r > 0 && !t3_pf_add(rec, e)) r = 0;
        }
        if (r > 0 && !t3_pf_add(rec, tri)) r = 0;
    }
    kfree(buf);   // the mount went (-1): this was ours, sbi is not touched
    return r;
}

// A batched delete's list, or 0 when it cannot be had (out of memory): the
// caller then commits and frees after, as under `strict`. *gone is set when
// the mount went in a gap.
static struct t3_pfree *pending_frees(struct t3_state *sbi, uint64_t ino,
                                      const struct t3_inode *node, int *gone) {
    *gone = 0;
    struct t3_pfree *rec = t3_pf_new(ino);
    if (!rec) return 0;
    int r = collect_frees(sbi, node, rec);
    if (r > 0) return rec;
    t3_pf_free(rec);
    if (r < 0) *gone = 1;
    return 0;
}

// depth 0: entries are data blocks; deeper: entries are tables.
// Recursion depth is bounded at 3 by the format itself. Uses a local
// table copy (kmalloc) instead of the shared scratch because levels
// nest.
static void free_tree_level(struct t3_state *sbi, uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl) return; // leak rather than corrupt -- fsck reclaims
    if (t3_read_block(sbi, table_blk, tbl)) {
        uint32_t run_start = 0, run_len = 0;
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(tbl + i * 4);
            if (!e) continue;
            if (depth == 0) {
                t3_free_block_bit(sbi, e);
                if (run_len && e == run_start + run_len) run_len++;
                else { t3_trim_run(sbi, run_start, run_len); run_start = e; run_len = 1; }
            } else {
                free_tree_level(sbi, e, depth - 1);
            }
        }
        t3_trim_run(sbi, run_start, run_len);
    }
    t3_free_block_bit(sbi, table_blk);
    t3_trim_run(sbi, table_blk, 1);
    kfree(tbl);
}

// ---- partial truncation ----------------------------------------------------
//
// Shrinking has to obey the same ordering everything else here does:
// the inode that stops referencing a block must be DURABLE before that
// block's bit is freed, or a crash in between leaves a live file
// pointing at space the allocator is free to hand to someone else --
// the double allocation this filesystem's whole discipline exists to
// make impossible. (A leak in the other direction is fine; fsck
// reclaims it.)
//
// That splits truncation into two phases with a commit between them,
// and the phases can't both walk the pointer tables from disk, because
// phase one rewrites them. What saves it is that the tail being
// dropped is a clean cut: at every level, entries are either wholly
// dropped or wholly kept, EXCEPT for at most one straddling entry --
// so there is at most one partially-rewritten table per level, three
// in the deepest case. Keeping those three original images in memory
// (12 KiB) is enough for phase two to walk everything phase one
// detached.
struct t3_trunc {
    uint8_t *img[3];   // original image of the boundary table at each depth
    uint32_t blk[3];   // where it lives
    uint32_t from[3];  // table-relative index of the cut
    int used[3];
    int emptied[3];    // nothing kept -- the table block goes too
    int failed;        // a boundary table could not be cut: the truncate is refused
    uint32_t ptrs[15]; // the inode's original pointers
};

// Phase one at one level: zero every entry covering table-relative
// index >= `from`, write the table back, and record the original for
// phase two. Returns 1 if the table kept nothing (the caller drops the
// entry pointing at it), 0 otherwise -- including on any allocation or
// I/O failure, where keeping the tail attached is the safe answer.
static int trunc_detach(struct t3_state *sbi, struct t3_trunc *tr, uint32_t table_blk, int depth, uint32_t from) {
    if (depth < 0 || depth > 2 || tr->used[depth]) return 0; // one boundary per level, by construction
    uint8_t *orig = kmalloc(T3_BLOCK);
    if (!orig) { tr->failed = 1; return 0; }
    if (!t3_read_block(sbi, table_blk, orig)) { kfree(orig); tr->failed = 1; return 0; }

    uint8_t *edit = kmalloc(T3_BLOCK);
    if (!edit) { kfree(orig); tr->failed = 1; return 0; }
    k_memcpy(edit, orig, T3_BLOCK);

    uint32_t span = 1;
    for (int d = 0; d < depth; d++) span *= T3_PTRS_PER_BLOCK;

    int empty = 1, dirty = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(orig + i * 4);
        if (!e) continue;
        uint32_t start = i * span; // first file-block index this entry covers
        if (start + span <= from) { empty = 0; continue; }      // wholly kept
        if (start >= from) { wr32(edit + i * 4, 0); dirty = 1; } // wholly dropped
        else if (trunc_detach(sbi, tr, e, depth - 1, from - start)) { // straddles
            wr32(edit + i * 4, 0); dirty = 1;
        } else {
            empty = 0;
        }
    }
    // An emptied table is dropped whole by the caller, so writing it
    // would be a write to a block about to be freed. A kept one is
    // JOURNALED with the size, in the truncate's handle: a table can be
    // staged by an earlier write, and that commit would put back the
    // pointers an in-place write removed.
    if (dirty && !empty) {
        uint8_t *img = t3_txn_stage(sbi, table_blk);
        if (img) { k_memcpy(img, edit, T3_BLOCK); t3_rcache_drop(sbi); }
        else { empty = 0; tr->failed = 1; }
    }
    t3_map_cache_forget(sbi, table_blk);   // edited behind the write caches' backs
    kfree(edit);

    tr->img[depth] = orig;
    tr->blk[depth] = table_blk;
    tr->from[depth] = from;
    tr->used[depth] = 1;
    tr->emptied[depth] = empty;
    return empty;
}

// Detach every block holding file-block index >= `first` from *node
// (in memory and, for boundary tables, on disk). The caller must
// commit the inode before calling trunc_free().
static void trunc_begin(struct t3_state *sbi, struct t3_trunc *tr, struct t3_inode *node, uint32_t first) {
    k_memset(tr, 0, sizeof(*tr));
    for (int i = 0; i < 15; i++) tr->ptrs[i] = node->ptrs[i];

    for (uint32_t i = first; i < 12; i++) node->ptrs[i] = 0;

    uint32_t base = 12, span = T3_PTRS_PER_BLOCK;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint32_t slot = 12 + (uint32_t)lvl;
        uint32_t blk = node->ptrs[slot];
        if (blk) {
            if (first <= base) node->ptrs[slot] = 0;                 // whole level dropped
            else if (first < base + span &&
                     trunc_detach(sbi, tr, blk, lvl, first - base)) node->ptrs[slot] = 0;
        }
        base += span;
        span *= T3_PTRS_PER_BLOCK;
    }
}

// The truncate failed before its commit: the images go. Its table edits
// were staged in its handle, which the caller has reset -- nothing changed.
static void trunc_abort(struct t3_trunc *tr) {
    for (int d = 0; d < 3; d++) { kfree(tr->img[d]); tr->img[d] = 0; tr->used[d] = 0; }
}

// Phase two: return the detached blocks to the bitmap, walking the
// originals trunc_begin() kept. Every table read here is one phase one
// deliberately did NOT rewrite, so the disk still describes the
// subtree being freed.
// THE TABLES ARE READ IN LOCK GAPS, as a delete's are (free_all_blocks()):
// the shrink has committed and the inode is held, so nobody writes them.
// 1, or -1: the mount went meanwhile (touch nothing).
static int trunc_free_level(struct t3_state *sbi, uint32_t blk, int depth, uint8_t *buf) {
    if (!buf) { free_tree_level(sbi, blk, depth); return 1; }   // no memory: every read locked
    return free_level_unlocked(sbi, blk, depth, buf);
}

static int trunc_free(struct t3_state *sbi, struct t3_trunc *tr, uint32_t first, int locked) {
    uint32_t run_start = 0, run_len = 0;
    uint8_t *buf = locked ? 0 : kmalloc(3 * T3_BLOCK);   // none: every table read locked
    for (uint32_t i = first; i < 12; i++) {
        uint32_t blk = tr->ptrs[i];
        if (!blk) continue;
        t3_free_block_bit(sbi, blk);
        if (run_len && blk == run_start + run_len) run_len++;
        else { t3_trim_run(sbi, run_start, run_len); run_start = blk; run_len = 1; }
    }
    t3_trim_run(sbi, run_start, run_len);

    uint32_t base = 12, span = T3_PTRS_PER_BLOCK;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint32_t blk = tr->ptrs[12 + lvl];
        if (blk && first <= base && trunc_free_level(sbi, blk, lvl, buf) < 0) goto gone;   // the table too
        base += span;
        span *= T3_PTRS_PER_BLOCK;
    }

    for (int d = 2; d >= 0; d--) {
        if (!tr->used[d]) continue;
        // A TABLE THAT KEPT ITS OLD ENTRIES ON DISK still names what is past
        // the cut: those blocks stay allocated (fsck reclaims them).
        if (tr->failed) { kfree(tr->img[d]); tr->img[d] = 0; tr->used[d] = 0; continue; }
        uint32_t sp = 1;
        for (int k = 0; k < d; k++) sp *= T3_PTRS_PER_BLOCK;
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(tr->img[d] + i * 4);
            if (!e) continue;
            uint32_t start = i * sp;
            if (start < tr->from[d]) continue; // kept, or the next boundary down
            if (d == 0) { t3_free_block_bit(sbi, e); t3_trim_run(sbi, e, 1); }
            else if (trunc_free_level(sbi, e, d - 1, buf) < 0) goto gone;
        }
        if (tr->emptied[d]) { t3_free_block_bit(sbi, tr->blk[d]); t3_trim_run(sbi, tr->blk[d], 1); }
        kfree(tr->img[d]);
        tr->img[d] = 0;
        tr->used[d] = 0;
    }
    t3_trim_flush(sbi);
    kfree(buf);
    return 1;
gone:
    trunc_abort(tr);   // memory only: sbi is freed
    kfree(buf);
    return -1;
}

// Is anything mapped at a file-block index in [from, to) under the table
// `tbl` (`depth` 0 a leaf; indices relative to it)? A table that will not
// read counts as mapped, and so does one whose whole span is inside.
static int table_maps_in(struct t3_state *sbi, uint32_t tbl, int depth, uint32_t from, uint32_t to) {
    uint8_t *buf = kmalloc(T3_BLOCK);
    if (!buf || !t3_read_block(sbi, tbl, buf)) { kfree(buf); return 1; }
    uint32_t span = 1;
    for (int d = 0; d < depth; d++) span *= T3_PTRS_PER_BLOCK;
    int any = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK && !any; i++) {
        uint32_t e = rd32(buf + i * 4), start = i * span;
        if (!e || start + span <= from || start >= to) continue;
        any = (start >= from && start + span <= to) ||
              table_maps_in(sbi, e, depth - 1, from > start ? from - start : 0,
                            to - start < span ? to - start : span);
    }
    kfree(buf);
    return any;
}

static int mapped_in(struct t3_state *sbi, const struct t3_inode *node, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < 12 && i < to; i++) if (node->ptrs[i]) return 1;
    uint32_t base = 12, span = T3_PTRS_PER_BLOCK;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint32_t blk = node->ptrs[12 + lvl];
        if (blk && from < base + span && to > base &&
            ((from <= base && to >= base + span) ||
             table_maps_in(sbi, blk, lvl, from > base ? from - base : 0,
                           to - base < span ? to - base : span)))
            return 1;
        base += span;
        span *= T3_PTRS_PER_BLOCK;
    }
    return 0;
}

static int mapped_from(struct t3_state *sbi, const struct t3_inode *node, uint32_t first) {
    return mapped_in(sbi, node, first, 0xFFFFFFFFu);
}

// Detach everything at file-block index >= `first` and commit it with
// `size`, then free it. 1 done, 0 refused with nothing changed, -1
// committed but the mount went meanwhile (touch nothing) -- never when
// `locked`, which reads every table under the lock.
static int trunc_to(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint32_t first,
                    uint64_t size, int locked) {
    struct t3_trunc tr;
    struct t3_inode n = *node;
    // The inode and up to three boundary tables (trunc_detach()).
    if (!t3_txn_begin(sbi, sbi->jslots < 4 ? (int)sbi->jslots : 4)) return 0;
    trunc_begin(sbi, &tr, &n, first);
    // A CUT THAT FELL SHORT leaves the tail mapped under the smaller
    // size, and a regrow would read it back: refused instead.
    if (tr.failed) { t3_txn_reset(sbi); trunc_abort(&tr); return 0; }
    n.size = size;
    t3_touch(&n);
    if (!t3_txn_stage_inode(sbi, ino, &n)) { t3_txn_reset(sbi); trunc_abort(&tr); return 0; }
    if (!t3_txn_commit_last(sbi)) { trunc_abort(&tr); return 0; }
    *node = n;
    // Durable: nothing reachable references the tail any more.
    if (trunc_free(sbi, &tr, first, locked) < 0) return -1;
    t3_flush_alloc_state(sbi);
    return 1;
}

// BLOCKS CAN BE MAPPED PAST END-OF-FILE: a stepped write's tables ride
// other ops' commits before its inode does, and one abandoned -- or cut
// by a crash -- leaves them there. Harmless until the file grows over
// them without writing them, when they would read back as its data, so
// a grow that leaves a gap detaches them first (ext4 truncates a failed
// extending write's blocks instead; it holds the inode across the write).
// 1, or 0: the write is refused, nothing changed.
//
// AN OPEN STREAM ON THE FILE LOSES WHAT IT MAPPED THERE, and the cut may
// leave the inode as it found it: it fails at its next step.
static void wsteps_cut(struct t3_state *sbi, uint64_t ino);
static int cut_past_eof(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint64_t grow_to) {
    uint32_t first = (uint32_t)((node->size + T3_BLOCK - 1) / T3_BLOCK);
    if (grow_to / T3_BLOCK <= first || !mapped_from(sbi, node, first)) return 1;
    if (trunc_to(sbi, ino, node, first, node->size, 1) != 1) return 0;
    wsteps_cut(sbi, ino);
    return 1;
}

// ---- dirent editing (through the transaction) ------------------------------

// The transaction's view of a block, if it has one. Dirent editing
// must read through this: a rename stages an INSERT and then a REMOVE,
// and if the second read the block from disk it would compute its fold
// from record lengths the first had already changed -- swallowing the
// entry just written. (Found by reasoning about the same-directory
// case, where both edits land in one block.)
static const uint8_t *txn_peek(struct t3_state *sbi, uint32_t blk) {
    for (int i = 0; i < sbi->txn_count; i++) {
        if (sbi->txn_target[i] == blk) return sbi->txn_img[i];
    }
    return 0;
}

// Find room for a new dirent in `dir` and stage the patched block.
// Grows the directory by one block IN ITS OWN transaction first when
// needed (an empty extra dir block is harmless if the follow-up
// transaction never lands -- two consistent states, no 5-slot
// transaction; see the design doc's journal section).
static int dirent_insert(struct t3_state *sbi, uint64_t dir_ino, struct t3_inode *dir,
                         const char *name, uint32_t name_len, uint64_t child_ino) {
    uint32_t need = (7 + name_len + 3) & ~3u;
    uint32_t nblocks = t3_dir_blocks(dir->size);

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, dir, b, &blk) || !blk) return 0;
        const uint8_t *cur = txn_peek(sbi, blk);
        if (!cur) {
            if (!t3_read_block(sbi, blk, sbi->blk)) return 0;
            cur = sbi->blk;
        }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(cur + off);
            uint16_t rec_len = rd16(cur + off + 4);
            uint8_t nl = cur[off + 6];
            if (t3_dirent_bad(off, rec_len, nl)) break;
            uint32_t used = e_ino ? ((7u + nl + 3u) & ~3u) : 0;
            if (rec_len - used >= need) {
                uint8_t *img = t3_txn_stage(sbi, blk);
                if (!img) return 0;
                uint32_t new_off = off + used;
                if (e_ino) wr16(img + off + 4, (uint16_t)used);
                wr32(img + new_off, (uint32_t)child_ino);
                wr16(img + new_off + 4, (uint16_t)(rec_len - used));
                img[new_off + 6] = (uint8_t)name_len;
                k_memcpy(img + new_off + 7, name, name_len);
                return 1;
            }
            off += rec_len;
        }
    }

    // No room: grow the directory by one EMPTY block in its own
    // transaction, then stage the insertion into it as part of the
    // caller's transaction. The empty-first split is load-bearing:
    // an early version wrote the child's entry INTO the grow block,
    // which made the name visible one transaction before the child's
    // inode existed -- exactly the namespace-corruption window the
    // journal is for. An empty extra block, by contrast, is harmless
    // slack if the caller's transaction never lands.
    if (nblocks >= T3_DIR_MAX_BLOCKS) return 0; // dirs stop at single-indirect scale
    // The grow commits on its own, so the caller must have staged nothing
    // yet (callers stage after insert only).
    if (sbi->txn_count != sbi->txn_hbase) return 0;
    for (int i = 0; i < sbi->txn_hbase; i++)
        if (sbi->txn_undo[i]) return 0;   // ...not even into an earlier op's block
    int credits = sbi->txn_credits - sbi->txn_hbase;
    uint32_t prefer_group = (uint32_t)(dir_ino / sbi->sb.ipg);
    uint32_t newblk = t3_alloc_block(sbi, prefer_group, 0);
    if (!newblk) return 0;
    k_memset(sbi->blk, 0, T3_BLOCK);
    wr16(sbi->blk + 4, (uint16_t)T3_BLOCK); // one free entry spanning the block
    if (!t3_write_block(sbi, newblk, sbi->blk)) { t3_free_block_bit(sbi, newblk); return 0; }

    // Wire it into the map. Directs cover 12 blocks; past that the
    // single-indirect table gets the pointer (no pcache at dir scale).
    // A table the directory ALREADY HAD is journaled with the grow, so it
    // never reaches the disk ahead of the bitmap naming newblk used; a
    // fresh one is named by nothing until this commit, which fences it.
    uint32_t table = 0;
    int fresh = 0;
    if (nblocks < 12) {
        dir->ptrs[nblocks] = newblk;
    } else {
        table = dir->ptrs[12];
        if (!table) {
            table = t3_alloc_block(sbi, prefer_group, 0);
            if (!table) return 0;
            dir->ptrs[12] = table;
            fresh = 1;
        }
    }
    dir->size += T3_BLOCK;
    t3_touch(dir);
    if (!t3_flush_alloc_state(sbi)) return 0;
    // The caller's handle has staged nothing, so closing it around the
    // grow's own (begin unwinds it, a no-op) and opening it again after
    // is the same handle. The grow's commit takes anything earlier ops
    // left staged with it. Per-mount scratch is filled only AFTER the
    // begin, which can open a lock gap.
    if (!t3_txn_begin(sbi, table && !fresh ? 2 : 1)) return 0;
    if (table) {
        uint32_t slot = (nblocks - 12) * 4;
        if (fresh) {
            k_memset(sbi->ptr_blk, 0, T3_BLOCK);
            wr32(sbi->ptr_blk + slot, newblk);
            t3_alog_note_table(sbi, table);
            sbi->alloc_unfenced = 1;
            if (!t3_write_block(sbi, table, sbi->ptr_blk)) { t3_txn_reset(sbi); return 0; }
        } else {
            uint8_t *ti = t3_txn_stage(sbi, table);
            if (!ti) { t3_txn_reset(sbi); return 0; }
            wr32(ti + slot, newblk);
            t3_rcache_drop(sbi);   // staged, not written: the read cache would not know
        }
        t3_map_cache_forget(sbi, table);
    }
    if (!t3_txn_stage_inode(sbi, dir_ino, dir)) { t3_txn_reset(sbi); return 0; }
    if (!t3_txn_commit(sbi)) return 0;
    // The grow just COMMITTED: its blocks are referenced by the
    // parent inode now, so they must survive any rollback of the
    // caller's still-pending transaction.
    t3_alog_forget(sbi, newblk);
    if (dir->ptrs[12]) t3_alog_forget(sbi, dir->ptrs[12]);
    if (!t3_txn_begin(sbi, credits)) return 0;

    // Now stage the actual insertion into the fresh block, in the
    // caller's transaction.
    uint8_t *img = t3_txn_stage(sbi, newblk);
    if (!img) return 0;
    wr32(img, (uint32_t)child_ino);
    wr16(img + 4, (uint16_t)T3_BLOCK);
    img[6] = (uint8_t)name_len;
    k_memcpy(img + 7, name, name_len);
    return 1;
}

// Stage the removal of `name` from `dir` (ext2-style fold into the
// previous entry). Returns the removed entry's inode via *out_child,
// 0 on not-found/failure.
static int dirent_remove(struct t3_state *sbi, struct t3_inode *dir, const char *name,
                         uint32_t name_len, uint64_t *out_child) {
    uint32_t nblocks = t3_dir_blocks(dir->size);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, dir, b, &blk) || !blk) return 0;
        const uint8_t *cur = txn_peek(sbi, blk);
        if (!cur) {
            if (!t3_read_block(sbi, blk, sbi->blk)) return 0;
            cur = sbi->blk;
        }
        uint32_t off = 0, prev_off = 0;
        int have_prev = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(cur + off);
            uint16_t rec_len = rd16(cur + off + 4);
            uint8_t nl = cur[off + 6];
            if (t3_dirent_bad(off, rec_len, nl)) break;
            if (e_ino && nl == name_len && k_memcmp(cur + off + 7, name, name_len) == 0) {
                uint8_t *img = t3_txn_stage(sbi, blk);
                if (!img) return 0;
                if (have_prev) {
                    uint16_t p_len = rd16(img + prev_off + 4);
                    wr16(img + prev_off + 4, (uint16_t)(p_len + rec_len));
                } else {
                    wr32(img + off, 0);
                    img[off + 6] = 0;
                }
                *out_child = e_ino;
                return 1;
            }
            prev_off = off; have_prev = 1;
            off += rec_len;
        }
    }
    return 0;
}

// Split a normalized path into (parent inode, final component). The
// component is COPIED into `namebuf` (T3_NAME_MAX + 1 bytes): `norm` is
// per-mount scratch, and t3_txn_begin() can open a lock gap in which
// another op normalizes into the same buffer.
static int split_parent(struct t3_state *sbi, const char *norm, uint64_t *out_parent,
                        char *namebuf, const char **out_name, uint32_t *out_len) {
    if (k_strcmp(norm, "/") == 0) return 0;
    const char *last = norm;
    for (const char *p = norm; *p; p++) if (*p == '/') last = p;
    uint32_t len = (uint32_t)k_strlen(last + 1);
    if (len == 0 || len > T3_NAME_MAX) return 0;
    char *const parent = sbi->pb.split_parent_parent; // per-function, see t3_normalize()
    if (last == norm) { parent[0] = '/'; parent[1] = '\0'; }
    else {
        uint32_t plen = (uint32_t)(last - norm);
        if (plen >= T3_PATH_BUF) return 0;
        k_memcpy(parent, norm, plen);
        parent[plen] = '\0';
    }
    uint64_t pino = T3_INO_ROOT;
    if (k_strcmp(parent, "/") != 0 && !t3_resolve(sbi, parent, &pino)) return 0;
    *out_parent = pino;
    k_memcpy(namebuf, last + 1, len + 1);
    *out_name = namebuf;
    *out_len = len;
    return 1;
}

// ---- mutating ops ---------------------------------------------------------

// Create a file or directory entry under an existing parent.
// Transaction: parent dirent block + child inode block (+ the child's
// own `.`/`..` dirent block is DATA, written before anything points
// at it) + parent inode block for mkdir's link-count bump. <= 3
// slots; directory growth runs as its own transaction inside
// dirent_insert() (see its comment).
static int create_entry_inner(struct t3_state *sbi, const char *path, uint8_t type,
                              uint64_t *out_ino, uint64_t *out_existing);

// Create `path`, or 0. `out_existing`, when given, gets the inode of a
// name that was ALREADY there (and stays 0 otherwise), so a caller that
// accepts an existing file -- touch -- need not look the path up first:
// that lookup is a directory scan for a name that is usually absent,
// and create_entry_inner() has to do it anyway.
static int create_entry_ex(struct t3_state *sbi, const char *path, uint8_t type,
                           uint64_t *out_ino, uint64_t *out_existing) {
    if (out_existing) *out_existing = 0;
    t3_alog_begin(sbi);
    int ok = create_entry_inner(sbi, path, type, out_ino, out_existing);
    if (ok) { t3_alog_commit(sbi); return 1; }
    if (out_existing && *out_existing) { t3_alog_cancel(sbi); return 0; }   // nothing was allocated
    t3_alog_rollback(sbi);   // as do_write()
    return 0;
}

static int create_entry(struct t3_state *sbi, const char *path, uint8_t type, uint64_t *out_ino) {
    return create_entry_ex(sbi, path, type, out_ino, 0);
}

static int create_entry_inner(struct t3_state *sbi, const char *path, uint8_t type,
                              uint64_t *out_ino, uint64_t *out_existing) {
    char *const norm = sbi->pb.create_entry_inner_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t existing;
    if (t3_resolve(sbi, norm, &existing)) {    // caller decides what exists means
        if (out_existing) *out_existing = existing;
        return 0;
    }
    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    char namebuf[T3_NAME_MAX + 1];
    if (!split_parent(sbi, norm, &parent_ino, namebuf, &name, &name_len)) return 0;
    if (!t3_lock(sbi, parent_ino, 1)) return 0;   // a namespace change: the parent
    struct t3_inode parent;
    if (!t3_read_inode(sbi, parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;
    if (type == T3_TYPE_DIR && parent.links >= T3_LINK_MAX) return 0;   // its `..` would wrap the count

    uint32_t prefer_group = (uint32_t)(parent_ino / sbi->sb.ipg);
    if (!t3_lock_room(sbi)) return 0;
    uint64_t ino = t3_alloc_inode(sbi, prefer_group);
    if (!ino) return 0;
    // THE NEW INODE TOO, across the commit's gap: an op let in there finds
    // it through the committing overlay, and one that stages into the new
    // folder's block would build on work a failed commit takes back. A
    // number nobody holds (t3_ino_busy()) and a free slot: this cannot wait.
    if (!t3_lock(sbi, ino, 1)) return 0;
    // Block allocations below roll back via the alloc log (armed by
    // the create_entry() shell); the inode bit is freed by hand on
    // each failure path since the log only tracks blocks.

    struct t3_inode node;
    k_memset(&node, 0, sizeof(node));
    node.type = type;
    node.links = (type == T3_TYPE_DIR) ? 2 : 1;
    t3_touch(&node);
    node.created = node.modified;
    // A NEW inode records its mode explicitly rather than leaning on the
    // read-time default, so that a later chmod has somewhere to write
    // and so an inode's stored mode means what it says.
    node.mode = T3_MODE_DEFAULT(type);

    if (type == T3_TYPE_DIR) {
        // The child's own dirent block: plain data until the inode
        // transaction lands, so a direct (unjournaled) write is safe.
        uint32_t blk = t3_alloc_block(sbi, prefer_group, 0);
        if (!blk) { t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0; }
        k_memset(sbi->blk, 0, T3_BLOCK);
        wr32(sbi->blk, (uint32_t)ino); wr16(sbi->blk + 4, 12); sbi->blk[6] = 1; sbi->blk[7] = '.';
        wr32(sbi->blk + 12, (uint32_t)parent_ino); wr16(sbi->blk + 16, (uint16_t)(T3_BLOCK - 12));
        sbi->blk[18] = 2; sbi->blk[19] = '.'; sbi->blk[20] = '.';
        if (!t3_write_block(sbi, blk, sbi->blk)) { t3_free_inode_bit(sbi, ino); t3_free_block_bit(sbi, blk); t3_flush_alloc_state(sbi); return 0; }
        node.ptrs[0] = blk;
        node.size = T3_BLOCK;
    }

    if (!t3_flush_alloc_state(sbi)) { t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0; } // set-before-use

    // dirent block + the new inode + (for a directory) the parent's
    // link count. Under `batched` the handle STOPS, as a write's does, so
    // the creates and writes around it share one commit -- a copied file
    // is one commit at its close on a stick (vfs.c's owe()), not two. A
    // crash before then loses the file whole and leaks its inode bit,
    // which fsck reclaims -- what a stopped write already risks.
    int defer = storage_sync_batched();
    if (!t3_txn_begin(sbi, 3)) {
        t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0;
    }
    int ins = dirent_insert(sbi, parent_ino, &parent, name, name_len, ino);
    if (!ins) { t3_txn_reset(sbi); t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0; }
    if (!t3_txn_stage_inode(sbi, ino, &node)) { t3_txn_reset(sbi); t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0; }
    if (type == T3_TYPE_DIR) {
        parent.links++;
        parent.modified = node.modified;
        parent.modified_ns = node.modified_ns;
        if (!t3_txn_stage_inode(sbi, parent_ino, &parent)) { t3_txn_reset(sbi); t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0; }
    }
    if (defer) {
        t3_txn_stop(sbi);
    } else if (!t3_txn_commit_last(sbi)) {
        t3_free_inode_bit(sbi, ino); t3_flush_alloc_state(sbi); return 0;
    }
    // NO NAME-CACHE FLUSH: both caches hold only names that resolved, and
    // adding a name cannot change what any of them names. Flushing here
    // threw away the root and parent the next create in the same
    // directory walks through again (see t3_ncache_flush()).
    if (out_ino) *out_ino = ino;
    return 1;
}

int tfs3_touch(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_touch_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (create_entry_ex(sbi, path, T3_TYPE_FILE, 0, &ino)) return 1;
    if (!ino) return 0;   // not there, and could not be made
    // Existing file: a no-op that succeeds; existing dir: refuse.
    // Matches tfs_touch()'s behavior exactly (incl. not bumping
    // `modified` -- see fs.h's fs_stat_info comment).
    if (!t3_lock(sbi, ino, 0)) return 0;
    if (!t3_read_inode(sbi, ino, &node)) return 0;
    return node.type == T3_TYPE_FILE;
}

int tfs3_create_excl(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_create_excl_norm;
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    if (create_entry_ex(sbi, path, T3_TYPE_FILE, 0, &ino)) return 1;
    return ino ? -EEXIST : 0;
}

int tfs3_mkdir(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_mkdir_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    if (t3_resolve(sbi, norm, &ino)) return 0; // exists (file OR dir) -- refuse
    return create_entry(sbi, path, T3_TYPE_DIR, 0);
}

int tfs3_write_range(void *st, const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_write_range_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino)) {
        if (!create_entry(sbi, path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!t3_lock(sbi, ino, 1)) return 0;
    if (!t3_read_inode(sbi, ino, &node) || node.type != T3_TYPE_FILE) return 0;
    if (offset == FS_OFFSET_END) offset = node.size;   // under the inode's lock
    if (len == 0) return 1;
    return do_write(sbi, ino, &node, offset, buf, len);
}

int tfs3_write(void *st, const char *path, const char *data, int append) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_write_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino)) {
        if (!create_entry(sbi, path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!t3_lock(sbi, ino, 1)) return 0;
    if (!t3_read_inode(sbi, ino, &node) || node.type != T3_TYPE_FILE) return 0;

    uint32_t len = (uint32_t)k_strlen(data);
    uint64_t start = node.size;
    if (!append && node.size > 0) {
        // Truncate: commit the emptied inode FIRST, then return the
        // old blocks -- tfs_write()'s detach-then-persist ordering,
        // expressed in TFS3 terms (clear-after-persist; a crash
        // between the two steps leaks, fsck reclaims).
        // Under `batched` the emptied inode STOPS and the old blocks wait
        // for its commit, as a delete's do -- so the new data goes to
        // fresh blocks, and a crash before the commit keeps the old file.
        struct t3_inode old = node;
        int batched = storage_sync_batched();
        struct t3_pfree *rec = 0;
        if (batched) {
            int went;
            rec = pending_frees(sbi, 0, &old, &went);
            if (went) return 0;   // unreachable: the inode lock is held
        }
        k_memset(node.ptrs, 0, sizeof(node.ptrs));
        node.size = 0;
        t3_touch(&node);
        if (!t3_txn_begin(sbi, 1)) { t3_pf_free(rec); return 0; }
        if (!t3_txn_stage_inode(sbi, ino, &node)) { t3_txn_reset(sbi); t3_pf_free(rec); return 0; }
        if (rec) {
            t3_txn_stop(sbi);
            t3_pf_queue(sbi, rec);
        } else {
            if (!t3_txn_commit(sbi)) return 0;
            if (free_all_blocks(sbi, &old) < 0) return 0;   // unmounted meanwhile
            t3_flush_alloc_state(sbi);
        }
        start = 0;
    } else if (!append) {
        start = 0;
    }
    if (len == 0) return 1;
    return do_write(sbi, ino, &node, start, data, len);
}

int tfs3_delete(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_delete_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;
    uint64_t ino, del_pino;
    const char *del_name;
    uint32_t del_len;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino)) return 0;
    char namebuf[T3_NAME_MAX + 1];
    if (!split_parent(sbi, norm, &del_pino, namebuf, &del_name, &del_len)) return 0;
    if (!t3_lock(sbi, del_pino, 1) || !t3_lock(sbi, ino, 1)) return 0;
    if (!t3_read_inode(sbi, ino, &node)) return 0;

    if (node.type == T3_TYPE_DIR) {
        // Empty means "nothing but . and .." -- the no-recursive-
        // delete policy, unchanged (docs/decisions.md).
        uint32_t nblocks = t3_dir_blocks(node.size);
        for (uint32_t b = 0; b < nblocks; b++) {
            uint32_t blk;
            if (!t3_block_for_index(sbi, &node, b, &blk) || !blk || !t3_read_block(sbi, blk, sbi->blk)) return 0;
            uint32_t off = 0;
            while (off + 8 <= T3_BLOCK) {
                uint32_t e_ino = rd32(sbi->blk + off);
                uint16_t rec_len = rd16(sbi->blk + off + 4);
                uint8_t nl = sbi->blk[off + 6];
                // A BROKEN CHAIN HIDES THE REST OF ITS BLOCK: not empty, or
                // the names past it are orphaned and the next repair frees them.
                if (t3_dirent_bad(off, rec_len, nl)) return 0;
                if (e_ino && !(nl == 1 && sbi->blk[off + 7] == '.') &&
                    !(nl == 2 && sbi->blk[off + 7] == '.' && sbi->blk[off + 8] == '.')) {
                    return 0; // not empty
                }
                off += rec_len;
            }
        }
    }

    // UNDER `batched` A DELETE STOPS, as a create does, and what it frees
    // waits for the commit that carries it (struct t3_pfree): until then
    // the bitmap still holds the blocks and the inode, so a crash before
    // the commit replays a volume where the file is still there, and
    // nothing can reuse them meanwhile. ONE read of the mode, for the
    // reason t3_stage_inode_update() gives.
    int batched = storage_sync_batched();
    int last = node.type == T3_TYPE_DIR || node.links <= 1;
    struct t3_pfree *rec = 0;
    if (batched && last) {
        int went;
        rec = pending_frees(sbi, ino, &node, &went);
        if (went) return 0;   // unreachable: an inode lock is held (an unmount waits)
        // Its tables were read with the lock dropped: nothing in `norm` is
        // used past here, and the name was copied (split_parent()).
    }

    uint64_t parent_ino = del_pino;
    struct t3_inode parent;
    if (!t3_read_inode(sbi, parent_ino, &parent) || parent.type != T3_TYPE_DIR) {
        t3_pf_free(rec);
        return 0;
    }

    // dirent block + the inode + (for a directory) the parent's link
    // count.
    if (!t3_txn_begin(sbi, 3)) { t3_pf_free(rec); return 0; }
    uint64_t removed = 0;
    if (!dirent_remove(sbi, &parent, del_name, del_len, &removed) || removed != ino) {
        t3_txn_reset(sbi);
        t3_pf_free(rec);
        return 0;
    }

    int gone = 0;
    if (last) {
        // Last name: zero the inode; blocks are freed after commit.
        if (!t3_txn_stage_inode(sbi, ino, 0)) { t3_txn_reset(sbi); t3_pf_free(rec); return 0; }
        gone = 1;
        if (node.type == T3_TYPE_DIR) {
            parent.links--;
            t3_touch(&parent);
            if (!t3_txn_stage_inode(sbi, parent_ino, &parent)) { t3_txn_reset(sbi); t3_pf_free(rec); return 0; }
        }
    } else {
        // A hardlink remains -- just drop the count.
        node.links--;
        if (!t3_txn_stage_inode(sbi, ino, &node)) { t3_txn_reset(sbi); return 0; }
    }
    if (batched && (!gone || rec)) {
        // Queued AFTER the stop: a handle never carries frees, so unwinding
        // one never has to take any back (commit_finish()).
        t3_txn_stop(sbi);
        if (rec) t3_pf_queue(sbi, rec);
        t3_ncache_flush(sbi);
        return 1;
    }
    // BEFORE THE COMMIT'S GAP too: an op let in there resolves through
    // the caches, which still name what this staged away.
    t3_ncache_flush(sbi);
    if (!t3_txn_commit_last(sbi)) return 0;
    t3_ncache_flush(sbi);

    if (gone) {
        // clear-after-persist: nothing references these anymore. The
        // delete has committed, so a mount gone meanwhile still succeeded
        // -- with its blocks leaked, which fsck reclaims.
        if (free_all_blocks(sbi, &node) < 0) return 1;
        t3_free_inode_bit(sbi, ino);
        t3_flush_alloc_state(sbi);
    }
    return 1;
}

// The first OPTIONAL fs_ops op, gated by FS_CAP_HARDLINKS (the caps
// honesty check in vfs.c verifies the pair). Files only -- hardlinked
// directories turn the tree into a graph, refused by every real Unix
// filesystem for the same reason (see the design doc).
int tfs3_link(void *st, const char *existing, const char *newpath) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_link_norm;
    char *const newnorm = sbi->pb.tfs3_link_newnorm; // see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, existing, norm) || !t3_normalize(sbi, newpath, newnorm)) return 0;
    uint64_t ino, clash;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino) || !t3_read_inode(sbi, ino, &node)) return 0;
    if (node.type != T3_TYPE_FILE) return 0;
    if (t3_resolve(sbi, newnorm, &clash)) return 0; // target name taken
    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    char namebuf[T3_NAME_MAX + 1];
    if (!split_parent(sbi, newnorm, &parent_ino, namebuf, &name, &name_len)) return 0;
    if (!t3_lock(sbi, parent_ino, 1) || !t3_lock(sbi, ino, 1)) return 0;
    if (!t3_read_inode(sbi, ino, &node)) return 0;   // under the lock
    struct t3_inode parent;
    if (!t3_read_inode(sbi, parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;
    if (node.links >= T3_LINK_MAX) return 0;   // a wrapped count frees an inode still named

    t3_alog_begin(sbi); // dir growth inside dirent_insert can allocate
    if (!t3_txn_begin(sbi, 2)) { t3_alog_rollback(sbi); return 0; } // dirent block + the inode's link count
    if (!dirent_insert(sbi, parent_ino, &parent, name, name_len, ino)) { t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0; }
    node.links++;
    if (!t3_txn_stage_inode(sbi, ino, &node)) { t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0; }
    if (!t3_txn_commit_last(sbi)) { t3_alog_rollback(sbi); return 0; }
    t3_alog_commit(sbi);
    t3_ncache_flush(sbi);
    return 1;
}

// Stage a change to an EXISTING entry's inode number (rename's ".."
// fixup). Same scan as dirent_remove(), different patch.
static int dirent_repoint(struct t3_state *sbi, struct t3_inode *dir, const char *name,
                          uint32_t name_len, uint64_t to) {
    uint32_t nblocks = t3_dir_blocks(dir->size);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, dir, b, &blk) || !blk) return 0;
        const uint8_t *cur = txn_peek(sbi, blk);
        if (!cur) {
            if (!t3_read_block(sbi, blk, sbi->blk)) return 0;
            cur = sbi->blk;
        }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(cur + off);
            uint16_t rec_len = rd16(cur + off + 4);
            uint8_t nl = cur[off + 6];
            if (t3_dirent_bad(off, rec_len, nl)) break;
            if (e_ino && nl == name_len && k_memcmp(cur + off + 7, name, name_len) == 0) {
                uint8_t *img = t3_txn_stage(sbi, blk);
                if (!img) return 0;
                wr32(img + off, (uint32_t)to);
                return 1;
            }
            off += rec_len;
        }
    }
    return 0;
}

// True if `child` names something at or below `parent` -- the check
// that stops `mv /a /a/b`, which would otherwise detach a whole
// subtree into a cycle referenced by nothing. Both are normalized
// absolute paths and this filesystem has no symlinks or hardlinked
// directories, so a string prefix IS the tree relation.
static int path_is_within(const char *parent, const char *child) {
    uint32_t plen = (uint32_t)k_strlen(parent);
    if (k_strcmp(parent, "/") == 0) return 1;
    if (k_strncmp(child, parent, plen) != 0) return 0;
    return child[plen] == '\0' || child[plen] == '/';
}

// THE REPLACING HALF OF rename_impl(): `newpath` names a file and it is
// to become `oldpath`'s file, dpkg's write-`.new`-then-rename shape.
// ONE transaction repoints the destination's dirent at the source inode
// and removes the source's, so a crash shows the old file or the new
// one under that name and never neither -- what a two-call unlink and
// rename cannot promise. The displaced inode is freed after the commit
// (clear-after-persist, as tfs3_delete()).
//
// Files only: a directory on either side is refused, and so is a
// cross-parent replace on a v1 journal (five credits, like a directory
// move).
static int replace_locked(struct t3_state *sbi, uint64_t ino, const struct t3_inode *node,
                          uint64_t old_ino, uint64_t src_pino, const char *src_name,
                          uint32_t src_len, uint64_t dst_pino, const char *dst_name,
                          uint32_t dst_len) {
    struct t3_inode victim, src_parent, dst_parent;
    if (node->type == T3_TYPE_DIR) return 0;
    if (!t3_read_inode(sbi, old_ino, &victim) || victim.type == T3_TYPE_DIR) return 0;
    if (!t3_read_inode(sbi, src_pino, &src_parent) || src_parent.type != T3_TYPE_DIR) return 0;
    if (!t3_read_inode(sbi, dst_pino, &dst_parent) || dst_parent.type != T3_TYPE_DIR) return 0;

    int same_parent = (src_pino == dst_pino);
    // Two dirent blocks, the displaced inode, and each parent's mtime.
    int credits = same_parent ? 4 : 5;
    if (credits > (int)sbi->jslots) {
        klog_write("tfs3: this volume's journal is too small to replace a file in another "
                   "directory (v1 format)\n");
        return 0;
    }
    struct t3_inode *dstp = same_parent ? &src_parent : &dst_parent;

    if (!t3_txn_begin(sbi, credits)) return 0;
    if (!dirent_repoint(sbi, dstp, dst_name, dst_len, ino)) { t3_txn_reset(sbi); return 0; }
    uint64_t removed = 0;
    if (!dirent_remove(sbi, &src_parent, src_name, src_len, &removed) || removed != ino) {
        t3_txn_reset(sbi); return 0;
    }
    int gone = victim.links <= 1;
    if (gone) {
        if (!t3_txn_stage_inode(sbi, old_ino, 0)) { t3_txn_reset(sbi); return 0; }
    } else {
        victim.links--;   // a hardlink elsewhere keeps the old data alive
        if (!t3_txn_stage_inode(sbi, old_ino, &victim)) { t3_txn_reset(sbi); return 0; }
    }
    struct t3_inode now;
    t3_touch(&now);
    src_parent.modified = now.modified;
    src_parent.modified_ns = now.modified_ns;
    dstp->modified = now.modified;
    dstp->modified_ns = now.modified_ns;
    if (!t3_txn_stage_inode(sbi, src_pino, &src_parent)) { t3_txn_reset(sbi); return 0; }
    if (!same_parent && !t3_txn_stage_inode(sbi, dst_pino, &dst_parent)) {
        t3_txn_reset(sbi); return 0;
    }
    t3_ncache_flush(sbi);   // before the gap as well (tfs3_delete())
    if (!t3_txn_commit_last(sbi)) return 0;
    t3_ncache_flush(sbi);

    if (gone) {
        if (free_all_blocks(sbi, &victim) < 0) return 1;   // committed; fsck reclaims
        t3_free_inode_bit(sbi, old_ino);
        t3_flush_alloc_state(sbi);
    }
    return 1;
}

// Rename/move, in ONE journal transaction: the namespace never shows
// both names or neither. Refuses an existing destination unless
// `replace` (fs_rename_replace(), fs.h), a directory moved into its own
// subtree, and the root.
//
// Credit accounting is the interesting part, and it is why v2's larger
// journal exists. Worst case is a directory changing parents: both
// dirent blocks, the child's ".." block, and both parents' link
// counts -- five. Every other shape needs three or four, which is why
// a v1 image can still rename freely and only refuses that one case,
// with a message, instead of failing halfway.
static int rename_impl(void *st, const char *oldpath, const char *newpath, int replace) {
    struct t3_state *sbi = st;
    char *const oldn = sbi->pb.tfs3_rename_oldn;
    char *const newn = sbi->pb.tfs3_rename_newn; // see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, oldpath, oldn) || !t3_normalize(sbi, newpath, newn)) return 0;
    if (k_strcmp(oldn, "/") == 0 || k_strcmp(newn, "/") == 0) return 0;
    uint64_t ino, clash;
    if (k_strcmp(oldn, newn) == 0) return t3_resolve(sbi, oldn, &ino); // to itself: nothing, if it exists

    struct t3_inode node;
    if (!t3_resolve(sbi, oldn, &ino) || !t3_read_inode(sbi, ino, &node)) return 0;
    int has_clash = t3_resolve(sbi, newn, &clash);
    if (has_clash && !replace) return 0; // destination taken
    if (has_clash && clash == ino) return 1; // two names of one inode: POSIX does nothing
    if (node.type == T3_TYPE_DIR && path_is_within(oldn, newn)) return 0;

    uint64_t src_pino, dst_pino;
    const char *src_name, *dst_name;
    uint32_t src_len, dst_len;
    char src_buf[T3_NAME_MAX + 1], dst_buf[T3_NAME_MAX + 1];
    if (!split_parent(sbi, oldn, &src_pino, src_buf, &src_name, &src_len)) return 0;
    if (!split_parent(sbi, newn, &dst_pino, dst_buf, &dst_name, &dst_len)) return 0;
    // All three, taken as a set: none is held while waiting for another
    // (t3_lock()), so there is no order to keep. A DIRECTORY CHANGING
    // PARENTS takes the move lock first: the check above read the tree's
    // shape, and another such move let in by one of this op's gaps could
    // put the destination under the source -- a loop cut off from `/`.
    if (node.type == T3_TYPE_DIR && src_pino != dst_pino && !t3_lock(sbi, T3_LOCK_DIRMOVE, 1))
        return 0;
    if (!t3_lock(sbi, src_pino, 1) || !t3_lock(sbi, dst_pino, 1) || !t3_lock(sbi, ino, 1))
        return 0;
    if (has_clash && !t3_lock(sbi, clash, 1)) return 0;
    if (!t3_read_inode(sbi, ino, &node)) return 0;   // under the lock
    if (has_clash) return replace_locked(sbi, ino, &node, clash, src_pino, src_name, src_len,
                                         dst_pino, dst_name, dst_len);

    struct t3_inode src_parent, dst_parent;
    if (!t3_read_inode(sbi, src_pino, &src_parent) || src_parent.type != T3_TYPE_DIR) return 0;
    if (!t3_read_inode(sbi, dst_pino, &dst_parent) || dst_parent.type != T3_TYPE_DIR) return 0;

    int same_parent = (src_pino == dst_pino);
    int is_dir = (node.type == T3_TYPE_DIR);
    if (is_dir && !same_parent && dst_parent.links >= T3_LINK_MAX) return 0;
    int credits = same_parent ? 3 : 4;
    if (is_dir && !same_parent) credits++; // the ".." fixup block
    if (credits > (int)sbi->jslots) {
        klog_write("tfs3: this volume's journal is too small to move a directory "
                   "between parents (v1 format) -- reformat with `fsformat tfs3 confirm`\n");
        return 0;
    }

    // The names are this frame's copies (split_parent()); the
    // same-parent case aliases the two struct copies, so take that fork
    // explicitly rather than editing two stale copies of one directory.
    struct t3_inode *dstp = same_parent ? &src_parent : &dst_parent;

    t3_alog_begin(sbi); // a destination directory can grow inside dirent_insert
    if (!t3_txn_begin(sbi, credits)) { t3_alog_rollback(sbi); return 0; }

    // Insert FIRST: only the insert can need to grow a directory, and
    // a grow commits its own transaction, which it can only do while
    // nothing else is staged.
    if (!dirent_insert(sbi, dst_pino, dstp, dst_name, dst_len, ino)) {
        t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0;
    }
    uint64_t removed = 0;
    if (!dirent_remove(sbi, &src_parent, src_name, src_len, &removed) || removed != ino) {
        t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0;
    }

    struct t3_inode now;
    t3_touch(&now);
    if (is_dir && !same_parent) {
        if (!dirent_repoint(sbi, &node, "..", 2, dst_pino)) { t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0; }
        src_parent.links--;
        dst_parent.links++;
    }
    src_parent.modified = now.modified;
    src_parent.modified_ns = now.modified_ns;
    dstp->modified = now.modified;
    dstp->modified_ns = now.modified_ns;
    if (!t3_txn_stage_inode(sbi, src_pino, &src_parent)) { t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0; }
    if (!same_parent && !t3_txn_stage_inode(sbi, dst_pino, &dst_parent)) {
        t3_txn_reset(sbi); t3_alog_rollback(sbi); return 0;
    }
    t3_ncache_flush(sbi);   // before the gap as well (tfs3_delete())
    if (!t3_txn_commit_last(sbi)) { t3_alog_rollback(sbi); return 0; }
    t3_alog_commit(sbi);
    t3_ncache_flush(sbi);
    return 1;
}

int tfs3_rename(void *st, const char *oldpath, const char *newpath) {
    return rename_impl(st, oldpath, newpath, 0);
}

int tfs3_rename_replace(void *st, const char *oldpath, const char *newpath) {
    return rename_impl(st, oldpath, newpath, 1);
}

// Set a file's size exactly. Growing is SPARSE -- the size moves and
// the new range reads as zeros, which read_range_impl() already
// handles, so a 1 GB truncate costs one inode write and no blocks.
// Shrinking commits the smaller size FIRST and frees afterwards
// (clear-after-persist): a crash in between costs leaked blocks that
// fsck reclaims, never a live file pointing at freed space.
int tfs3_truncate(void *st, const char *path, uint64_t size) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_truncate_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino)) return 0;
    if (!t3_lock(sbi, ino, 1)) return 0;
    if (!t3_read_inode(sbi, ino, &node)) return 0;
    if (node.type != T3_TYPE_FILE) return 0; // directories size themselves
    if (size > T3_MAX_FILE_SIZE) return 0;   // a sparse grow past what the format addresses
    if (node.size == size) return 1;

    int shrinking = (size < node.size);
    if (shrinking) {
        // ZERO THE RETAINED TAIL, or a shrink and regrow hands the old
        // bytes back: the final partial block survives the truncate, and
        // both a regrow and a write into the gap read it whole. ext4
        // zeroes the partial page in ext4_block_truncate_page() for the
        // same reason; NTFS tracks a valid-data-length instead, which
        // needs an inode field this format has not got.
        //
        // BEFORE the size commit, as ext4 orders it. The cost is that a
        // failed truncate may already have discarded the bytes it was
        // asked to discard -- cheaper than the alternative, where a
        // failed zeroing leaves them exposed with the size already moved.
        uint32_t tail = (uint32_t)(size % T3_BLOCK);
        if (tail) {
            uint32_t tblk;
            if (!t3_block_for_index(sbi, &node, (uint32_t)(size / T3_BLOCK), &tblk)) return 0;
            if (tblk) {
                if (!t3_read_block(sbi, tblk, sbi->blk)) return 0;
                k_memset(sbi->blk + tail, 0, T3_BLOCK - tail);
                if (!t3_write_block(sbi, tblk, sbi->blk)) return 0;
            }
        }
        uint32_t first = (uint32_t)((size + T3_BLOCK - 1) / T3_BLOCK);
        return trunc_to(sbi, ino, &node, first, size, 0) != 0;   // -1: committed, the mount went
    }

    // A GROW reads back the old last block's tail as file data, so it is
    // zeroed first: a write that failed past end-of-file may have left
    // bytes there (zero_past_eof()). Blocks mapped past the old end are
    // cut with the size, for the same reason (cut_past_eof()).
    if (!zero_old_tail(sbi, &node, size, 0)) return 0;
    uint32_t old_first = (uint32_t)((node.size + T3_BLOCK - 1) / T3_BLOCK);
    if (mapped_from(sbi, &node, old_first)) return trunc_to(sbi, ino, &node, old_first, size, 0) != 0;
    node.size = size;
    t3_touch(&node);
    if (!t3_txn_begin(sbi, 1) || !t3_txn_stage_inode(sbi, ino, &node)) { t3_txn_reset(sbi); return 0; }
    return t3_txn_commit_last(sbi);
}

// Steppable write: one block per step() call, inode committed once on
// the final step -- so a crash mid-stream leaks fresh blocks and
// leaves the file at its old size, same contract the blocking path
// gives (fs.h: on FS_STEP_DONE size/mtime/metadata are updated).
// FIELD BY FIELD, never memcmp: the struct has padding (after `type`
// and `links`) that t3_read_inode() does not write, so two reads of one
// unchanged inode compare unequal.
static int inode_same(const struct t3_inode *a, const struct t3_inode *b) {
    if (a->type != b->type || a->links != b->links || a->size != b->size ||
        a->created != b->created || a->modified != b->modified || a->mode != b->mode ||
        a->modified_ns != b->modified_ns)
        return 0;
    for (int i = 0; i < 15; i++) if (a->ptrs[i] != b->ptrs[i]) return 0;
    return 1;
}

// ITS INODE NUMBER IS NOT HANDED OUT while the stream is open
// (t3_ino_busy()): a delete and a create in the same second would
// otherwise pass inode_same() as the file it began on.
struct t3_write_step {
    uint64_t ino;
    struct t3_inode node;
    struct t3_inode orig;      // the inode as begin() read it -- see the step
    const uint8_t *src;
    uint64_t offset;
    uint32_t len, total;
    uint32_t last_alloc;
    int cut;                      // a grow cut what it mapped (cut_past_eof())
    struct t3_write_step *next;   // sbi->wsteps
};

static void wsteps_cut(struct t3_state *sbi, uint64_t ino) {
    for (struct t3_write_step *s = sbi->wsteps; s; s = s->next) if (s->ino == ino) s->cut = 1;
}

static void step_free(struct t3_state *sbi, struct t3_write_step *step) {
    for (struct t3_write_step **p = &sbi->wsteps; *p; p = &(*p)->next)
        if (*p == step) { *p = step->next; break; }
    kfree(step);
}

int t3_wstep_has(struct t3_state *sbi, uint64_t ino) {
    for (struct t3_write_step *s = sbi->wsteps; s; s = s->next) if (s->ino == ino) return 1;
    return 0;
}

void t3_wsteps_free(struct t3_state *sbi) {
    while (sbi->wsteps) step_free(sbi, sbi->wsteps);
}

// Every open stream's inode copy, for fsck: what its finished steps
// allocated is named by nothing on disk until its final step commits.
// NOT ONE THAT WILL FAIL: its file was deleted or changed under it, and
// what its copy names may be free -- or another file's -- by now.
void t3_steps_each(struct t3_state *sbi,
                   void (*fn)(struct t3_state *, uint64_t, const struct t3_inode *, void *), void *ctx) {
    for (struct t3_write_step *s = sbi->wsteps; s; s = s->next) {
        struct t3_inode cur;
        if (s->cut || !t3_read_inode(sbi, s->ino, &cur) || !inode_same(&cur, &s->orig)) continue;
        fn(sbi, s->ino, &s->node, ctx);
    }
}

void *tfs3_write_range_begin(void *st, const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_write_range_begin_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    if (!t3_range_fits(offset, len)) return 0;
    uint64_t ino;
    if (!t3_resolve(sbi, norm, &ino)) {
        if (!create_entry(sbi, path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!t3_lock(sbi, ino, 1)) return 0;
    struct t3_write_step *step = kmalloc(sizeof(*step));
    if (!step) return 0;
    if (!t3_read_inode(sbi, ino, &step->node) || step->node.type != T3_TYPE_FILE) { kfree(step); return 0; }
    if (len && !cut_past_eof(sbi, ino, &step->node, offset)) { kfree(step); return 0; }
    step->orig = step->node;
    step->ino = ino;
    step->src = (const uint8_t *)buf;
    step->offset = offset;
    step->len = len;
    step->total = 0;
    step->last_alloc = 0;
    step->cut = 0;
    step->next = sbi->wsteps;
    sbi->wsteps = step;
    t3_pcache_drop(sbi);
    return step;
}

// A step that failed: undo its allocations and end the stream -- unless
// the rollback has FS_OP run the step again, which needs the handle and
// the inode copy as the step found them.
static int step_fail(struct t3_state *sbi, struct t3_write_step *step,
                     const struct t3_inode *node0, uint32_t last0, uint32_t total0) {
    if (t3_alog_rollback(sbi)) {
        step->node = *node0;
        step->last_alloc = last0;
        step->total = total0;   // the rollback unmapped what this step wrote
        return 2;   /* FS_STEP_FAILED -- or run again */
    }
    step_free(sbi, step);
    return 2 /* FS_STEP_FAILED */;
}

int tfs3_write_range_step(void *st, void *handle) {
    struct t3_state *sbi = st;
    struct t3_write_step *step = (struct t3_write_step *)handle;
    // A STREAM SPANS SYSCALLS, and a lock may not (a process dying mid-
    // stream would keep it), so each step locks the file on its own. What
    // that leaves open is another writer between two steps -- and this
    // stream commits its inode COPY at the end, which would silently undo
    // the other write. So it checks the inode is still the one it began
    // from, and FAILS rather than lose somebody's data. Blocks from its
    // earlier steps leak, as an abandoned stream's always have: fsck
    // reclaims those only its inode copy named, once the stream is gone
    // (t3_steps_each()); those an existing table names stay mapped past
    // end-of-file until a grow cuts them (cut_past_eof()) or a delete.
    if (!t3_lock(sbi, step->ino, 1)) return 2 /* FS_STEP_FAILED -- or run again */;
    struct t3_inode cur;
    if (step->cut || !t3_read_inode(sbi, step->ino, &cur) || !inode_same(&cur, &step->orig)) {
        step_free(sbi, step);
        return 2 /* FS_STEP_FAILED */;
    }
    // Room for the tables this step may journal, while nothing is half-built
    // (as do_write()).
    if ((step->node.ptrs[12] | step->node.ptrs[13] | step->node.ptrs[14]) && sbi->txn_deferred &&
        sbi->txn_count + T3_TABLE_CREDITS > (int)sbi->jslots)
        t3_txn_flush_deferred(sbi);
    // Rollback scope is THIS STEP only -- other fs operations
    // interleave between steps of an async write, so a whole-stream
    // log can't be kept armed. Earlier completed steps of an
    // abandoned stream leak by design (crash-shaped), as above.
    t3_alog_begin(sbi);
    struct t3_inode node0 = step->node;
    uint32_t last0 = step->last_alloc, total0 = step->total;
    if (step->total == 0 && step->len && !zero_old_tail(sbi, &step->node, step->offset, 1))
        return step_fail(sbi, step, &node0, last0, total0);
    if (step->total < step->len) {
        uint64_t file_off = step->offset + step->total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > step->len - step->total) chunk = step->len - step->total;

        uint32_t prefer_group = (uint32_t)(step->ino / sbi->sb.ipg);
        uint32_t leaf_blk, leaf_slot, existing;
        if (!t3_map_get_or_alloc_tables(sbi, &step->node, bi, prefer_group, &leaf_blk, &leaf_slot, &existing)) {
            return step_fail(sbi, step, &node0, last0, total0);
        }
        uint32_t blk = existing;
        int fresh = 0;
        if (!blk) {
            blk = t3_alloc_block(sbi, prefer_group, step->last_alloc);
            if (!blk) return step_fail(sbi, step, &node0, last0, total0);
            t3_map_set_block(sbi, &step->node, leaf_blk, leaf_slot, blk);
            fresh = 1;
        }
        step->last_alloc = blk;

        int ok;
        if (chunk == T3_BLOCK) {
            ok = t3_write_block(sbi, blk, step->src + step->total);
        } else {
            if (!block_has_live_bytes(fresh, bi, step->node.size)) {
                k_memset(sbi->blk, 0, T3_BLOCK);
            } else if (!t3_read_block(sbi, blk, sbi->blk)) {
                return step_fail(sbi, step, &node0, last0, total0);
            } else {
                zero_past_eof(sbi->blk, bi, step->node.size, within, chunk);
            }
            k_memcpy(sbi->blk + within, step->src + step->total, chunk);
            ok = t3_write_block(sbi, blk, sbi->blk);
        }
        if (!ok) return step_fail(sbi, step, &node0, last0, total0);
        step->total += chunk;
        // LANDED BEFORE RETURNING: other ops run between two steps, and
        // one that drops the pointer cache (another stream's begin())
        // would discard this stream's block pointers with it.
        if (step->total < step->len) {
            if (!t3_pcache_flush(sbi)) return step_fail(sbi, step, &node0, last0, total0);
            // Tables it journaled ride the running transaction; the inode
            // that makes the blocks the file's still waits for the last step.
            if (sbi->txn_hopen) t3_txn_stop(sbi);
            t3_alog_commit(sbi);
            return 0 /* FS_STEP_PENDING */;
        }
    }

    // Final step: land the pointer cache, the allocation state, and
    // the inode -- the same commit point do_write() has.
    if (!t3_pcache_flush(sbi)) return step_fail(sbi, step, &node0, last0, total0);
    // A GROW OVER ANOTHER STREAM'S BLOCKS: begin() cut what lay between the
    // old end and this stream's range, so anything mapped there now is a
    // stream's that began since -- and would read back as this file's data.
    uint32_t first = (uint32_t)((step->orig.size + T3_BLOCK - 1) / T3_BLOCK);
    if (step->len && step->offset / T3_BLOCK > first &&
        mapped_in(sbi, &step->node, first, (uint32_t)(step->offset / T3_BLOCK))) {
        // As a step that pends: its tables ride the running transaction,
        // and what they map is past end-of-file now.
        if (sbi->txn_hopen) t3_txn_stop(sbi);
        t3_alog_commit(sbi);
        step_free(sbi, step);
        return 2 /* FS_STEP_FAILED */;
    }
    if (step->len && step->offset + step->len > step->node.size) step->node.size = step->offset + step->len;
    t3_touch(&step->node);
    int ok = t3_flush_alloc_state(sbi) && t3_txn_begin_or_join(sbi, 1);
    if (ok && !t3_txn_stage_inode(sbi, step->ino, &step->node)) { t3_txn_reset(sbi); ok = 0; }
    if (ok) ok = t3_txn_commit(sbi);
    if (!ok) return step_fail(sbi, step, &node0, last0, total0);
    t3_alog_commit(sbi);
    step_free(sbi, step);
    return 1 /* FS_STEP_DONE */;
}
