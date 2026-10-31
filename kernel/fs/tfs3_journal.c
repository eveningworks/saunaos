// TFS3's journal: the fixed-size multi-block transaction, the DEFERRED
// transaction storage.sync=batched uses, and staging an inode through
// it. Split out of tfs3.c; tfs3_internal.h has the map.

#include "tfs3_internal.h"
#include "ktime.h"
#include "caltime.h"
#include "string.h"
#include "storage_config.h" // storage.sync -- whether the barriers are real
#include "klog.h"
#include "heap.h"   // a handle's undo copies
#include "timer.h"  // coarse_ticks() -- when a stopped handle's work was staged

// ---- journal: one fixed-size multi-block transaction ---------------------
//
// What goes THROUGH the journal: dirent blocks and inode-table blocks
// -- the structures whose torn write is NAMESPACE corruption. What
// deliberately does NOT: bitmap and GDT blocks (leak-safe ordering
// above; a torn bitmap costs a leak fsck reclaims), data blocks and
// indirect-pointer blocks (unreachable until the inode that points at
// them commits). This is narrower than the design doc's first sketch
// ("dirent + inode + bitmaps") and is a deliberate revision: bitmaps
// under the leak rule keeps every operation's transaction at <= 3
// slots with the same crash guarantees TFS2 gives, for half the
// journaled bytes. The doc's journal section records this.
//
// Commit discipline = persist_record()'s, generalized (see TFS2 for
// the two-barrier reasoning): stage images + committed header, FLUSH,
// write targets, FLUSH, clear header (no barrier -- a stale committed
// header just replays idempotently).

// ---- HANDLES on one RUNNING transaction (docs/fslock-design.md, 5b) ---
//
// jbd2's shape: each mount has ONE running transaction, and an op stages
// into it through a handle. t3_txn_begin() opens the handle -- joining
// whatever earlier ops left staged under `batched` when its credits fit,
// committing that first when they do not. t3_txn_commit() commits the
// whole running transaction, this handle's work and everything before
// it; t3_txn_stop() leaves it all staged for a later commit; and
// t3_txn_reset() unwinds THIS HANDLE ALONE.
//
// One op runs inside tfs3 at a time (the mount lock), so a handle's own
// slots are always the tail, [txn_hbase, txn_count). A block it patched
// that an EARLIER op had staged is put back from the copy t3_txn_stage()
// took before the first change -- the per-handle undo. Outside a handle
// txn_hbase == txn_count, so "is anything older staged" is txn_hbase > 0.
//
// Under `batched` a write and a create stop rather than commit; many of
// them then share one commit, which is what removes the barriers that
// are 56-69% of block time on real hardware. What a stopped handle
// leaves staged is only METADATA whose data is already on the disk: a
// write has written its blocks and bitmaps, a create its child's dirent
// block. A crash loses those ops whole and leaks what they allocated --
// a leak `fsck` reclaims, never a journal that cannot be replayed. A
// staged block is what every read of it sees (t3_vol_read_sectors()),
// so a name resolves and a size reads back before the commit.

// The transaction is over, committed or not: forget every inode it
// staged, since a read while it was open may have cached a copy the
// commit has just changed (tfs3_internal.h, the inode cache).
static void txn_forget_staged(struct t3_state *sbi) {
    if (sbi->txn_nino > T3_TXN_INO) t3_icache_drop(sbi);
    else for (int i = 0; i < sbi->txn_nino; i++) t3_icache_forget(sbi, sbi->txn_ino[i]);
    sbi->txn_nino = 0;
}

// Only [0, txn_hbase) can hold an undo copy, and every caller closes
// before it moves txn_hbase.
static void handle_close(struct t3_state *sbi) {
    for (int i = 0; i < sbi->txn_hbase; i++)
        if (sbi->txn_undo[i]) { kfree(sbi->txn_undo[i]); sbi->txn_undo[i] = 0; }
    sbi->txn_hopen = 0;
    sbi->txn_hnoundo = 0;
    sbi->txn_hbase = sbi->txn_count;
    sbi->txn_hnino = sbi->txn_nino;
}

