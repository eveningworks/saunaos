// GRAPHEME CLUSTERS AS ONE TOKEN (ugfx.h, UGFX_CP_CLUSTER): a family emoji,
// a flag or a letter with an uncomposable mark interned once per process,
// so every loop that steps text one value at a time -- measure, draw, fit,
// hit-test, a terminal's cells -- steps it one CLUSTER at a time unchanged.
//
// THE TABLE ONLY GROWS: a token handed out stays valid for the life of the
// process, because a grid (uvterm's cells) keeps tokens, not text. Its size
// is the number of DISTINCT clusters the process has drawn -- a few dozen
// in practice, each a few code points; a hostile stream of unique clusters
// costs memory in proportion to its own length, which it already took to
// arrive.
//
// **A TOKEN'S VALUE IS PAST U+10FFFF** (UGFX_CLUSTER_FIRST + its index), so a
// caller that masks UGFX_CP_VALUE and compares with '\n' or a letter can
// never mistake a cluster for a character. Past the last such value, a
// cluster is drawn as its base character.
#include "ui/ugfx.h"
#include "base/ucompose.h"
#include "base/uemoji.h"
#include "base/ulog.h"
#include <stdlib.h>
#include <string.h>

struct cluster {
    uint32_t *cps;
    int n;
    unsigned hash;
};

static struct cluster *g_list;       // index = token value
static int g_count, g_cap;
static int *g_slots;                 // open addressing: index + 1, 0 empty
static int g_nslots;

static unsigned hash_cps(const uint32_t *cps, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ cps[i]) * 16777619u;
    return h ? h : 1;
}

static int rehash(int nslots) {
    int *s = calloc((size_t)nslots, sizeof *s);
    if (!s) return 0;
    for (int i = 0; i < g_count; i++) {
        unsigned k = g_list[i].hash & (unsigned)(nslots - 1);
        while (s[k]) k = (k + 1) & (unsigned)(nslots - 1);
        s[k] = i + 1;
    }
    free(g_slots);
    g_slots = s;
    g_nslots = nslots;
    return 1;
}

static uint32_t intern(const uint32_t *cps, int n) {
    unsigned h = hash_cps(cps, n);
    if (g_nslots) {
        for (unsigned k = h & (unsigned)(g_nslots - 1); g_slots[k]; k = (k + 1) & (unsigned)(g_nslots - 1)) {
            struct cluster *c = &g_list[g_slots[k] - 1];
            if (c->hash == h && c->n == n && !memcmp(c->cps, cps, (size_t)n * sizeof *cps))
                return UGFX_CP_CLUSTER | (UGFX_CLUSTER_FIRST + (uint32_t)(g_slots[k] - 1));
        }
    }
    if ((uint32_t)g_count > UGFX_CP_VALUE - UGFX_CLUSTER_FIRST) {
        static int said;
        if (!said++) ulogf("ugfx: %d distinct clusters; drawing further ones as their base\n", g_count);
        return cps[0];
    }
    if (g_count == g_cap) {
        int cap = g_cap ? g_cap * 2 : 16;
        struct cluster *l = realloc(g_list, (size_t)cap * sizeof *l);
        if (!l) return cps[0];
        g_list = l;
        g_cap = cap;
    }
    if ((g_count + 1) * 2 > g_nslots && !rehash(g_nslots ? g_nslots * 2 : 64)) return cps[0];
    uint32_t *copy = malloc((size_t)n * sizeof *copy);
    if (!copy) return cps[0];
    memcpy(copy, cps, (size_t)n * sizeof *copy);
    g_list[g_count] = (struct cluster){ copy, n, h };
    unsigned k = h & (unsigned)(g_nslots - 1);
    while (g_slots[k]) k = (k + 1) & (unsigned)(g_nslots - 1);
    g_slots[k] = g_count + 1;
    return UGFX_CP_CLUSTER | (UGFX_CLUSTER_FIRST + (uint32_t)g_count++);
}

uint32_t ugfx_cluster_token(uint32_t *cps, int n) {
    if (!cps || n <= 0) return 0;
    if (n == 1) return cps[0];
    // A character and its selectors: the tagged character, as before
    // clusters -- the FIRST selector decides (Unicode's rule).
    int sel = 1;
    for (int i = 1; i < n; i++)
        if (cps[i] != UEMOJI_VS_EMOJI && cps[i] != UEMOJI_VS_TEXT) { sel = 0; break; }
    if (sel) return cps[0] | (cps[1] == UEMOJI_VS_EMOJI ? UGFX_CP_EMOJI : UGFX_CP_TEXT);
    // A letter and marks that compose: the precomposed character.
    n = ucompose_cluster(cps, n);
    if (n == 1) return cps[0];
    return intern(cps, n);
}

int ugfx_cluster_cps(uint32_t cp, const uint32_t **cps) {
    if (!(cp & UGFX_CP_CLUSTER) || (cp & (UGFX_CP_EMOJI | UGFX_CP_TEXT))) return 0;
    uint32_t v = cp & UGFX_CP_VALUE;
    if (v < UGFX_CLUSTER_FIRST || (int)(v - UGFX_CLUSTER_FIRST) >= g_count) return 0;
    uint32_t id = v - UGFX_CLUSTER_FIRST;
    if (cps) *cps = g_list[id].cps;
    return g_list[id].n;
}

