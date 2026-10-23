#ifndef ULIB_UGROW_H
#define ULIB_UGROW_H

// A GROWABLE ARRAY in one macro: UGROW(arr, cap, need) makes the typed
// pointer `arr` hold at least `need` elements, doubling `cap`. The shape
// is git's ALLOC_GROW -- the caller keeps its own pointer, count and
// capacity, so a list stays a plain C array everywhere else.
//
//     if (!UGROW(g_hits, g_hits_cap, g_nhits + 1)) { log; stop; }
//     g_hits[g_nhits++] = hit;
//
// 1 on success; 0 when memory is out, and then `arr` and `cap` are BOTH
// UNCHANGED -- the old array is still whole, so a caller keeps what it
// had and says so (docs/conventions/kernel.md, "Size it to the data").
// Parallel arrays each take their own `cap`, so one failing leaves the
// others consistent with theirs.
//
// A MACRO, NOT A FUNCTION TAKING `void **`: storing a new address
// through a `void **` that really points at a `struct x *` is undefined
// under strict aliasing, and userland builds at -O2 with it on. The
// assignment here is to `arr` itself, in its own type. The array MAY
// MOVE: re-aim anything that kept a pointer into it.
//
// New elements are NOT zeroed (realloc's contract).

#include <stddef.h>
#include <stdlib.h>

#define UGROW_FIRST 16   // the first allocation, in elements

// The bytes for `need` elements of `elem`, doubling from `*cap`; the new
// block, or NULL (and `*cap` untouched) when memory or `int` runs out.
static inline void *ugrow_realloc(void *p, int *cap, int need, size_t elem) {
    int c = *cap > 0 ? *cap : UGROW_FIRST;
    while (c < need) {
        if (c > (int)(0x7fffffff / 2)) return NULL;
        c *= 2;
    }
    if (elem && (size_t)c > (size_t)-1 / elem) return NULL;
    void *n = realloc(p, (size_t)c * elem);
    if (n) *cap = c;
    return n;
}

#define UGROW(arr, cap, need) __extension__ ({                               \
    int ugrow_ok_ = 1;                                                       \
    if ((need) > (cap)) {                                                    \
        int ugrow_c_ = (cap);                                                \
        void *ugrow_p_ = ugrow_realloc((arr), &ugrow_c_, (need), sizeof *(arr)); \
        if (ugrow_p_) { (arr) = ugrow_p_; (cap) = ugrow_c_; }                \
        else ugrow_ok_ = 0;                                                  \
    }                                                                        \
    ugrow_ok_;                                                               \
})

#endif