// Take this handle's work back out of the running transaction, leaving
// what earlier ops staged exactly as they left it.
static void handle_abort(struct t3_state *sbi) {
    if (!sbi->txn_hopen) return;
    int touched = sbi->txn_count > sbi->txn_hbase;
    for (int i = 0; i < sbi->txn_hbase; i++) {
        if (!sbi->txn_undo[i]) continue;
        k_memcpy(sbi->txn_img[i], sbi->txn_undo[i], T3_BLOCK);
        touched = 1;
    }
    // What a read cached from this handle's images is now wrong.
    if (sbi->txn_nino > T3_TXN_INO) t3_icache_drop(sbi);
    else for (int i = sbi->txn_hnino; i < sbi->txn_nino; i++) t3_icache_forget(sbi, sbi->txn_ino[i]);
    sbi->txn_nino = sbi->txn_hnino;
    if (touched) {
        sbi->ino_gen++;
        t3_ncache_flush(sbi);   // a lookup in the op may have seen its staged name
    }
    sbi->txn_count = sbi->txn_hbase;
    sbi->txn_credits = sbi->txn_hcredits_prev;
    handle_close(sbi);
}

void t3_txn_reset(struct t3_state *sbi) {
    handle_abort(sbi);
}

// Everything staged goes, deferred or not -- only for a mount whose
// state is about to be freed, where nothing is left to commit it into.
void t3_txn_discard(struct t3_state *sbi) {
    handle_abort(sbi);
    sbi->txn_count = 0;
    sbi->txn_credits = 0;
    sbi->txn_deferred = 0;
    txn_forget_staged(sbi);
    handle_close(sbi);
}

// Commit whatever earlier ops left staged on this mount. Safe to call
// with nothing staged; never from inside a handle. Returns 0 only if the
// commit failed: ABORTED keeps the staged work for the next try, and one
// that could not be applied leaves nothing staged and the volume
// read-only (t3_txn_commit()).
int t3_txn_flush_deferred(struct t3_state *sbi) {
    if (!sbi->txn_deferred) return 1;
    return t3_txn_commit(sbi);
}

// Open a handle that will stage at most `credits` DISTINCT blocks of its
// own, jbd2's reservation discipline in miniature: an operation that
// cannot fit says so before it has changed anything, instead of
// discovering it halfway through when t3_txn_stage() returns 0. Returns
// 0 if this volume's journal is too small -- which is the whole reason it
// exists, since a v1 image (four slots) genuinely cannot express a
// directory move -- or if the older work that had to commit first could
// not. Callers turn that into a refusal, not a corrupt half-operation.
//
// A handle an op left open is unwound here, which is what a begin has
// always done to an unfinished transaction (it zeroed the count).
int t3_txn_begin(struct t3_state *sbi, int credits) {
    handle_abort(sbi);
    if (credits <= 0 || credits > (int)sbi->jslots) return 0;
    if (sbi->txn_deferred && sbi->txn_count + credits > (int)sbi->jslots &&
        !t3_txn_flush_deferred(sbi)) return 0;
    if (!sbi->txn_deferred) {
        sbi->txn_count = 0;
        sbi->txn_credits = 0;
        sbi->txn_nino = 0;
    }
    sbi->txn_hcredits_prev = sbi->txn_credits;
    sbi->txn_hbase = sbi->txn_count;
    sbi->txn_hnino = sbi->txn_nino;
    sbi->txn_credits = sbi->txn_count + credits;
    sbi->txn_hopen = 1;
    return 1;
}

// End the handle and leave its work staged in the running transaction,
// to land with the next commit (`batched`). Always 1.
int t3_txn_stop(struct t3_state *sbi) {
    if (sbi->txn_count > 0) {
        sbi->txn_deferred = 1;
        sbi->txn_staged_tick = coarse_ticks();
    }
    handle_close(sbi);
    return 1;
}

