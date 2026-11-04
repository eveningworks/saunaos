// TFS3's fsck: checking a volume against itself, and repairing it.
// Split out of tfs3.c; tfs3_internal.h has the map.

#include "tfs3_internal.h"
#include "string.h"
#include "klog.h"
#include "heap.h"
#include "errno.h"
#include "mount.h"   // a repair waits out other ops (mount_wait())
#include "kfmt.h"    // klog_printf()

// ---- fsck -----------------------------------------------------------------
//
// One pass: walk the namespace from the root, mark every reachable
// block and inode, then reconcile against the allocation bitmaps.
// Result-field mapping keeps fs_check_result's TFS2-era meanings:
// records_used = reachable inodes, leaked/referenced_but_free/
// double_allocated/out_of_range = blocks, exactly as fs.h documents.
// Repair semantics follow the same rules as TFS2's: reclaim leaks
// (and TRIM them), re-mark referenced-but-free, zero out-of-range
// pointers, NEVER resolve a double allocation. TFS3 additions: link
// counts verified against observed name counts (repaired on a repair
// pass), free-count caches recomputed, and a primary superblock that
// was bad at mount (we ran from a backup) is rewritten -- on repair
// only, never automatically (see the design doc's backup rules).

// THE MAPS ARE PER GROUP AND ALLOCATED WHEN A GROUP IS FIRST TOUCHED, so
// a check's memory follows what the volume USES: a whole-volume array was
// 1 bit per block plus a byte per inode, ~480 MB on an 8 TB disk however
// empty (e2fsck's sparse bitmaps answer the same problem). A group nothing
// reaches has no page, and reads as all zero.
struct t3_fsck {
    uint8_t **breach;    // per group: block reachability, T3_BLOCK bytes
    uint8_t **ireach;    // per group: inode reachability, T3_BLOCK bytes
    uint8_t **names;     // per group: observed name count per inode, u8 saturating
    int nomem;           // a page could not be had: the result is not trustworthy
    int partial;         // part of the tree could not be read: see tfs3_check()
    int extra;           // walking an open stream: what is marked already is not a double
    int in_dir;          // walking a directory's blocks: a bad pointer there is never cleared
    uint32_t *zap;       // repair: pointer tables holding an out-of-range word, cleared
    uint32_t nzap, zcap; //   after the walk, and only when no block is shared
    struct fsck_dotdot { uint32_t blk, off, was, parent; } *dfix;   // repair: `..` entries to
    uint32_t ndfix, dfcap;                                           //   repoint, likewise
    struct fs_check_result *r;
    int repair;
    int link_mismatches;
    struct fs_check_live *live;
    int stopped;         // FSCK_STOP was honoured: unwind, report nothing
};

// Group g's page of `tab`, allocated zeroed on first use; NULL when out
// of memory (and the pass is then abandoned, not believed).
static uint8_t *fk_page(struct t3_fsck *fk, uint8_t **tab, uint32_t g, size_t bytes) {
    if (!tab[g] && !(tab[g] = kzalloc(bytes))) fk->nomem = 1;
    return tab[g];
}

static int fk_bit(uint8_t **tab, uint32_t g, uint32_t i) {
    return tab[g] ? (tab[g][i >> 3] >> (i & 7)) & 1 : 0;
}

static int fsck_block_ok(struct t3_state *sbi, uint32_t blk) {
    if (blk < sbi->group0 + sbi->meta_off) return 0;
    uint32_t g = (blk - sbi->group0) / T3_BPG;
    uint32_t i = (blk - sbi->group0) % T3_BPG;
    if (g >= sbi->sb.gc) return 0;
    if (i < sbi->meta_off || i >= t3_group_data_end(sbi, g)) return 0;
    return 1;
}

