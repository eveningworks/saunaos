// Grapheme cluster boundaries, UAX #29 -- see api/kgrapheme.h.
//
// COMPILED TWICE, kernel and libbase (the Makefile's shared list), like
// klineedit.c: it names nothing kernel-only and allocates nothing.
//
// The forward walk carries the only state the rules need: the run of
// regional indicators (GB12/13 pair them) and whether "pictograph,
// extenders, ZWJ" has just been seen (GB11 joins the next pictograph). The
// backward walk does not re-derive that state in reverse -- it backs up to
// a code point no rule can join to what precedes it, then walks FORWARD,
// so the two directions cannot disagree.
#include "kgrapheme.h"
#include "kgrapheme_table.h"
#include "kutf8.h"

enum kgrapheme_class kgrapheme_class(uint32_t cp) {
    if (cp >= 0xAC00 && cp <= 0xD7A3)               // Hangul syllables: LV every 28th
        return (cp - 0xAC00) % 28 ? KGB_LVT : KGB_LV;
    int lo = 0, hi = (int)(sizeof kgrapheme_ranges / sizeof kgrapheme_ranges[0]) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < kgrapheme_ranges[mid].lo) hi = mid - 1;
        else if (cp > kgrapheme_ranges[mid].hi) lo = mid + 1;
        else return (enum kgrapheme_class)kgrapheme_ranges[mid].cls;
    }
    return KGB_OTHER;
}

static uint32_t at(const char *s, int n, int i, int *len) {
    if (n >= 0 && i >= n) { *len = 0; return 0; }
    return kutf8_decode(s + i, n < 0 ? -1 : n - i, len);
}

static int is_break_class(enum kgrapheme_class k) {
    return k == KGB_CR || k == KGB_LF || k == KGB_CONTROL;
}

struct walk { enum kgrapheme_class prev; int ri; int ep_zwj; int ep_ext; };

// Whether the cluster in `w` takes a code point of class `k` (GB3-GB13).
//
// GB3 (CR x LF) is NOT applied: every caller treats a line break as a
// character of its own, and "\r\n" stays two, as it always was here.
static int joins(const struct walk *w, enum kgrapheme_class k) {
    enum kgrapheme_class p = w->prev;
    if (is_break_class(p) || is_break_class(k)) return 0;            // GB4, GB5
    if (p == KGB_L && (k == KGB_L || k == KGB_V || k == KGB_LV || k == KGB_LVT)) return 1;   // GB6
    if ((p == KGB_LV || p == KGB_V) && (k == KGB_V || k == KGB_T)) return 1;                 // GB7
    if ((p == KGB_LVT || p == KGB_T) && k == KGB_T) return 1;                                // GB8
    if (k == KGB_EXTEND || k == KGB_ZWJ || k == KGB_SPACINGMARK) return 1;   // GB9, GB9a
    if (p == KGB_PREPEND) return 1;                                            // GB9b
    if (p == KGB_ZWJ && k == KGB_EP && w->ep_zwj) return 1;                   // GB11
    if (p == KGB_RI && k == KGB_RI && (w->ri & 1)) return 1;                  // GB12, GB13
    return 0;
}

static void take(struct walk *w, enum kgrapheme_class k) {
    w->ep_zwj = (k == KGB_ZWJ && w->ep_ext);
    if (k == KGB_EP) w->ep_ext = 1;
    else if (k != KGB_EXTEND) w->ep_ext = 0;
    w->ri = (k == KGB_RI) ? w->ri + 1 : 0;
    w->prev = k;
}

int kgrapheme_next(const char *s, int n, int i) {
    if (!s || i < 0) return 0;
    int len;
    uint32_t c = at(s, n, i, &len);
    if (len == 0) return i;
    struct walk w = { KGB_OTHER, 0, 0, 0 };
    take(&w, kgrapheme_class(c));
    int j = i + len;
    for (;;) {
        c = at(s, n, j, &len);
        if (len == 0) break;
        enum kgrapheme_class k = kgrapheme_class(c);
        if (!joins(&w, k)) break;
        take(&w, k);
        j += len;
    }
    return j;
}

int kgrapheme_prev(const char *s, int i) {
    if (!s || i <= 0) return 0;
    // Back to a SAFE START: a code point that begins a cluster whatever
    // precedes it, judged by the code point before it alone -- class Other
    // or a line break (not after a Prepend, GB9b); a pictograph not after a
    // ZWJ (GB11); a Hangul L, LV or LVT not after an L (GB6). What is left
    // (an extender, a regional indicator whose pairing depends on the whole
    // run, a V or T jamo) might belong to the cluster before it. Judging by
    // one neighbour keeps a walk back over a run of emoji or Korean linear.
    int q = kutf8_prev(s, i);
    while (q > 0) {
        int len, p = kutf8_prev(s, q);
        uint32_t before = at(s, i, p, &len);
        enum kgrapheme_class k = kgrapheme_class(at(s, i, q, &len)), pk = kgrapheme_class(before);
        int safe = pk != KGB_PREPEND &&
                   ((k == KGB_OTHER || is_break_class(k)) ||
                    (k == KGB_EP && before != 0x200D) ||
                    ((k == KGB_L || k == KGB_LV || k == KGB_LVT) && pk != KGB_L));
        if (safe) break;
        q = p;
    }
    // Then forward: the last boundary before `i`.
    int start = q;
    for (int j = q; j < i;) {
        int nx = kgrapheme_next(s, i, j);
        if (nx <= j) break;
        start = j;
        j = nx;
    }
    return start;
}

int kgrapheme_is_emoji(const char *s, int i, int end) {
    if (!s || i < 0 || end <= i) return 0;
    for (int j = i; j < end;) {
        int len;
        uint32_t c = at(s, end, j, &len);
        if (len == 0) break;
        if (j == i && kgrapheme_class(c) == KGB_EP) return 1;
        if (c == 0x200D || c == 0xFE0F || c == 0x20E3 || (c >= 0x1F1E6 && c <= 0x1F1FF) ||
            (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F))
            return 1;
        j += len;
    }
    return 0;
}

int kgrapheme_backspace(const char *s, int i) {
    if (!s || i <= 0) return 0;
    int start = kgrapheme_prev(s, i);
    return kgrapheme_is_emoji(s, start, i) ? start : kutf8_prev(s, i);
}