// Stage `blk`'s new image into the transaction. Returns a writable
// pointer to the staged 4 KiB image (pre-loaded from disk so callers
// patch in place), or 0 when full/read-failed. Staging the same block
// twice returns the same image.
uint8_t *t3_txn_stage(struct t3_state *sbi, uint32_t blk) {
    for (int i = 0; i < sbi->txn_count; i++) {
        if (sbi->txn_target[i] != blk) continue;
        // An earlier op's block: keep its image before this handle's
        // first change, so a failure can put it back (handle_abort()).
        if (sbi->txn_hopen && !sbi->txn_hnoundo && i < sbi->txn_hbase && !sbi->txn_undo[i]) {
            uint8_t *copy = kmalloc(T3_BLOCK);
            if (!copy) return 0;
            k_memcpy(copy, sbi->txn_img[i], T3_BLOCK);
            sbi->txn_undo[i] = copy;
        }
        return sbi->txn_img[i];
    }
    // Past the reservation is a bug in the CALLER's credit count, not
    // a runtime condition -- but refusing here keeps it a failed
    // operation rather than a torn one.
    if (sbi->txn_count >= sbi->txn_credits || sbi->txn_count >= (int)sbi->jslots) return 0;
    if (!t3_read_block(sbi, blk, sbi->txn_img[sbi->txn_count])) return 0;
    sbi->txn_target[sbi->txn_count] = blk;
    return sbi->txn_img[sbi->txn_count++];
}

// The header's slot table has to move for v2: four v1 entries end at
// byte 44, which is exactly where v1 put the header checksum, so 32 of
// them would overwrite it. v2 starts the table at 16 and puts the
// checksum in the sector's last four bytes -- room for 60 slots before
// the two would meet again. The version decides which shape is
// written and read; nothing guesses.
#define T3_JH_V1_SLOTS_OFF  12u
#define T3_JH_V1_CKSUM_OFF  44u
#define T3_JH_V2_SLOTS_OFF  16u
#define T3_JH_V2_CKSUM_OFF  508u   // ON-DISK: never derive it from a sector size

static uint32_t jh_slots_off(uint32_t version) {
    return version >= 2 ? T3_JH_V2_SLOTS_OFF : T3_JH_V1_SLOTS_OFF;
}
uint32_t t3_jh_cksum_off(uint32_t version) {
    return version >= 2 ? T3_JH_V2_CKSUM_OFF : T3_JH_V1_CKSUM_OFF;
}

static int write_journal_header(struct t3_state *sbi, int commit) {
    uint8_t sec[T3_SECTOR];
    uint32_t slots = jh_slots_off(sbi->sb.version), ck = t3_jh_cksum_off(sbi->sb.version);
    k_memset(sec, 0, sizeof(sec));
    sec[0] = 'J'; sec[1] = 'R'; sec[2] = 'N'; sec[3] = '3';
    sec[4] = (uint8_t)commit;
    sec[5] = (uint8_t)sbi->txn_count;
    wr32(sec + 8, sbi->jrn_seq);
    for (int i = 0; i < sbi->txn_count; i++) {
        wr32(sec + slots + (uint32_t)i * 8, sbi->txn_target[i]);
        wr32(sec + slots + (uint32_t)i * 8 + 4, k_fnv1a(sbi->txn_img[i], T3_BLOCK));
    }
    wr32(sec + ck, k_fnv1a(sec, ck));
    return t3_vol_write_sectors(sbi, T3_JH_BLOCK * T3_SPB, 1, sec);
}

// A journal barrier, or a no-op under `storage.sync = lazy`.
//
// EVERY barrier of t3_txn_commit()'s goes through here, because none is
// the optional one: barrier 0 orders the bitmaps against the journal,
// barrier 1 orders the journal against the targets so a crash
// mid-target-write can be replayed, and barrier 2 orders the
// targets against clearing the commit flag so a crash cannot leave the
// journal saying "nothing to do" over work that never landed. Turning
// them off trades crash recoverability for throughput, which is ext4's
// `nobarrier` exactly -- see storage_config.c for what it costs and why
// the default is the other one.
//
// It still returns 1 when skipped. A caller must not read "no barrier
// was issued" as "the barrier failed": failure ABANDONS the
// transaction, and doing that on every write would be a filesystem that
// refuses to write at all.
static int txn_barrier(struct t3_state *sbi) {
    if (!storage_sync_strict()) return 1;
    uint32_t covered = __atomic_load_n(&sbi->dev_writes, __ATOMIC_ACQUIRE);
    if (!blkdev_flush(sbi->vol.dev)) return 0;
    sbi->fenced_writes = covered;
    return 1;
}