// Mark one referenced block; counts out-of-range and doubles.
// Returns 1 if the block is usable (in range, first reference).
static int fsck_mark_block(struct t3_state *sbi, struct t3_fsck *fk, uint32_t blk) {
    if (!fsck_block_ok(sbi, blk)) { fk->r->out_of_range++; return 0; }
    uint32_t idx = blk - sbi->group0;
    uint32_t g = idx / T3_BPG, i = idx % T3_BPG;
    uint8_t *page = fk_page(fk, fk->breach, g, T3_BLOCK);
    if (!page) return 0;
    uint8_t *b = &page[i >> 3];
    if (*b & (1u << (i & 7))) { if (!fk->extra) fk->r->double_allocated++; return 0; }
    *b |= (uint8_t)(1u << (i & 7));
    fk->r->blocks_referenced++;
    return 1;
}

// A POINTER TABLE ALREADY MARKED is not walked again -- and what it maps
// is then marked by nobody else, so it would look leaked: a block shared
// with another file's data or table hides a whole file from the pass.
// The pass reclaims nothing (an open stream's walk expects its tables).
static int fsck_mark_table(struct t3_state *sbi, struct t3_fsck *fk, uint32_t blk) {
    if (fsck_mark_block(sbi, fk, blk)) return 1;
    if (!fk->extra && !fk->partial)
        klog_printf(KLOG_ERR "tfs3 fsck: pointer table %u is shared -- what it maps not walked\n", blk);
    if (!fk->extra) fk->partial = 1;
    return 0;
}

// An out-of-range pointer in a DIRECTORY is never cleared: the names in
// the block it lost would look leaked, and the next pass frees them. It
// stays reported, and the pass reclaims nothing.
static int fsck_bad_ptr(struct t3_fsck *fk) {
    fk->r->out_of_range++;
    if (fk->in_dir) { fk->partial = 1; return 0; }
    return fk->repair;
}

// Walk one inode's whole block tree (data + pointer blocks). On a
// repair pass an out-of-range pointer in the INODE is zeroed at once,
// through the journal; a table holding one is only noted, and cleared
// by fsck_zap_tables() once the walk has shown no block is shared -- a
// "table" cross-linked with another file's data would be that data.
static void fsck_walk_table(struct t3_state *sbi, struct t3_fsck *fk, uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl) { fk->nomem = 1; return; }
    if (!t3_read_block(sbi, table_blk, tbl)) { fk->partial = 1; kfree(tbl); return; }
    int noted = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(tbl + i * 4);
        if (!e) continue;
        if (!fsck_block_ok(sbi, e)) {
            if (fsck_bad_ptr(fk) && !noted) {
                noted = 1;
                if (fk->nzap == fk->zcap) {
                    uint32_t cap = fk->zcap ? fk->zcap * 2 : 16;
                    uint32_t *z = krealloc(fk->zap, cap * sizeof *z);
                    if (!z) { fk->nomem = 1; break; }
                    fk->zap = z;
                    fk->zcap = cap;
                }
                fk->zap[fk->nzap++] = table_blk;
            }
            continue;
        }
        if (depth == 0) fsck_mark_block(sbi, fk, e);
        else if (fsck_mark_table(sbi, fk, e)) fsck_walk_table(sbi, fk, e, depth - 1);
    }
    kfree(tbl);
}

// Clear the noted tables' out-of-range words, each in its own journaled
// handle (an existing table changes only through the journal).
static void fsck_zap_tables(struct t3_state *sbi, struct t3_fsck *fk) {
    if (!fk->nzap) return;
    // NO DOUBLES PROVES NOTHING ON A PARTIAL PASS: a file it never walked
    // marked nothing, and its data may be the "table" noted here.
    if (fk->r->double_allocated || fk->partial) {
        klog_write(KLOG_ERR "tfs3 fsck: blocks may be shared -- pointer tables not repaired\n");
        return;
    }
    for (uint32_t z = 0; z < fk->nzap; z++) {
        uint32_t blk = fk->zap[z], cleared = 0;
        uint8_t *p;
        if (!t3_txn_begin(sbi, 1) || !(p = t3_txn_stage(sbi, blk))) { t3_txn_reset(sbi); continue; }
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(p + i * 4);
            if (e && !fsck_block_ok(sbi, e)) { wr32(p + i * 4, 0); cleared++; }
        }
        if (t3_txn_commit(sbi)) fk->r->pointers_cleared += cleared;
        else t3_txn_reset(sbi);
        t3_map_cache_forget(sbi, blk);   // or a cached copy writes the bad pointer back
        t3_rcache_drop(sbi);
    }
}

