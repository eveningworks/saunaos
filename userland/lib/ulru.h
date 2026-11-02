#ifndef ULRU_H
#define ULRU_H

// ulru -- a hash table with a least-recently-used list, INTRUSIVE: the
// caller's entry embeds a `struct ulru_node`, and the caller keeps the
// key, the bytes and the budget. What a cache of rendered things needs
// (ugfx's glyph cache, uglyph's): a lookup that is a hash and a short
// chain, not a scan, and the oldest entry to evict when over budget.
//
// The table grows to one bucket per entry on average; a failed growth
// keeps the old table (longer chains, still right). Nothing here
// allocates an entry or frees one -- eviction hands the node back.
//
//     struct my_entry { struct ulru_node node; ...key... ...value... };
//     struct ulru_node *n = ulru_find(&c, h, my_eq, &key);
//     if (!n) { e = make(); if (!ulru_insert(&c, &e->node, h)) free(e); }
//     while (over budget && (n = ulru_oldest(&c))) { ulru_remove(&c, n); free(n); }

struct ulru_node {
    struct ulru_node *hnext;            // bucket chain
    struct ulru_node *newer, *older;    // the LRU list, newest at head
    unsigned hash;
};

struct ulru {
    struct ulru_node **buckets;
    unsigned nbuckets;                  // a power of two, or 0 before first insert
    int count;
    struct ulru_node *head, *tail;      // newest, oldest
};

// The entry with this hash that `eq(node, key)` accepts, made the newest;
// NULL when there is none.
struct ulru_node *ulru_find(struct ulru *l, unsigned hash,
                            int (*eq)(const struct ulru_node *n, const void *key), const void *key);
// Adds `n` as the newest. 0 only when there is no table at all and none
// could be allocated -- the caller still owns `n` then.
int ulru_insert(struct ulru *l, struct ulru_node *n, unsigned hash);
// The least recently used entry, still in the table; NULL when empty.
struct ulru_node *ulru_oldest(const struct ulru *l);
// Takes `n` out of the table and the list; the caller frees it.
void ulru_remove(struct ulru *l, struct ulru_node *n);
// Frees the bucket array; the entries are the caller's to have freed.
void ulru_free(struct ulru *l);

#endif