// A COMMIT HAS THREE OUTCOMES, AND THE THIRD IS THE POINT.
//
// Past the commit point the journal is durable and WILL be replayed,
// so the blocks the operation allocated are live even though the
// operation failed -- freeing them hands a replayed inode pointers to
// space something else can take. Before the commit point it is the
// exact opposite: nothing landed, and not freeing them leaks. The two
// used to be one `return 0`, and every caller rolled back.
//
// jbd2 draws the same line, and answers it the same way
// (jbd2_journal_abort() plus errors=remount-ro): a filesystem that
// cannot apply its own commit stops writing, because the next mount's
// replay is the only thing that can still finish the job.
enum t3_commit {
    T3_COMMIT_ABORTED   = 0,   // nothing durable -- the caller must roll back
    T3_COMMIT_OK        = 1,
    T3_COMMIT_UNAPPLIED = 2,   // committed; replay owns these blocks now
};

// THE JOURNAL IMAGES IN AS FEW COMMANDS AS THE DEVICE TAKES. The
// journal's data blocks are contiguous on disk and txn_img is
// contiguous in memory, so a commit that wrote them one 4 KiB command at
// a time was paying per-command latency up to 32 times for one run.
static int write_journal_images(struct t3_state *sbi) {
    uint32_t per = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
    if (per == 0) per = 1;
    for (int i = 0; i < sbi->txn_count; ) {
        uint32_t k = (uint32_t)(sbi->txn_count - i);
        if (k > per) k = per;
        if (!t3_vol_write_sectors(sbi, (sbi->jdata_block + (uint32_t)i) * T3_SPB,
                               (int)(k * T3_SPB), sbi->txn_img[i])) return 0;
        i += (int)k;
    }
    return 1;
}

// THE TARGETS ALL IN FLIGHT AT ONCE. They are scattered, independent,
// and fenced by the barriers either side, so their order among
// themselves never mattered -- which is exactly a batch
// (blkdev_submit_batch(); AHCI queues it with NCQ, others loop). In the
// mount rather than on the stack, for the frame budget.

static int write_targets(struct t3_state *sbi) {
    if (sbi->readonly) return 0;             // t3_vol_write_sectors()'s gate
    t3_rcache_drop(sbi);                         // ...and its cache rule
    for (int i = 0; i < sbi->txn_count; i++) {
        uint64_t lba = sbi->txn_target[i] * T3_SPB;
        if (lba + T3_SPB > sbi->vol.sector_count) return 0;
        sbi->txn_io[i] = (struct blk_io){ .lba = sbi->vol.base_lba + lba, .count = T3_SPB,
                                       .write = 1, .buf = sbi->txn_img[i] };
    }
    int ok = blkdev_submit_batch(sbi->vol.dev, sbi->txn_io, sbi->txn_count);
    t3_note_write(sbi);
    return ok;
}

static enum t3_commit txn_commit_raw(struct t3_state *sbi) {
    if (sbi->txn_count == 0) return T3_COMMIT_OK;
    // BARRIER 0, only when allocation state went out since the last one:
    // the bitmaps this transaction depends on must be on the media
    // before a header that replay will act on (t3_flush_alloc_state()).
    if (sbi->alloc_unfenced) {
        if (!txn_barrier(sbi)) return T3_COMMIT_ABORTED;
        sbi->alloc_unfenced = 0;
    }
    sbi->jrn_seq++;
    if (!write_journal_images(sbi)) return T3_COMMIT_ABORTED;
    if (!write_journal_header(sbi, 1)) return T3_COMMIT_ABORTED;

    // BARRIER 1, and it is checked. The journal's whole guarantee is
    // that everything written before this point is on the platter, so
    // if the flush cannot say that, the targets below must NOT be
    // overwritten: a half-written target with no durable journal behind
    // it is unrecoverable, while abandoning the transaction here costs
    // nothing that was not already lost. blkdev_flush(sbi->vol.dev) returned void until
    // a write-back cache went in underneath (ata_cache.h) -- that is
    // where a deferred write's failure now surfaces.
    if (!txn_barrier(sbi)) {
        klog_write(KLOG_ERR "tfs3: journal barrier failed -- transaction abandoned, "
                   "targets untouched\n");
        return T3_COMMIT_ABORTED;
    }
    sbi->alloc_unfenced = 0;   // that barrier covered anything since, too