// Repoint the noted `..` entries, each in its own journaled handle -- after
// the walk, so a block shared with another folder or a file's data (the
// pass found doubles) is never written, and only when the pass saw it all.
static void fsck_fix_dotdots(struct t3_state *sbi, struct t3_fsck *fk) {
    if (!fk->ndfix) return;
    if (fk->r->double_allocated || fk->partial) {
        klog_write(KLOG_ERR "tfs3 fsck: blocks may be shared -- `..` entries not repaired\n");
        return;
    }
    for (uint32_t k = 0; k < fk->ndfix; k++) {
        const struct fsck_dotdot *d = &fk->dfix[k];
        uint8_t *p;
        if (!t3_txn_begin(sbi, 1) || !(p = t3_txn_stage(sbi, d->blk))) { t3_txn_reset(sbi); continue; }
        if (rd32(p + d->off) != d->was || p[d->off + 6] != 2 || p[d->off + 7] != '.' || p[d->off + 8] != '.') {
            t3_txn_reset(sbi);   // not what the walk read: leave it
            continue;
        }
        wr32(p + d->off, d->parent);
        if (t3_txn_commit(sbi)) {
            fk->r->dirs_fixed++;
            klog_printf("tfs3 fsck: `..` in block %u repointed at inode %u -- repaired\n", d->blk, d->parent);
        } else {
            t3_txn_reset(sbi);
        }
    }
}

static void fsck_walk_inode_blocks(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino, struct t3_inode *node) {
    int inode_dirty = 0;
    for (int i = 0; i < 12; i++) {
        if (!node->ptrs[i]) continue;
        if (!fsck_block_ok(sbi, node->ptrs[i])) {
            if (fsck_bad_ptr(fk)) { node->ptrs[i] = 0; inode_dirty++; }
        } else {
            fsck_mark_block(sbi, fk, node->ptrs[i]);
        }
    }
    for (int p = 12; p <= 14; p++) {
        if (!node->ptrs[p]) continue;
        if (!fsck_block_ok(sbi, node->ptrs[p])) {
            if (fsck_bad_ptr(fk)) { node->ptrs[p] = 0; inode_dirty++; }
        } else if (fsck_mark_table(sbi, fk, node->ptrs[p])) {
            fsck_walk_table(sbi, fk, node->ptrs[p], p - 12);
        }
    }
    if (inode_dirty) {
        if (t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, node) && t3_txn_commit(sbi))
            fk->r->pointers_cleared += inode_dirty;
        else t3_txn_reset(sbi);
    }
}

static void fsck_mark_ino(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg), i = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc) return;
    uint8_t *page = fk_page(fk, fk->ireach, g, T3_BLOCK);
    if (page) page[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static int fsck_ino_reached(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg), i = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc) return 1;
    return fk_bit(fk->ireach, g, i);
}

// Does the folder map a block at index >= `n`? One past the folder format's
// reach (a double-indirect table) always counts.
static int dir_maps_past(struct t3_state *sbi, const struct t3_inode *dir, uint32_t n) {
    for (uint32_t i = n; i < 12; i++) if (dir->ptrs[i]) return 1;
    if (dir->ptrs[13] || dir->ptrs[14]) return 1;
    if (!dir->ptrs[12]) return 0;
    if (n <= 12) return 1;
    for (uint32_t b = n; b < T3_DIR_MAX_BLOCKS; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, dir, b, &blk) || blk) return 1;
    }
    return 0;
}

