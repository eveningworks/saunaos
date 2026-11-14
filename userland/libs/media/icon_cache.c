// See icon_cache.h.
#include "media/icon_cache.h"
// ulog(), not the WM's logger: this moved out of userland/apps/system/toywm/ when the
// toolkit's sidebar needed icons too, and a library that logs through
// one app's helper is a library only that app can link.
#include "base/ulog.h"
#include "string.h"
#include "kfmt.h"
#include <stdlib.h>   // realloc, for a batch

#define ICON_DIR       "/usr/share/icons"
#define ICON_NAME_MAX  24
// SIZED FROM THE REAL WORKING SET, which is apps x sizes: ~14 registry
// entries against the desktop grid, the taskbar, a title bar, a Start
// menu row, the Start button and a sidebar. 32 was set when the comment
// below said "eleven apps and three sizes" -- which is 33, already over
// its own cap, and the wholesale drop below then re-decoded every icon
// on EVERY frame. Measured: opening the Start menu took a desktop from
// 30 cached to the cap, after which the count collapsed to 18, 20, 14,
// 6 frame after frame and the WM logged 100-260 ms frames.
#define ICON_CACHE_MAX 96

struct icon_entry {
    char name[ICON_NAME_MAX];
    int size;
    int missing;          // looked for and not there: remembered, so a
                          // fallback icon costs one failed open, not one
                          // per frame
    unsigned long long used; // last hit, for the LRU below
    struct uimg img;      // scaled to `size`; px is NULL when missing
};

static struct icon_entry g_cache[ICON_CACHE_MAX];
static int g_count;
static unsigned long long g_clock;   // monotonic, for `used`
static int g_evictions;              // reported by `gui icons`
static unsigned long g_inline;       // decodes icon_get() did itself

void icon_cache_invalidate(void) {
    for (int i = 0; i < g_count; i++) uimg_free(&g_cache[i].img);
    k_memset(g_cache, 0, sizeof g_cache);
    g_count = 0;
}

int icon_cache_count(void) { return g_count; }
int icon_cache_evictions(void) { return g_evictions; }

static struct icon_entry *find(const char *name, int size) {
    for (int i = 0; i < g_count; i++)
        if (g_cache[i].size == size && k_strcmp(g_cache[i].name, name) == 0)
            return &g_cache[i];
    return NULL;
}

// The file for `name`, decoded and scaled into *out: 0, or -1 (logged).
// Touches no cache state, so any thread may call it.
static int decode(const char *name, int size, struct uimg *out) {
    char path[64];
    k_snprintf(path, sizeof path, "%s/%s.qoi", ICON_DIR, name);
    struct uimg master;
    if (uimg_load(path, &master) < 0) {
        // Logged ONCE per (name, size), because the entry is kept as a
        // negative result -- an icon that is missing must not produce a
        // line per repaint.
        ulogf("icons: %s not loaded -- %s\n", path, uimg_last_error());
        return -1;
    }
    int rc = uimg_scale(&master, size, size, out);
    uimg_free(&master);
    if (rc < 0) {
        ulogf("icons: %s could not be scaled to %d -- %s\n", path, size,
                uimg_last_error());
        return -1;
    }
    return 0;
}

const struct uimg *icon_get(const char *name, int size) {
    if (!name || !name[0] || size <= 0) return NULL;

    struct icon_entry *e = find(name, size);
    if (e) { e->used = ++g_clock; return e->missing ? NULL : &e->img; }

    // FULL DROPS ONE ENTRY, NOT ALL OF THEM. It used to drop the cache
    // wholesale, on the reasoning that the cap was never reached -- and
    // when it was, the next frame re-decoded everything and hit the cap
    // again, so the cache became a decode-per-frame with extra steps.
    // A single LRU eviction degrades instead: a working set one entry
    // over the cap costs one decode per frame, not thirty.
    //
    // THE CAP IS STILL WHAT MATTERS. An LRU over a cap below the working
    // set thrashes just as surely, only more slowly -- which is why
    // `gui icons` reports the eviction count, so the next time this is
    // too small it says so instead of being silently slow.
    if (g_count >= ICON_CACHE_MAX) {
        int lru = 0;
        for (int i = 1; i < g_count; i++)
            if (g_cache[i].used < g_cache[lru].used) lru = i;
        if (!g_evictions)
            ulogf("icons: cache full (%d entries) -- evicting, see `gui icons`\n",
                  ICON_CACHE_MAX);
        g_evictions++;
        uimg_free(&g_cache[lru].img);
        g_cache[lru] = g_cache[g_count - 1];
        g_count--;
    }

    e = &g_cache[g_count++];
    // CLEARED: after an eviction this slot still holds a copy of the entry
    // moved down, its pixels included -- a failed decode left that alias
    // behind, freed twice at the next eviction.
    k_memset(e, 0, sizeof *e);
    e->used = ++g_clock;
    k_strlcpy(e->name, name, sizeof e->name);
    e->size = size;
    e->missing = 1;

    g_inline++;
    if (decode(name, size, &e->img) == 0) e->missing = 0;
    return e->missing ? NULL : &e->img;
}

// ---- batches -------------------------------------------------------------

struct icon_batch_item {
    char name[ICON_NAME_MAX];
    int size;
    int rc;               // decode()'s answer, once decoded
    struct uimg img;
};

int icon_batch_add(struct icon_batch *b, const char *name, int size) {
    if (!b || !name || !name[0] || size <= 0) return 0;
    for (int i = 0; i < b->n; i++)
        if (b->items[i].size == size && k_strcmp(b->items[i].name, name) == 0) return 1;
    if (b->n == b->cap) {
        int cap = b->cap ? 2 * b->cap : 32;
        struct icon_batch_item *g = realloc(b->items, (size_t)cap * sizeof *g);
        if (!g) return 0;
        b->items = g;
        b->cap = cap;
    }
    struct icon_batch_item *it = &b->items[b->n++];
    k_memset(it, 0, sizeof *it);
    k_strlcpy(it->name, name, sizeof it->name);
    it->size = size;
    it->rc = -1;
    return 1;
}

int icon_cache_sizes(int *out, int max) {
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        int seen = 0;
        for (int k = 0; k < n; k++) seen |= out[k] == g_cache[i].size;
        if (!seen && n < max) out[n++] = g_cache[i].size;
    }
    return n;
}

void icon_batch_decode(struct icon_batch *b) {
    for (int i = 0; b && i < b->n; i++)
        b->items[i].rc = decode(b->items[i].name, b->items[i].size, &b->items[i].img);
}

void icon_cache_install(struct icon_batch *b) {
    for (int i = 0; b && i < b->n; i++) {
        struct icon_batch_item *it = &b->items[i];
        struct icon_entry *e = find(it->name, it->size);
        if (!e) {
            if (g_count >= ICON_CACHE_MAX) continue;   // never evicts for a prefetch
            e = &g_cache[g_count++];
            k_memset(e, 0, sizeof *e);
            k_strlcpy(e->name, it->name, sizeof e->name);
            e->size = it->size;
            e->used = ++g_clock;
        } else {
            uimg_free(&e->img);
        }
        e->missing = it->rc != 0;
        e->img = it->img;           // the batch's pixels move into the cache
        k_memset(&it->img, 0, sizeof it->img);
    }
    icon_batch_free(b);
}

void icon_batch_free(struct icon_batch *b) {
    if (!b) return;
    for (int i = 0; i < b->n; i++) uimg_free(&b->items[i].img);
    free(b->items);
    b->items = 0;
    b->n = b->cap = 0;
}

unsigned long icon_cache_inline_decodes(void) { return g_inline; }
