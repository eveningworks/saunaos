// jit16.c -- the JIT's block cache: the code arena, the table from a block start to its
// translation, the dispatcher that runs blocks, and invalidation when a store hits translated
// code (jit16.h). The x86 is jit16_x86.c's.
//
// THE ARENA IS FLUSHED WHOLE WHEN FULL, QEMU's tb_flush: a block's host code is never freed on
// its own, so an invalidated block's bytes stay until the next flush (docs/w16-jit-design.md,
// "Stage 1"). The flush and invalidation counters say whether a program churns enough for
// per-page reclaim to be worth having.
//
// A LINE OF THE ARENA IS 64 BYTES (CPU16_JIT_LINE), and c->jit_lines has one byte per line: set
// where some live block's guest code lies. A store into a set line, from the interpreter, from
// translated code (which leaves the block first and lets the interpreter do the store) or from
// the C side, drops the blocks of that PAGE that overlap it. A page whose blocks keep being
// dropped -- code and the data it writes sharing a page -- is given up and left to the
// interpreter, DOSBox's answer to the same thrash.
#include "jit16_internal.h"
#include "lin16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define JIT_ARENA      (32u << 20)   // host code; full means flush
#define JIT_THRESHOLD  50            // arrivals before a block start is translated (LuaJIT's 56)
#define JIT_PAGE_GIVE_UP 32          // invalidations before a page is left to the interpreter

// One block START -- with the DS and SS it is reached under, since a block's accesses are compiled
// for them: code called with two data segments is two entries, not one translated over and over.
struct jkey {
    const uint8_t *host;             // NULL: an empty slot
    uint32_t ip;
    uint16_t cs, ds, ss;
};

struct jent {
    struct jkey k;
    uint8_t refused;                 // its first instruction cannot be translated
    uint32_t count;
    struct jblock *b;
};

struct jit16 {
    struct jit16_stats st;
    unsigned threshold;
    uint8_t *code;                   // the arena, JIT_ARENA bytes
    uint32_t top;
    struct jent *tab;
    uint32_t tab_mask, tab_used;
    struct jblock *all;              // every block since the last flush
    struct jblock **page_blocks;     // per arena page (lin_size >> 12), the live blocks there
    uint8_t *page_drops;             // ...and how often its blocks were dropped
    uint32_t npages;
    uint8_t *buf;                    // the translator's output, before it is copied in
    int off;                         // turned off: a page of code could not be made executable again
};

static struct cpu16 *g_lin_cpu;      // lin16's free tells the JIT through this (one CPU per process)

static uint32_t hash_ptr(const uint8_t *p) {
    uint64_t x = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ULL;
    return (uint32_t)(x >> 32);
}

static int tab_alloc(struct jit16 *j, uint32_t size) {
    j->tab = calloc(size, sizeof *j->tab);
    if (!j->tab) return -1;
    j->tab_mask = size - 1;
    j->tab_used = 0;
    return 0;
}

static int same_key(const struct jkey *a, const struct jkey *b) {
    return a->host == b->host && a->ip == b->ip && a->cs == b->cs && a->ds == b->ds && a->ss == b->ss;
}

// The entry for `k`, made when `make`. NULL when the table cannot grow.
static struct jent *tab_find(struct jit16 *j, const struct jkey *k, int make) {
    for (uint32_t i = hash_ptr(k->host) & j->tab_mask;; i = (i + 1) & j->tab_mask) {
        struct jent *e = &j->tab[i];
        if (!e->k.host) {
            if (!make) return 0;
            if ((j->tab_used + 1) * 2 > j->tab_mask + 1) {
                // Half full: twice the size, the entries rehashed. Sized to the program, not capped.
                struct jent *old = j->tab;
                uint32_t n = j->tab_mask + 1;
                if (tab_alloc(j, n * 2) < 0) { j->tab = old; return 0; }
                for (uint32_t m = 0; m < n; m++)
                    if (old[m].k.host) *tab_find(j, &old[m].k, 1) = old[m];
                free(old);
                return tab_find(j, k, 1);
            }
            e->k = *k;
            j->tab_used++;
            return e;
        }
        if (same_key(&e->k, k)) return e;
    }
}

static struct jkey key_of(const struct jblock *b) {
    return (struct jkey){ b->host, b->ip, b->cs, b->ds, b->ss };
}