// Depth-capped DFS over the directory tree. 32 components is far past
// anything the 64-byte caller paths can even express today; a deeper
// tree gets a klog and an unwalked subtree (reported as leaks --
// wrong, but loudly wrong).
static void fsck_walk_dir(struct t3_state *sbi, struct t3_fsck *fk, uint64_t dir_ino, uint64_t parent_ino, int depth) {
    if (fk->stopped) return;
    if (depth > 32) {
        klog_write("tfs3 fsck: directory nesting past 32 -- subtree not walked\n");
        fk->partial = 1;
        return;
    }
    struct t3_inode dir;
    if (!t3_read_inode(sbi, dir_ino, &dir)) { fk->partial = 1; return; }
    if (dir.type != T3_TYPE_DIR) return;
    fk->r->records_used++;
    if (fsck_step(fk->live, fk->r->records_used, fk->r)) fk->stopped = 1;
    fk->in_dir = 1;
    fsck_walk_inode_blocks(sbi, fk, dir_ino, &dir);
    fk->in_dir = 0;

    uint32_t nblocks = t3_dir_blocks(dir.size);
    // BLOCKS PAST WHAT ITS SIZE CLAIMS were marked above, but no name in
    // them is read (lookup reads none either): what they reach must not
    // be reclaimed as unreached.
    if (dir_maps_past(sbi, &dir, nblocks)) {
        fk->r->dir_errors++;
        fk->partial = 1;
        klog_printf(KLOG_ERR "tfs3 fsck: folder inode %u has blocks past its size\n", (uint32_t)dir_ino);
    }
    for (uint32_t b = 0; b < nblocks && !fk->stopped; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, &dir, b, &blk)) { fk->partial = 1; continue; }
        if (!blk) continue;   // a hole: nothing to read
        if (!fsck_block_ok(sbi, blk)) continue;   // counted by the walk above; partial
        uint8_t *dirblk = kmalloc(T3_BLOCK);
        if (!dirblk) { fk->nomem = 1; return; }
        if (!t3_read_block(sbi, blk, dirblk)) { fk->partial = 1; kfree(dirblk); continue; }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK && !fk->stopped) {
            uint32_t e_ino = rd32(dirblk + off);
            uint16_t rec_len = rd16(dirblk + off + 4);
            uint8_t nl = dirblk[off + 6];
            // A BROKEN CHAIN HIDES THE REST OF THE BLOCK, and what those
            // names reach must not be reclaimed as unreached.
            if (t3_dirent_bad(off, rec_len, nl) || (e_ino && !nl)) {
                fk->r->dir_errors++;
                fk->partial = 1;
                klog_printf(KLOG_ERR "tfs3 fsck: corrupt dirent chain in inode %u\n", (uint32_t)dir_ino);
                break;
            }
            if (e_ino && t3_name_bad(dirblk + off + 7, nl)) {
                // Unreachable by any path, and never listed; what it names
                // is still walked, so it is not reclaimed.
                fk->r->dir_errors++;
                klog_printf(KLOG_ERR "tfs3 fsck: a name in inode %u holds '/' or NUL\n", (uint32_t)dir_ino);
            }
            if (e_ino != 0) {
                int is_dot = (nl == 1 && dirblk[off + 7] == '.');
                int is_dotdot = (nl == 2 && dirblk[off + 7] == '.' && dirblk[off + 8] == '.');
                uint64_t child = e_ino;
                // `.` AND `..` COUNT FOR WHAT THE WALK KNOWS THEY NAME: one
                // pointing away is reported, and must not "repair" the
                // link count of the inode it wrongly names.
                uint64_t counted = is_dot ? dir_ino : is_dotdot ? parent_ino : child;
                if (counted < (uint64_t)sbi->sb.gc * sbi->sb.ipg) {
                    uint8_t *page = fk_page(fk, fk->names, (uint32_t)(counted / sbi->sb.ipg), sbi->sb.ipg);
                    uint8_t *nc = page ? &page[counted % sbi->sb.ipg] : 0;
                    if (nc && *nc < 255) (*nc)++;
                }
                if (is_dot) {
                    if (child != dir_ino) {
                        fk->r->dir_errors++;
                        klog_write("tfs3 fsck: `.` points away from its own directory\n");
                    }
                } else if (is_dotdot) {
                    if (child != parent_ino) {
                        fk->r->dir_errors++;
                        klog_write("tfs3 fsck: `..` points away from the parent\n");
                        // The walk knows the parent, so this one is
                        // repairable -- a shrink interrupted while it
                        // renumbered a big folder leaves exactly this.
                        // Noted, and written after the walk (fsck_fix_dotdots()).
                        if (fk->repair) {
                            if (fk->ndfix == fk->dfcap) {
                                uint32_t cap = fk->dfcap ? fk->dfcap * 2 : 8;
                                struct fsck_dotdot *d = krealloc(fk->dfix, cap * sizeof *d);
                                if (!d) { fk->nomem = 1; kfree(dirblk); return; }
                                fk->dfix = d;
                                fk->dfcap = cap;
                            }
                            fk->dfix[fk->ndfix++] = (struct fsck_dotdot){ blk, off, (uint32_t)child,
                                                                          (uint32_t)parent_ino };
                        }
                    }
                } else {
                    struct t3_inode cn;
                    if (!t3_read_inode(sbi, child, &cn)) {
                        // Its blocks -- a folder's whole subtree -- are not
                        // known, so nothing may be reclaimed as unreached.
                        fk->r->bad_names++;
                        fk->partial = 1;
                        fsck_mark_ino(sbi, fk, child);
                        klog_write(KLOG_ERR "tfs3 fsck: dirent -> inode ");
                        klog_write_dec((uint32_t)child);
                        klog_write(" whose checksum fails (not repaired -- deleting a name is data loss)\n");
                    } else if (fsck_ino_reached(sbi, fk, child)) {
                        // Already visited: fine for files (hardlink),
                        // never for dirs.
                        if (cn.type == T3_TYPE_DIR) {
                            fk->r->dir_errors++;
                            klog_write("tfs3 fsck: directory reachable by two names\n");
                        }
                    } else {
                        fsck_mark_ino(sbi, fk, child);
                        if (cn.type == T3_TYPE_DIR) {
                            fsck_walk_dir(sbi, fk, child, dir_ino, depth + 1);
                        } else {
                            fk->r->records_used++;
                            if (fsck_step(fk->live, fk->r->records_used, fk->r)) fk->stopped = 1;
                            fsck_walk_inode_blocks(sbi, fk, child, &cn);
                        }
                    }
                }
            }
            off += rec_len;
        }
        kfree(dirblk);
    }
}