    int ok = write_targets(sbi);

    // BARRIER 2: the targets must be durable before the commit flag is
    // cleared below, or a crash after clearing it loses the record of
    // work that never reached the disk. A failure here takes the same
    // path a failed target write does -- leave the header committed and
    // let replay finish the job next boot.
    if (!txn_barrier(sbi)) ok = 0;
    if (ok) {
        int saved = sbi->txn_count;
        sbi->txn_count = 0;
        uint32_t before = __atomic_load_n(&sbi->dev_writes, __ATOMIC_ACQUIRE);
        write_journal_header(sbi, 0); // no barrier -- see the discipline note above
        // ...nor does it unfence the volume: a pull that loses the cleared
        // header replays a committed one, idempotently. Only when it was
        // the one write since barrier 2 -- an overwrite in a lock gap is not.
        if (sbi->fenced_writes == before &&
            __atomic_load_n(&sbi->dev_writes, __ATOMIC_ACQUIRE) == before + 1)
            sbi->fenced_writes = before + 1;
        sbi->txn_count = saved;
    } else {
        // Leave the header committed: replay finishes the job next
        // boot, same call t3_replay_journal() in TFS2 makes.
        // Either a target write or barrier 2 -- both mean the same
        // thing here, and only the journal can finish it now.
        klog_write(KLOG_ERR "tfs3: transaction could not be applied -- left committed for replay\n");
    }
    return ok ? T3_COMMIT_OK : T3_COMMIT_UNAPPLIED;
}

// What every caller in this file actually wants: 1 on success, 0 on
// failure. The three-way answer is consumed HERE rather than at the
// sixteen call sites, which is what makes the rule impossible to
// forget. After an UNAPPLIED one a caller's own t3_alog_rollback() frees
// nothing, because there is nothing it may undo.
int t3_txn_commit(struct t3_state *sbi) {
    int was_deferred = sbi->txn_deferred;
    enum t3_commit r = T3_COMMIT_ABORTED;
    // Earlier ops' allocation state goes first, BEFORE the commit that
    // makes those blocks reachable -- a stopped write skips it, which is
    // the whole saving (do_write_inner()).
    if (!was_deferred || t3_flush_alloc_state(sbi)) {
        sbi->txn_deferred = 0;
        r = txn_commit_raw(sbi);
    }
    if (r == T3_COMMIT_ABORTED) {
        // Nothing landed. This handle's work comes off, so its caller can
        // roll back what it allocated; what earlier ops staged is KEPT
        // for the next commit -- dropping it lost other processes'
        // completed writes whenever an unrelated op failed its journal
        // write.
        handle_abort(sbi);
        sbi->txn_deferred = was_deferred && sbi->txn_count > 0;
        t3_icache_drop(sbi);   // staged and disk may now disagree
        return 0;
    }
    txn_forget_staged(sbi);
    sbi->txn_count = 0;
    sbi->txn_credits = 0;
    handle_close(sbi);
    if (r == T3_COMMIT_UNAPPLIED) {
        t3_icache_drop(sbi);
        t3_alog_cancel(sbi);
        t3_vol_go_readonly(sbi, "a committed transaction could not be applied");
    }
    return r == T3_COMMIT_OK;
}

