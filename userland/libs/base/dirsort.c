// See dirsort.h for why this is shared, and why it moves entries rather
// than handing back a permutation.
#include "base/dirsort.h"
#include <stdlib.h>
#include <string.h>

int dirsort_cmp(const struct ulist_entry *a, const struct ulist_entry *b,
                 enum dirsort_key key) {
    int r = 0;
    if (key == DIRSORT_SIZE) {
        r = (a->size < b->size) - (a->size > b->size);
    } else if (key == DIRSORT_TIME) {
        // Field by field rather than through an epoch conversion:
        // struct rtc_time is what the ABI hands over, and tz.h's
        // converter is kernel-side.
        const struct rtc_time *x = &a->modified, *y = &b->modified;
        const uint32_t ax[6] = { x->year, x->month, x->day, x->hour, x->minute, x->second };
        const uint32_t by[6] = { y->year, y->month, y->day, y->hour, y->minute, y->second };
        for (int i = 0; i < 6 && r == 0; i++)
            r = (ax[i] < by[i]) - (ax[i] > by[i]);
    }
    // Name is the default AND the tie-break, so two files written in the
    // same second still land in a stable, reproducible order.
    if (r == 0) r = strcmp(a->name, b->name);
    return r;
}

static int cmp(const struct ulist_entry *a, const struct ulist_entry *b,
               enum dirsort_key key, int reverse) {
    int r = dirsort_cmp(a, b, key);
    return reverse ? -r : r;
}

// The fallback when there is no memory for the merge: in place, one
// scratch entry, quadratic.
static void insertion(struct ulist_entry *e, int n, enum dirsort_key key, int reverse) {
    for (int i = 1; i < n; i++) {
        struct ulist_entry v = e[i];
        int j = i - 1;
        while (j >= 0 && cmp(&e[j], &v, key, reverse) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = v;
    }
}

void dirsort(struct ulist_entry *e, int n, enum dirsort_key key, int reverse) {
    // A MERGE SORT OF POINTERS, then one gather: a listing has no bound
    // (ulistdir.h) and an entry is ~280 bytes, so moving entries per
    // comparison -- the insertion sort this was -- is quadratic in
    // bytes. Bottom-up, so no recursion on a 2 KiB ring-3 frame.
    if (n < 2) return;
    const struct ulist_entry **a = malloc(sizeof *a * (size_t)n * 2);
    struct ulist_entry *out = a ? malloc(sizeof *out * (size_t)n) : 0;
    if (!out) { free(a); insertion(e, n, key, reverse); return; }
    const struct ulist_entry **b = a + n;
    for (int i = 0; i < n; i++) a[i] = &e[i];
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) b[k++] = cmp(a[j], a[i], key, reverse) < 0 ? a[j++] : a[i++];
            while (i < mid) b[k++] = a[i++];
            while (j < hi) b[k++] = a[j++];
        }
        const struct ulist_entry **t = a; a = b; b = t;
    }
    for (int i = 0; i < n; i++) out[i] = *a[i];
    memcpy(e, out, sizeof *out * (size_t)n);
    free(out);
    free(a < b ? a : b);   // the one block both halves live in
}