// An open stream's inode copy, walked like a file's: only to MARK. Its
// repairs would commit the stream's half-built copy over the real inode.
static void fsck_walk_stream(struct t3_state *sbi, uint64_t ino, const struct t3_inode *node, void *ctx) {
    struct t3_fsck *fk = ctx;
    struct t3_inode copy = *node;
    int repair = fk->repair;
    fk->repair = 0;
    fk->extra = 1;
    fsck_walk_inode_blocks(sbi, fk, ino, &copy);
    fk->extra = 0;
    fk->repair = repair;
}

// What fsck_begin() names; Disks lists them, `fsck` prints the current one.
static const struct fsck_stage_desc T3_STAGES[] = {
    { "Walk every file", "files" },
    { "Compare the allocation maps", "groups" },
    { "Check link counts", "inodes" },
};

// The maps the pass allocated; every return after them goes through here.
static void fsck_free(struct t3_fsck *fk, uint32_t gc) {
    for (uint32_t g = 0; g < gc; g++) {
        if (fk->breach) kfree(fk->breach[g]);
        if (fk->ireach) kfree(fk->ireach[g]);
        if (fk->names) kfree(fk->names[g]);
    }
    kfree(fk->breach);
    kfree(fk->ireach);
    kfree(fk->names);
    kfree(fk->zap);
    kfree(fk->dfix);
}

