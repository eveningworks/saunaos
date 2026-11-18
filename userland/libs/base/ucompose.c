// Canonical composition of a pair, by binary search -- base/ucompose.h.
#include "base/ucompose.h"
#include "base/ucompose_table.h"

uint32_t ucompose_pair(uint32_t first, uint32_t second) {
    int lo = 0, hi = (int)(sizeof ucompose_pairs / sizeof ucompose_pairs[0]) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t a = ucompose_pairs[mid].first, b = ucompose_pairs[mid].second;
        if (first < a || (first == a && second < b)) hi = mid - 1;
        else if (first > a || second > b) lo = mid + 1;
        else return ucompose_pairs[mid].composed;
    }
    return 0;
}

int ucompose_cluster(uint32_t *cps, int n) {
    if (!cps || n < 2) return n < 0 ? 0 : n;
    int k = 1;
    while (k < n) {
        uint32_t c = ucompose_pair(cps[0], cps[k]);
        if (!c) break;
        cps[0] = c;
        k++;
    }
    if (k == 1) return n;
    int out = 1;
    for (int i = k; i < n; i++) cps[out++] = cps[i];
    return out;
}