static void lines_mark(struct cpu16 *c, const struct jblock *b) {
    if (b->rel == JIT_NO_REL) return;
    for (uint32_t l = b->rel >> CPU16_JIT_LINE; l <= (b->rel + b->len - 1) >> CPU16_JIT_LINE; l++)
        c->jit_lines[l] = 1;
}

void jit16_flush(struct cpu16 *c) {
    struct jit16 *j = c->jit;
    if (!j) return;
    for (struct jblock *b = j->all, *n; b; b = n) {
        n = b->all_next;
        if (b->rel != JIT_NO_REL) {
            for (uint32_t l = b->rel >> CPU16_JIT_LINE; l <= (b->rel + b->len - 1) >> CPU16_JIT_LINE; l++)
                c->jit_lines[l] = 0;
            j->page_blocks[b->rel >> 12] = 0;
        }
        free(b);
    }
    j->all = 0;
    memset(j->tab, 0, (size_t)(j->tab_mask + 1) * sizeof *j->tab);
    j->tab_used = 0;
    j->top = 0;
    j->st.flushes++;
}

// A page's code lines again, from the live blocks it still has.
static void lines_rebuild(struct cpu16 *c, struct jit16 *j, uint32_t pg) {
    memset(c->jit_lines + ((size_t)pg << (12 - CPU16_JIT_LINE)), 0, 1u << (12 - CPU16_JIT_LINE));
    for (struct jblock *b = j->page_blocks[pg]; b; b = b->page_next) lines_mark(c, b);
}

// THE ONE WAY A BLOCK GOES before a flush: out of the table, out of its page's list, and its
// lines unmarked unless another block still covers them -- a mark left behind would send every
// store there to the interpreter for good. Its bytes stay in the arena until the flush.
static void block_drop(struct cpu16 *c, struct jit16 *j, struct jblock *b) {
    b->live = 0;
    struct jkey k = key_of(b);
    struct jent *e = tab_find(j, &k, 0);
    if (e && e->b == b) { e->b = 0; e->count = 0; }
    if (b->rel == JIT_NO_REL) return;
    uint32_t pg = b->rel >> 12;
    struct jblock **pp = &j->page_blocks[pg];
    while (*pp && *pp != b) pp = &(*pp)->page_next;
    if (*pp) *pp = b->page_next;
    lines_rebuild(c, j, pg);
}

void jit16_store(struct cpu16 *c, uint32_t rel, uint32_t len) {
    struct jit16 *j = c->jit;
    if (!j || !len) return;
    uint32_t end = rel + len;        // the arena is far below 4 GiB: no wrap
    for (uint32_t pg = rel >> 12; pg <= (end - 1) >> 12 && pg < j->npages; pg++) {
        int dropped = 0;
        for (struct jblock *b = j->page_blocks[pg], *n; b; b = n) {
            n = b->page_next;
            if (b->rel < end && rel < b->rel + b->len) {
                block_drop(c, j, b);
                j->st.invalidated++;
                dropped = 1;
            }
        }
        if (dropped && j->page_drops[pg] < 255) j->page_drops[pg]++;
    }
}

void cpu16_jit_store(struct cpu16 *c, const uint8_t *p, uint32_t len) {
    size_t rel = (size_t)(p - c->lin);
    if (!c->jit || rel >= c->lin_size) return;
    if (len > c->lin_size - rel) len = (uint32_t)(c->lin_size - rel);
    // Only when a line in the range is marked: the C side's big writes (a file read) come here
    // on every call, and most land on data.
    for (size_t l = rel >> CPU16_JIT_LINE; l <= (rel + len - 1) >> CPU16_JIT_LINE; l++)
        if (c->jit_lines[l]) { jit16_store(c, (uint32_t)rel, len); return; }
}

static void lin_released(void *p, uint32_t size) {
    if (g_lin_cpu && size) cpu16_jit_store(g_lin_cpu, p, size);
}

// Its descriptors as the block was made for them? A descriptor changed in place bumps desc_gen
// for every one, so the snapshot is compared then, not trusted.
static int same_desc(const struct cpu16_desc *a, const struct cpu16_desc *b) {
    return a->base == b->base && a->limit == b->limit && a->lin == b->lin && a->kind == b->kind &&
           a->flags == b->flags;
}

static int guard_ok(struct cpu16 *c, struct jblock *b) {
    if (c->sreg[S_CS] != b->cs || c->sreg[S_DS] != b->ds || c->sreg[S_SS] != b->ss) return 0;
    if (c->desc_gen == b->gen) return 1;
    const struct cpu16_desc *cs = cpu16_seg(c, S_CS), *ds = cpu16_seg(c, S_DS), *ss = cpu16_seg(c, S_SS);
    if (!cs || !ds || !ss || !same_desc(cs, &b->cs_d) || !same_desc(ds, &b->ds_d) || !same_desc(ss, &b->ss_d))
        return 0;
    b->gen = c->desc_gen;
    return 1;
}