int tfs3_check(void *st, int repair, struct fs_check_result *out, struct fs_check_live *live) {
    struct t3_state *sbi = st;
    struct fs_check_result local;
    struct fs_check_result *r = out ? out : &local;
    k_memset(r, 0, sizeof(*r));
    if (!sbi->mounted) return 0;

    // LAND ANY DEFERRED COMMIT FIRST. fsck walks what is ON THE DISK,
    // and under `storage.sync = batched` an inode update can be sitting
    // in the journal staging buffer -- so the blocks it references look
    // allocated-but-unreferenced and get counted as LEAKED. They are
    // not: they are referenced by an inode that has not landed yet.
    // Reporting a healthy filesystem as leaking is exactly the kind of
    // false alarm that teaches people to ignore the checker.
    // AND IT MUST LAND WHEN DELETES' FREES WAIT FOR IT: their blocks are
    // allocated and named by no inode, so they look leaked -- and a repair
    // clearing them lets the delete's commit free them a second time.
    if (!t3_txn_flush_deferred(sbi) && t3_pf_pending(sbi)) {
        klog_write(KLOG_ERR "tfs3 fsck: a delete's commit could not land -- not checked\n");
        return 0;
    }

    // A PASS WAITS OUT EVERY OP HOLDING ONE OF THIS VOLUME'S INODES: each
    // op that drops the lock mid-way holds one, so none is in a gap, and a
    // block a delete is between freeing is neither reclaimed under it nor
    // reported leaked. Waited like t3_lock() does, and run again from the
    // top. AFTER THE FLUSH, which can wait out a commit with the lock
    // dropped: a delete let in there is between frees when it returns.
    if (tfs3_locks_held(sbi)) {
        if (!mount_can_wait(sbi)) {
            klog_write("tfs3 fsck: the volume is busy -- not checked\n");
            return 0;
        }
        if (mount_wait(sbi, &sbi->ilock_chan) > 0) mount_op_restart(sbi);
        return 0;
    }

    struct t3_fsck fk;
    k_memset(&fk, 0, sizeof(fk));
    fk.r = r;
    fk.repair = repair;
    fk.live = live;
    uint32_t gc = sbi->sb.gc;
    uint64_t total_inodes = (uint64_t)gc * sbi->sb.ipg;
    fk.breach = kzalloc(sizeof(uint8_t *) * gc);
    fk.ireach = kzalloc(sizeof(uint8_t *) * gc);
    fk.names = kzalloc(sizeof(uint8_t *) * gc);
    if (!fk.breach || !fk.ireach || !fk.names) {
        klog_write("tfs3 fsck: not enough memory for the reachability maps -- not checked\n");
        fsck_free(&fk, 0);
        return 0;
    }

    fsck_begin(live, repair, T3_STAGES, 3);
    uint64_t in_use = total_inodes;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) in_use -= sbi->gd[g].free_inodes;
    fsck_stage(live, 0, (uint32_t)in_use);
    fsck_mark_ino(sbi, &fk, T3_INO_ROOT);
    fsck_walk_dir(sbi, &fk, T3_INO_ROOT, T3_INO_ROOT, 0);
    t3_steps_each(sbi, fsck_walk_stream, &fk);
    if (repair && !fk.nomem && !fk.stopped) {
        fsck_zap_tables(sbi, &fk);
        fsck_fix_dotdots(sbi, &fk);
    }
    if (fk.nomem) {
        klog_write("tfs3 fsck: ran out of memory for the reachability maps -- not checked\n");
        fsck_free(&fk, gc);
        fsck_end(live);
        return 0;
    }
    if (fk.stopped) { fsck_free(&fk, gc); fsck_end(live); return -ECANCELED; }
    // A WALK THAT MISSED PART OF THE TREE undercounts: what it could not
    // read looks leaked, and its names look missing. Reported, but a
    // repair takes nothing back and changes no link count -- reclaiming
    // a block an unread table still names hands it out twice. Marking a
    // referenced block allocated stays safe, and still runs.
    int reclaim = repair && !fk.partial;
    if (fk.partial)
        klog_write(KLOG_ERR "tfs3 fsck: part of the tree could not be read -- "
                   "leaks and link counts reported, not repaired\n");

    fsck_stage(live, 1, sbi->sb.gc);
    int forgot = 0;

    // Reconcile blocks: reach map vs allocation bitmap, per group.
    // Metadata and backup regions are allocated-by-design and outside
    // the reach map, so only the data area is compared.
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        uint32_t end = t3_group_data_end(sbi, g);
        uint32_t free_b = 0;
        uint32_t leak_run_start = 0, leak_run_len = 0;
        // A group whose bitmap was never written and that nothing reaches
        // agrees with itself by construction: every data block free.
        int untouched_b = (sbi->gd[g].flags & T3_BG_BLOCK_UNINIT) && !fk.breach[g];
        if (untouched_b) free_b = end > sbi->meta_off ? end - sbi->meta_off : 0;
        // A BITMAP THAT WILL NOT READ answers "allocated" for every bit, and
        // a repair would reclaim -- and TRIM -- whatever nothing reached.
        // The pass stops there, believed by nobody; the groups before it
        // were compared against bitmaps that did read.
        if (!t3_bmap_readable(sbi, g, T3_BMAP_BLOCK) || !t3_bmap_readable(sbi, g, T3_BMAP_INODE)) {
            klog_printf(KLOG_ERR "tfs3 fsck: group %u's bitmaps cannot be read -- not checked\n", g);
            fsck_free(&fk, gc);
            fsck_end(live);
            return 0;
        }
        for (uint32_t i = 0; i < T3_BPG && !untouched_b; i++) {
            int alloc = t3_bbm_test(sbi, g, i);
            if (i < sbi->meta_off || i >= end) {
                // Format-owned: the group's metadata, a backup region,
                // blocks past the volume's end. Always in use -- a clear
                // bit here would hand out a bitmap or the table.
                if (!alloc) {
                    r->referenced_but_free++;
                    if (repair) { t3_bbm_set(sbi, g, i, 1); r->marked_allocated++; }
                }
                continue;
            }
            int reach = fk_bit(fk.breach, g, i);
            uint32_t blk = t3_group_base(sbi, g) + i;
            // A RECLAIM IS A FREE: behind the last commit's fence, as
            // t3_free_block_bit() is, and no cached copy of its old life
            // may outlive it -- a fresh table would be handed that image.
            if (alloc && !reach) {
                r->leaked++;
                if (reclaim && t3_jlast_fence(sbi, blk)) {
                    t3_bbm_set(sbi, g, i, 0);
                    r->reclaimed++;
                    t3_map_cache_forget(sbi, blk);
                    forgot = 1;
                    if (leak_run_len && blk == leak_run_start + leak_run_len) leak_run_len++;
                    else { t3_trim_run(sbi, leak_run_start, leak_run_len); leak_run_start = blk; leak_run_len = 1; }
                }
            } else if (!alloc && reach) {
                r->referenced_but_free++;
                if (repair) { t3_bbm_set(sbi, g, i, 1); r->marked_allocated++; }
            }
            if (!t3_bbm_test(sbi, g, i)) free_b++;
        }
        if (repair) { t3_trim_run(sbi, leak_run_start, leak_run_len); t3_trim_flush(sbi); }
        if (forgot) { t3_rcache_drop(sbi); forgot = 0; }

        // Inode bitmap + free counts: reconcile, repair-only writes.
        uint32_t free_i = 0;
        int untouched_i = (sbi->gd[g].flags & T3_BG_INODE_UNINIT) && !fk.ireach[g];
        if (untouched_i) free_i = sbi->sb.ipg;
        for (uint32_t i = 0; i < sbi->sb.ipg && !untouched_i; i++) {
            uint64_t ino = (uint64_t)g * sbi->sb.ipg + i;
            int alloc = t3_ibm_test(sbi, g, i);
            int reach = fk_bit(fk.ireach, g, i);
            if (g == 0 && i == 0) { free_i += !alloc; continue; } // ino 0 reserved
            if (alloc && !reach) {
                // An orphaned inode is the inode-space leak. Reported
                // through the same counter (they are the same failure
                // class); reclaimed on repair.
                r->leaked++;
                if (reclaim) {
                    t3_ibm_set(sbi, g, i, 0);
                    r->reclaimed++;
                    if (t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, 0)) t3_txn_commit(sbi); else t3_txn_reset(sbi);
                }
            } else if (!alloc && reach) {
                r->referenced_but_free++;
                if (repair) { t3_ibm_set(sbi, g, i, 1); r->marked_allocated++; }
            }
            if (!t3_ibm_test(sbi, g, i)) free_i++;
        }

        if (sbi->gd[g].free_blocks != free_b || sbi->gd[g].free_inodes != free_i) {
            r->counts_wrong++;
            if (repair) {
                r->counts_fixed++;
                sbi->gd[g].free_blocks = free_b;
                sbi->gd[g].free_inodes = free_i;
                t3_mark_dirty(sbi->gdt_dirty, g);
            }
        }
        if (fsck_step(live, g + 1, r)) { fk.stopped = 1; break; }
    }
    if (fk.stopped) { fsck_free(&fk, gc); fsck_end(live); return -ECANCELED; }
    uint32_t named = 0;
    for (uint32_t g = 0; g < gc; g++) named += fk.names[g] != 0;
    fsck_stage(live, 2, named * sbi->sb.ipg);

    // Link counts: observed names vs stored counts. A directory's
    // observed count from the walk is its own dirent + `.` + each
    // child's `..`, which is exactly the 2+subdirs rule -- so one
    // comparison covers both types. Only groups something named.
    uint32_t walked = 0;
    for (uint64_t ino = 1; ino < total_inodes && !fk.stopped; ino++) {
        uint32_t g = (uint32_t)(ino / sbi->sb.ipg), i = (uint32_t)(ino % sbi->sb.ipg);
        if (!fk.names[g]) { ino = (uint64_t)(g + 1) * sbi->sb.ipg - 1; continue; }
        if (!(++walked & 255) && fsck_step(live, walked, r)) { fk.stopped = 1; break; }
        uint8_t nm = fk.names[g][i];
        if (!fsck_ino_reached(sbi, &fk, ino) && ino != T3_INO_ROOT) continue;
        if (!nm && ino != T3_INO_ROOT) continue;
        struct t3_inode node;
        if (!t3_read_inode(sbi, ino, &node)) continue;
        // One rule covers files, dirs AND the root: names[] counted
        // every dirent pointing at the inode, including `.`/`..`. A
        // dir gets parent-entry + own-`.` + children's `..` = 2+subdirs;
        // the root lacks a parent entry but its own `..` points at
        // itself, which restores the same total. Files get their
        // hardlink count.
        uint16_t want = nm;
        if (node.links != want && nm < 255) {
            fk.link_mismatches++;
            r->links_wrong++;
            klog_write("tfs3 fsck: inode "); klog_write_dec((uint32_t)ino);
            klog_write(" links="); klog_write_dec(node.links);
            klog_write(" but "); klog_write_dec(want);
            klog_write(" name(s) observed");
            if (reclaim) {
                node.links = want;
                if (t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, &node) && t3_txn_commit(sbi)) {
                    r->links_fixed++;
                    klog_write(" -- repaired");
                }
                else t3_txn_reset(sbi);
            }
            klog_write("\n");
        }
    }

    if (repair) {
        if (!t3_flush_alloc_state(sbi)) {
            klog_write(KLOG_ERR "tfs3 fsck: the repaired allocation maps could not be written\n");
            fsck_free(&fk, gc);
            fsck_end(live);
            return 0;
        }
        // A primary superblock that failed at mount (we're running
        // from a backup) gets rewritten now, deliberately here and
        // never automatically at mount -- see the design doc.
        if (sbi->mounted_from_backup) {
            if (t3_write_superblock_everywhere(sbi)) {
                klog_write("tfs3 fsck: primary superblock restored from the mounted backup\n");
                sbi->mounted_from_backup = 0;
            }
        }
    }

    fsck_free(&fk, gc);
    fsck_end(live);
    return fk.stopped ? -ECANCELED : 1;
}
