// The whole dotted tree -- see usetting_tree.h.
#include "base/usetting_tree.h"
#include "base/usetting.h"
#include "query_abi.h"
#include "syscall_abi.h"   // SYS_QUERY
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int usetting_tree_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca == '.') ca = 1;   // below every character a name may hold
        if (cb == '.') cb = 1;
        if (ca != cb || !ca) return (int)ca - (int)cb;
    }
}

// THE FIELD OPS THROUGH sys_call(), NOT rt's sys_query_field_*(): a
// shared library finds its sys_* calls in the PROGRAM's runtime, and one
// built before those names were exported could not load this library
// (tools/checks/check_libc_imports.py, libc's prctl.c).
static int field_op(struct query_msg *m) {
    return sys_call(SYS_QUERY, (uint64_t)(uintptr_t)m, 0, 0) < 0 ? -1 : 0;
}

static int field_count(unsigned cls) {
    struct query_msg m;
    memset(&m, 0, sizeof m);
    m.op = QUERY_OP_FIELD_COUNT;
    m.cls = cls;
    return field_op(&m) ? -1 : (int)m.returned;
}

static int field_info(unsigned cls, unsigned index, char *name, size_t cap) {
    struct query_msg m;
    memset(&m, 0, sizeof m);
    m.op = QUERY_OP_FIELD_INFO;
    m.cls = cls;
    m.index = index;
    if (field_op(&m)) return -1;
    m.name[sizeof m.name - 1] = '\0';
    strlcpy(name, m.name, cap);
    return 0;
}

static int field_get(const char *qualified, unsigned long long *value, unsigned *type) {
    struct query_msg m;
    memset(&m, 0, sizeof m);
    m.op = QUERY_OP_FIELD_GET;
    if (strlcpy(m.name, qualified, sizeof m.name) >= sizeof m.name) return -1;
    if (field_op(&m)) return -1;
    *value = m.value;
    *type = m.type;
    return 0;
}

static int by_name(const void *a, const void *b) {
    return usetting_tree_cmp(((const struct usetting_entry *)a)->name,
                             ((const struct usetting_entry *)b)->name);
}

struct grow {
    struct usetting_entry *v;
    int n, cap, failed;
};

static struct usetting_entry *add(struct grow *g) {
    if (g->n == g->cap) {
        int cap = g->cap ? g->cap * 2 : 256;
        struct usetting_entry *v = realloc(g->v, (size_t)cap * sizeof *v);
        if (!v) { g->failed = 1; return 0; }
        g->v = v;
        g->cap = cap;
    }
    struct usetting_entry *e = &g->v[g->n++];
    memset(e, 0, sizeof *e);
    return e;
}

int usetting_tree_collect(struct usetting_entry **out) {
    *out = 0;
    struct grow g = { 0 };
    // STATIC: a setting_msg is over a kilobyte, against ring 3's
    // 2048-byte frame budget. Callers are single-threaded tools.
    static struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    // A registry that cannot be read leaves the settings out; the facts
    // are the kernel's and still come.
    int n = usetting_dispatch(&m) == 0 ? (int)m.count : 0;
    for (int i = 0; i < n && !g.failed; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;
        struct usetting_entry *e = add(&g);
        if (!e) break;
        if (m.ns[0]) snprintf(e->name, sizeof e->name, "%s.%s", m.ns, m.name);
        else strlcpy(e->name, m.name, sizeof e->name);
        strlcpy(e->value, m.value, sizeof e->value);
        e->index = i;
        e->type = (uint8_t)m.type;
    }
    // Every SCALAR provider's named fields; a list has no single value.
    struct query_provider_info p;
    QUERY_FOREACH(QUERY_PROVIDERS, p, i) {
        if (g.failed) break;
        if (p.flags & QUERY_F_LIST) continue;
        int nf = field_count(p.cls);
        for (int f = 0; f < nf; f++) {
            char field[QUERY_FIELD_PATH_MAX], q[SETTING_ABI_QUALIFIED_MAX];
            unsigned type = 0;
            unsigned long long val = 0;
            if (field_info(p.cls, (unsigned)f, field, sizeof field) != 0) continue;
            snprintf(q, sizeof q, "%s.%s", p.name, field);
            if (field_get(q, &val, &type) != 0) continue;
            struct usetting_entry *e = add(&g);
            if (!e) break;
            strlcpy(e->name, q, sizeof e->name);
            snprintf(e->value, sizeof e->value, "%llu", val);
            e->index = -1;
            e->fact = 1;
            e->type = (uint8_t)type;
        }
    }
    // Out of memory part way: what was read, sorted, rather than nothing
    // -- and said, since the tree is then short.
    if (g.failed) sys_print("usetting_tree: out of memory -- the tree is incomplete\n");
    if (!g.v) return -1;
    if (g.n) qsort(g.v, (size_t)g.n, sizeof g.v[0], by_name);
    *out = g.v;
    return g.n;
}