// Translates the block at CS:EIP into the arena. NULL when it cannot be.
static struct jblock *translate(struct cpu16 *c, struct jit16 *j, const uint8_t *host, uint32_t avail) {
    struct jblock *b = calloc(1, sizeof *b);
    if (!b) return 0;
    size_t rel = (size_t)(host - c->lin);
    b->rel = c->lin && rel < c->lin_size ? (uint32_t)rel : JIT_NO_REL;
    if (b->rel != JIT_NO_REL) {
        // One page per block, so a page's list is every block a store there can hit.
        uint32_t room = 4096 - (b->rel & 4095);
        if (avail > room) avail = room;
        if (j->page_drops[b->rel >> 12] >= JIT_PAGE_GIVE_UP) { free(b); return 0; }
    } else if (avail > 4096) {
        avail = 4096;
    }
    int stop_op = -1, stop_0f = -1;
    uint32_t n = jit16_translate(c, b, host, avail, j->buf, JIT_CODE_MAX, &stop_op, &stop_0f);
    b->stop = (int16_t)(stop_op < 0 ? -1 : stop_op == 0x0F && stop_0f >= 0 ? 256 + stop_0f : stop_op);
    if (stop_op >= 0) {
        if (stop_op == 0x0F && stop_0f >= 0) j->st.stop_0f[stop_0f]++;
        else j->st.stop_op[stop_op]++;
    }
    if (!n) { free(b); return 0; }

    // W^X: the pages the copy lands on are writable only for the copy.
    uintptr_t lo = (uintptr_t)(j->code + j->top) & ~(uintptr_t)4095;
    uintptr_t hi = ((uintptr_t)(j->code + j->top + n) + 4095) & ~(uintptr_t)4095;
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE) != 0) { free(b); return 0; }
    memcpy(j->code + j->top, j->buf, n);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_EXEC) != 0) {
        // The page is writable and NOT executable now, and earlier blocks live on it: every
        // block goes, and the program runs on interpreted.
        fprintf(stderr, "w16: the JIT could not make its code executable again -- interpreted from here\n");
        free(b);
        jit16_flush(c);
        j->off = 1;
        return 0;
    }
    b->fn = (uint32_t (*)(struct cpu16 *))(void *)(j->code + j->top + b->entry);
    j->top = (j->top + n + 15) & ~15u;

    b->host = host;
    b->ip = c->ip;           // cs, ds and ss: jit16_translate()'s
    b->gen = c->desc_gen;
    b->live = 1;
    b->all_next = j->all;
    j->all = b;
    if (b->rel != JIT_NO_REL) {
        b->page_next = j->page_blocks[b->rel >> 12];
        j->page_blocks[b->rel >> 12] = b;
        lines_mark(c, b);
    }
    j->st.blocks++;
    j->st.insns += b->ninsn;
    j->st.code_bytes += n;
    return b;
}

// THE CALL-OUT RUNS THE INTERPRETER FROM INSIDE A BLOCK, so the block may not survive it: an
// instruction that loads a segment register changes what its accesses were compiled for, and a
// store into translated code may have dropped the block itself. Only instructions that cannot
// reach the program's own code again are called out (jit16_x86.c's callable()): a nested run could
// flush the arena under the block that is waiting here.
uint32_t jit16_callout(struct cpu16 *c, uint32_t next, uint32_t op) {
    struct jit16 *j = c->jit;
    uint16_t cs = c->sreg[S_CS], ds = c->sreg[S_DS], ss = c->sreg[S_SS];
    uint32_t gen = c->desc_gen;
    uint64_t dropped = j->st.invalidated + j->st.flushes;
    j->st.callout[op & 511]++;
    cpu16_jit_step(c);
    if (c->fault || c->exited) { j->st.callout_exit++; return 2; }
    if (c->ip != next || c->sreg[S_CS] != cs || c->sreg[S_DS] != ds || c->sreg[S_SS] != ss ||
        c->desc_gen != gen || j->st.invalidated + j->st.flushes != dropped || (c->flags & F_TF)) {
        j->st.callout_exit++;
        return 1;
    }
    return 0;
}

