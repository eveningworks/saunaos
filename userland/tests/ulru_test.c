// lib/ulru.h, the hash table + LRU list under ugfx's and uglyph's glyph
// caches: a find makes its entry the newest, eviction takes the oldest,
// a table grown past its first 256 buckets still finds every entry, a
// removal from the middle keeps both the chain and the list whole, and
// entries whose hashes COLLIDE are told apart by the key.
#include "lib/ulru.h"
#include "lib/utest.h"

struct ent { struct ulru_node node; int key; };

static int is(const struct ulru_node *n, const void *key) {
    return ((const struct ent *)n)->key == *(const int *)key;
}

// A deliberately poor hash, so chains are long and collisions certain.
static unsigned h(int k) { return (unsigned)(k % 37); }

static struct ent *find(struct ulru *l, int k) { return (struct ent *)ulru_find(l, h(k), is, &k); }

int main(void) {
    utest_begin("ulru_test", "the hash table and LRU list under the glyph caches", UTEST_VERDICT_FILE);
    struct ulru l = { 0 };
    enum { N = 1000 };
    static struct ent e[N];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        e[i].key = i;
        ok &= ulru_insert(&l, &e[i].node, h(i));
    }
    utest_checkf(ok && l.count == N && l.nbuckets >= N, "%d inserted, grown to %u buckets", l.count, l.nbuckets);
    int found = 0;
    for (int i = 0; i < N; i++) found += find(&l, i) == &e[i];
    utest_checkf(found == N, "every entry found after growth and collisions (%d of %d)", found, N);

    // The finds above ran 0..N-1, so the oldest is 0 and the newest N-1.
    utest_check(ulru_oldest(&l) == &e[0].node && l.head == &e[N - 1].node, "the finds left 0 oldest, N-1 newest");
    find(&l, 0);
    utest_check(l.head == &e[0].node && ulru_oldest(&l) == &e[1].node, "a find makes its entry the newest");

    ulru_remove(&l, &e[500].node);
    utest_check(!find(&l, 500) && find(&l, 537) == &e[537] && find(&l, 463) == &e[463],
                "a removal from the middle of a chain keeps its neighbours");
    int walked = 0;
    for (struct ulru_node *n = ulru_oldest(&l); n; n = n->newer) walked++;
    utest_checkf(walked == N - 1 && l.count == N - 1, "the list is whole after a removal (%d walked)", walked);

    int order = 1;
    struct ulru_node *prev = 0;
    for (int i = 0; i < 5; i++) {
        struct ulru_node *o = ulru_oldest(&l);
        ulru_remove(&l, o);
        if (o == prev) order = 0;
        prev = o;
    }
    utest_check(order && l.count == N - 6 && !find(&l, 1), "eviction takes the oldest each time");
    ulru_free(&l);
    utest_check(!l.buckets && !l.count, "ulru_free empties the table");
    return utest_end();
}
