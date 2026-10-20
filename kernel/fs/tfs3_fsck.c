// TFS3's fsck: checking a volume against itself, and repairing it.
// Split out of tfs3.c; tfs3_internal.h has the map.

#include "tfs3_internal.h"
#include "string.h"
#include "klog.h"
#include "heap.h"
#include "errno.h"

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
    if (*b & (1u << (i & 7))) { fk->r->double_allocated++; return 0; }
    *b |= (uint8_t)(1u << (i & 7));
    fk->r->blocks_referenced++;
    return 1;
}

// Walk one inode's whole block tree (data + pointer blocks). `zap`
// support: on a repair pass, out-of-range pointers found in the INODE
// itself are zeroed via a transaction; ones inside pointer blocks are
// zeroed in place (data-class blocks, unjournaled like all data).
static void fsck_walk_table(struct t3_state *sbi, struct t3_fsck *fk, uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl || !t3_read_block(sbi, table_blk, tbl)) { if (tbl) kfree(tbl); return; }
    int dirty = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(tbl + i * 4);
        if (!e) continue;
        if (!fsck_block_ok(sbi, e)) {
            fk->r->out_of_range++;
            if (fk->repair) { wr32(tbl + i * 4, 0); dirty = 1; fk->r->pointers_cleared++; }
            continue;
        }
        if (depth == 0) fsck_mark_block(sbi, fk, e);
        else if (fsck_mark_block(sbi, fk, e)) fsck_walk_table(sbi, fk, e, depth - 1);
    }
    if (dirty) t3_write_block(sbi, table_blk, tbl);
    kfree(tbl);
}

static void fsck_walk_inode_blocks(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino, struct t3_inode *node) {
    int inode_dirty = 0;
    for (int i = 0; i < 12; i++) {
        if (!node->ptrs[i]) continue;
        if (!fsck_block_ok(sbi, node->ptrs[i])) {
            fk->r->out_of_range++;
            if (fk->repair) { node->ptrs[i] = 0; inode_dirty = 1; fk->r->pointers_cleared++; }
        } else {
            fsck_mark_block(sbi, fk, node->ptrs[i]);
        }
    }
    for (int p = 12; p <= 14; p++) {
        if (!node->ptrs[p]) continue;
        if (!fsck_block_ok(sbi, node->ptrs[p])) {
            fk->r->out_of_range++;
            if (fk->repair) { node->ptrs[p] = 0; inode_dirty = 1; fk->r->pointers_cleared++; }
        } else if (fsck_mark_block(sbi, fk, node->ptrs[p])) {
            fsck_walk_table(sbi, fk, node->ptrs[p], p - 12);
        }
    }
    if (inode_dirty) {
        if (t3_txn_begin(sbi, 1) && t3_txn_stage_inode(sbi, ino, node)) t3_txn_commit(sbi); else t3_txn_reset(sbi);
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

// Depth-capped DFS over the directory tree. 32 components is far past
// anything the 64-byte caller paths can even express today; a deeper
// tree gets a klog and an unwalked subtree (reported as leaks --
// wrong, but loudly wrong).
static void fsck_walk_dir(struct t3_state *sbi, struct t3_fsck *fk, uint64_t dir_ino, uint64_t parent_ino, int depth) {
    if (fk->stopped) return;
    if (depth > 32) {
        klog_write("tfs3 fsck: directory nesting past 32 -- subtree not walked\n");
        return;
    }
    struct t3_inode dir;
    if (!t3_read_inode(sbi, dir_ino, &dir) || dir.type != T3_TYPE_DIR) return;
    fk->r->records_used++;
    if (fsck_step(fk->live, fk->r->records_used, fk->r)) fk->stopped = 1;
    fsck_walk_inode_blocks(sbi, fk, dir_ino, &dir);

    uint32_t nblocks = (uint32_t)((dir.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks && !fk->stopped; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, &dir, b, &blk)) continue;
        uint8_t *dirblk = kmalloc(T3_BLOCK);
        if (!dirblk) return;
        if (!blk || !t3_read_block(sbi, blk, dirblk)) { kfree(dirblk); continue; }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK && !fk->stopped) {
            uint32_t e_ino = rd32(dirblk + off);
            uint16_t rec_len = rd16(dirblk + off + 4);
            uint8_t nl = dirblk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) {
                fk->r->dir_errors++;
                klog_write(KLOG_ERR "tfs3 fsck: corrupt dirent chain in inode ");
                klog_write_dec((uint32_t)dir_ino); klog_write("\n");
                break;
            }
            if (e_ino != 0 && nl > 0) {
                int is_dot = (nl == 1 && dirblk[off + 7] == '.');
                int is_dotdot = (nl == 2 && dirblk[off + 7] == '.' && dirblk[off + 8] == '.');
                uint64_t child = e_ino;
                if (child < (uint64_t)sbi->sb.gc * sbi->sb.ipg) {
                    uint8_t *page = fk_page(fk, fk->names, (uint32_t)(child / sbi->sb.ipg), sbi->sb.ipg);
                    uint8_t *nc = page ? &page[child % sbi->sb.ipg] : 0;
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
                    }
                } else {
                    struct t3_inode cn;
                    if (!t3_read_inode(sbi, child, &cn)) {
                        fk->r->bad_names++;
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
    t3_txn_flush_deferred(sbi);

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
    if (fk.nomem) {
        klog_write("tfs3 fsck: ran out of memory for the reachability maps -- not checked\n");
        fsck_free(&fk, gc);
        fsck_end(live);
        return 0;
    }
    if (fk.stopped) { fsck_free(&fk, gc); fsck_end(live); return -ECANCELED; }

    fsck_stage(live, 1, sbi->sb.gc);

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
            if (alloc && !reach) {
                r->leaked++;
                if (repair) {
                    t3_bbm_set(sbi, g, i, 0);
                    r->reclaimed++;
                    uint32_t blk = t3_group_base(sbi, g) + i;
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
                if (repair) {
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
            if (repair) {
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
        t3_flush_alloc_state(sbi);
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