int jit16_run(struct cpu16 *c, uint64_t until) {
    struct jit16 *j = c->jit;
    int ran = 0;
    if (j->off || !c->cs32 || c->fault || c->exited) return 0;
    // BACK TO THE RUN LOOP AT EVERY TICK (CPU16_TICK_STEPS instructions), however many blocks are
    // left to run: the tick is what plays a wave buffer to its end and marks it done, and a game
    // polling for that in a loop that calls nothing -- Lighthouse after New Game, clicked early, or
    // after Esc -- waited for ever inside translated code that never let it come.
    // ONE COMPARE A BLOCK: a fault, an exit or a CS of another size cannot arise between blocks
    // without the block that caused it leaving early, which ends this loop already.
    uint64_t stop = (c->steps | (CPU16_TICK_STEPS - 1)) + 1;
    if (until && until < stop) stop = until;
    while (c->steps < stop) {
        uint32_t avail;
        const uint8_t *host = cpu16_code_at(c, c->ip, &avail);
        if (!host) break;
        struct jkey k = { host, c->ip, c->sreg[S_CS], c->sreg[S_DS], c->sreg[S_SS] };
        struct jent *e = tab_find(j, &k, 1);
        if (!e) break;
        struct jblock *b = e->b;
        if (!b) {
            if (e->refused || ++e->count < j->threshold) break;
            if (j->top + JIT_CODE_MAX > JIT_ARENA) {
                jit16_flush(c);              // the entry went with the table
                e = tab_find(j, &k, 1);
                if (!e) break;
            }
            b = translate(c, j, host, avail);
            if (j->off) break;               // the flush took `e` too
            if (!b) { e->refused = 1; j->st.refused++; break; }
            e->b = b;
        }
        if (!guard_ok(c, b)) {
            // A descriptor changed under it: translated again once the start is hot again.
            j->st.guard_miss++;
            block_drop(c, j, b);
            break;
        }
        uint32_t n = b->fn(c);
        c->steps += n;
        j->st.runs++;
        j->st.run_insns += n;
        ran = 1;
        if (n < b->ninsn) {
            // Left before an instruction it could not do: the interpreter does it, then looks again.
            j->st.exit_slow++;
            c->jit_resume = 1;
            break;
        }
        j->st.exit_end++;
        if (b->stop >= 0) j->st.run_stop[b->stop]++;
    }
    // The interpreter takes the next instruction; whatever comes after it may start a block.
    if (ran) c->jit_resume = 1;
    return ran;
}

int jit16_init(struct cpu16 *c, unsigned threshold) {
    struct jit16 *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->threshold = threshold ? threshold : JIT_THRESHOLD;
    void *code = mmap(0, JIT_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    j->buf = malloc(JIT_CODE_MAX);
    if (code == MAP_FAILED || !j->buf || tab_alloc(j, 4096) < 0) goto fail;
    j->code = code;
    if (c->lin) {
        j->npages = c->lin_size >> 12;
        j->page_blocks = calloc(j->npages, sizeof *j->page_blocks);
        j->page_drops = calloc(j->npages, 1);
        // One byte per 64 of the arena, demand-paged like the arena itself.
        void *lines = mmap(0, c->lin_size >> CPU16_JIT_LINE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (!j->page_blocks || !j->page_drops || lines == MAP_FAILED) {
            if (lines != MAP_FAILED) munmap(lines, c->lin_size >> CPU16_JIT_LINE);
            goto fail;
        }
        c->jit_lines = lines;
        g_lin_cpu = c;
        lin16_on_release = lin_released;
    }
    c->jit = j;
    return 0;
fail:
    if (code != MAP_FAILED) munmap(code, JIT_ARENA);
    free(j->buf);
    free(j->tab);
    free(j->page_blocks);
    free(j->page_drops);
    free(j);
    return -1;
}

void jit16_free(struct cpu16 *c) {
    struct jit16 *j = c->jit;
    if (!j) return;
    jit16_flush(c);
    if (c->jit_lines) munmap(c->jit_lines, c->lin_size >> CPU16_JIT_LINE);
    if (g_lin_cpu == c) { g_lin_cpu = 0; lin16_on_release = 0; }
    munmap(j->code, JIT_ARENA);
    free(j->buf);
    free(j->tab);
    free(j->page_blocks);
    free(j->page_drops);
    free(j);
    c->jit = 0;
    c->jit_lines = 0;
}

const struct jit16_stats *jit16_stats(const struct cpu16 *c) { return c->jit ? &c->jit->st : 0; }
