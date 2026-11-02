// A hash table with an LRU list, intrusive (lib/ulru.h).
#include "lib/ulru.h"

#include <stdlib.h>
#include <string.h>

static void unlink_lru(struct ulru *l, struct ulru_node *n) {
    if (n->newer) n->newer->older = n->older; else l->head = n->older;
    if (n->older) n->older->newer = n->newer; else l->tail = n->newer;
    n->newer = n->older = 0;
}

static void push_lru(struct ulru *l, struct ulru_node *n) {
    n->newer = 0;
    n->older = l->head;
    if (l->head) l->head->newer = n;
    l->head = n;
    if (!l->tail) l->tail = n;
}

struct ulru_node *ulru_find(struct ulru *l, unsigned hash,
                            int (*eq)(const struct ulru_node *n, const void *key), const void *key) {
    if (!l->nbuckets) return 0;
    for (struct ulru_node *n = l->buckets[hash & (l->nbuckets - 1)]; n; n = n->hnext)
        if (n->hash == hash && eq(n, key)) {
            if (l->head != n) {
                unlink_lru(l, n);
                push_lru(l, n);
            }
            return n;
        }
    return 0;
}

// One bucket per entry on average; a failed growth keeps the old table.
static int fit(struct ulru *l) {
    if (l->nbuckets && (unsigned)l->count < l->nbuckets) return 1;
    unsigned n = l->nbuckets ? l->nbuckets * 2 : 256;
    struct ulru_node **nb = calloc(n, sizeof *nb);
    if (!nb) return l->nbuckets != 0;
    for (unsigned i = 0; i < l->nbuckets; i++)
        for (struct ulru_node *e = l->buckets[i], *nx; e; e = nx) {
            nx = e->hnext;
            e->hnext = nb[e->hash & (n - 1)];
            nb[e->hash & (n - 1)] = e;
        }
    free(l->buckets);
    l->buckets = nb;
    l->nbuckets = n;
    return 1;
}

int ulru_insert(struct ulru *l, struct ulru_node *n, unsigned hash) {
    if (!fit(l)) return 0;
    n->hash = hash;
    n->hnext = l->buckets[hash & (l->nbuckets - 1)];
    l->buckets[hash & (l->nbuckets - 1)] = n;
    push_lru(l, n);
    l->count++;
    return 1;
}

struct ulru_node *ulru_oldest(const struct ulru *l) { return l->tail; }

void ulru_remove(struct ulru *l, struct ulru_node *n) {
    unlink_lru(l, n);
    for (struct ulru_node **pp = &l->buckets[n->hash & (l->nbuckets - 1)]; *pp; pp = &(*pp)->hnext)
        if (*pp == n) { *pp = n->hnext; break; }
    n->hnext = 0;
    l->count--;
}

void ulru_free(struct ulru *l) {
    free(l->buckets);
    memset(l, 0, sizeof *l);
}