// 1 if nothing is outstanding -- torn and discarded, or none to begin
// with -- and 2 when a transaction was REPLAYED, which the caller must
// treat as "any block may have changed, the superblock too" (tfs3_init()).
// 0 when a COMMITTED transaction is still on the
// disk unapplied, which the caller must not mount writable over: a
// write could reuse a block the journal names, and the replay is the
// only thing that can still finish it.
int t3_replay_journal(struct t3_state *sbi) {
    uint8_t sec[T3_SECTOR];
    if (!t3_vol_read_sectors(sbi, T3_JH_BLOCK * T3_SPB, 1, sec)) return 1;
    if (!(sec[0] == 'J' && sec[1] == 'R' && sec[2] == 'N' && sec[3] == '3')) return 1;
    uint32_t slots = jh_slots_off(sbi->sb.version), ck = t3_jh_cksum_off(sbi->sb.version);
    if (k_fnv1a(sec, ck) != rd32(sec + ck)) return 1; // torn header = no transaction
    sbi->jrn_seq = rd32(sec + 8);
    if (!sec[4]) return 1; // not committed
    uint32_t count = sec[5];
    if (count == 0 || count > sbi->jslots) count = 0;

    int all_ok = (count > 0);
    for (uint32_t i = 0; i < count && all_ok; i++) {
        if (!t3_read_block(sbi, sbi->jdata_block + i, sbi->txn_img[i])) all_ok = 0;
        else if (k_fnv1a(sbi->txn_img[i], T3_BLOCK) != rd32(sec + slots + i * 8 + 4)) all_ok = 0;
    }
    if (all_ok) {
        for (uint32_t i = 0; i < count && all_ok; i++) {
            if (!t3_write_block(sbi, rd32(sec + slots + i * 8), sbi->txn_img[i])) all_ok = 0;
        }
        if (all_ok && !blkdev_flush(sbi->vol.dev)) {
            // Same rule as t3_txn_commit()'s barrier 2: if the replayed
            // targets are not durable, do NOT clear the committed flag
            // below -- the next boot must replay them again.
            klog_write(KLOG_ERR "tfs3: journal replay barrier failed -- left committed "
                       "for next boot\n");
            return 0;
        }
        if (all_ok) {
            klog_write("tfs3: replayed a committed journal transaction (");
            klog_write_dec(count); klog_write(" blocks)\n");
        } else {
            klog_write(KLOG_ERR "tfs3: journal replay write failed -- left committed for next boot\n");
            return 0;
        }
    } else {
        klog_write("tfs3: discarded a torn journal transaction\n");
    }
    sbi->txn_count = 0;
    write_journal_header(sbi, 0);
    return all_ok ? 2 : 1;
}

// ---- inode staging (through the transaction) ------------------------------

static void pack_inode_into(uint8_t *p, const struct t3_inode *node) {
    k_memset(p, 0, T3_INODE_SIZE);
    p[0] = node->type;
    wr16(p + 2, node->links);
    wr64(p + 4, node->size);
    wr64(p + 12, node->created);
    wr64(p + 20, node->modified);
    for (int i = 0; i < 15; i++) wr32(p + 28 + i * 4, node->ptrs[i]);
    wr16(p + 92, node->mode);   // see struct t3_inode -- inside the checksum
    uint8_t chk[124];
    k_memcpy(chk, p, 88);
    k_memcpy(chk + 88, p + 92, 36);
    wr32(p + 88, k_fnv1a(chk, sizeof(chk)));
}

// Patch inode `ino`'s 128 bytes inside its (journal-staged) table
// block. `node == 0` zeroes the slot -- a dead inode fails its
// checksum by design, the bitmap is the allocation authority.
int t3_txn_stage_inode(struct t3_state *sbi, uint64_t ino, const struct t3_inode *node) {
    sbi->ino_gen++;   // see t3_state.ino_gen
    t3_icache_forget(sbi, ino);
    // Only a directory can be cached again before this ends (a freed
    // inode's type is unknown here, so it counts too). Recording every
    // file a deferred transaction stages would overflow the list and
    // empty the cache at each deferred commit.
    if (!node || node->type == T3_TYPE_DIR) {
        if (sbi->txn_nino < T3_TXN_INO) sbi->txn_ino[sbi->txn_nino] = ino;
        if (sbi->txn_nino <= T3_TXN_INO) sbi->txn_nino++;   // one past: "too many, drop all"
    }
    uint64_t lba;
    uint32_t off;
    if (!t3_inode_pos(sbi, ino, &lba, &off)) return 0;
    uint32_t blk = (uint32_t)(lba / T3_SPB);
    uint32_t within = (uint32_t)(lba % T3_SPB) * T3_SECTOR + off;
    uint8_t *img = t3_txn_stage(sbi, blk);
    if (!img) return 0;
    if (node) pack_inode_into(img + within, node);
    else k_memset(img + within, 0, T3_INODE_SIZE);
    return 1;
}

uint64_t t3_now_epoch(void) {
    struct rtc_time t;
    ktime_read(&t);
    return cal_rtc_to_epoch(&t);
}
