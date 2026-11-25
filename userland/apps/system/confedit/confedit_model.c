// The Configuration Editor's data: the tree, its branches, the rows --
// see confedit_internal.h.
#include "confedit_internal.h"
#include "base/usetting.h"
#include "base/usetting_schema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_no_find[1];
struct ce_model g_m = { .branch = -1, .find = g_no_find };

// Case-insensitive substring: what Find matches a name or a value by.
static int contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < n && hay[k]) {
            char a = hay[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k == n) return 1;
    }
    return 0;
}

// One branch per distinct proper prefix, in the tree's order -- the
// entries are sorted so that a branch's names follow it directly, which
// makes this one pass with a stack of the branches still open.
static int build_branches(struct ce_branch **out) {
    int cap = 64, nb = 0;
    struct ce_branch *b = malloc((size_t)cap * sizeof *b);
    if (!b) return -1;
    int stack[SETTING_ABI_QUALIFIED_MAX], depth = 0;   // branch indices, one per open level
    for (int i = 0; i <= g_m.n; i++) {
        const char *name = i < g_m.n ? g_m.e[i].name : "";
        // Close the open branches this name is not under.
        while (depth > 0) {
            const struct ce_branch *top = &b[stack[depth - 1]];
            size_t len = strlen(top->name);
            if (!strncmp(name, top->name, len) && name[len] == '.') break;
            b[stack[--depth]].last = i;
        }
        if (i == g_m.n) break;
        // Open a branch for each further dot.
        for (const char *dot = strchr(name, '.'); dot; dot = strchr(dot + 1, '.')) {
            int len = (int)(dot - name), lvl = 0;
            for (const char *p = name; p < dot; p++) lvl += *p == '.';
            if (lvl < depth) continue;   // already open
            if (nb == cap) {
                cap *= 2;
                struct ce_branch *nbuf = realloc(b, (size_t)cap * sizeof *b);
                if (!nbuf) { free(b); return -1; }
                b = nbuf;
            }
            struct ce_branch *br = &b[nb];
            memset(br, 0, sizeof *br);
            snprintf(br->name, sizeof br->name, "%.*s", len, name);
            const char *seg = strrchr(br->name, '.');
            strlcpy(br->label, seg ? seg + 1 : br->name, sizeof br->label);
            br->depth = lvl;
            // Its own value, when it has one, sorts just before its first child.
            br->first = i > 0 && !strcmp(g_m.e[i - 1].name, br->name) ? i - 1 : i;
            if (depth > 0) b[stack[depth - 1]].kids = 1;
            stack[depth++] = nb++;
        }
    }
    for (int k = 0; k < nb; k++) {
        for (int i = b[k].first; i < b[k].last; i++) b[k].facts += g_m.e[i].fact;
        if (b[k].depth) continue;
        int n = b[k].last - b[k].first;
        if (b[k].facts == n) snprintf(b[k].note, sizeof b[k].note, "%d fact%s", n, n == 1 ? "" : "s");
        else snprintf(b[k].note, sizeof b[k].note, "%d", n);
    }
    *out = b;
    return nb;
}

int ce_model_load(void) {
    struct usetting_entry *e;
    int n = usetting_tree_collect(&e);
    if (n < 0) return -1;
    struct ce_info *info = calloc((size_t)(n ? n : 1), sizeof *info);
    int *rows = malloc((size_t)(n ? n : 1) * sizeof *rows);
    if (!info || !rows) { free(e); free(info); free(rows); return -1; }

    struct ce_branch *old = g_m.b;
    int old_n = g_m.nb;
    free(g_m.e);
    free(g_m.info);
    free(g_m.rows);
    g_m.e = e;
    g_m.info = info;
    g_m.n = n;
    g_m.rows = rows;
    g_m.nrows = 0;
    g_m.b = 0;
    g_m.nb = 0;
    struct ce_branch *b;
    int nb = build_branches(&b);
    if (nb >= 0) {
        g_m.b = b;
        g_m.nb = nb;
        // What was open stays open, by name: an index moves when a
        // setting comes or goes.
        for (int k = 0; k < old_n; k++)
            if (old[k].open) {
                int j = ce_model_branch_named(old[k].name);
                if (j >= 0) g_m.b[j].open = 1;
            }
    }
    free(old);
    return 0;
}

int ce_model_branch_named(const char *name) {
    for (int k = 0; k < g_m.nb; k++)
        if (!strcmp(g_m.b[k].name, name)) return k;
    return -1;
}

int ce_model_entry_named(const char *name) {
    for (int i = 0; i < g_m.n; i++)
        if (!strcmp(g_m.e[i].name, name)) return i;
    return -1;
}

int ce_model_parent_of(int entry) {
    if (entry < 0 || entry >= g_m.n) return -1;
    char parent[SETTING_ABI_QUALIFIED_MAX];
    strlcpy(parent, g_m.e[entry].name, sizeof parent);
    char *dot = strrchr(parent, '.');
    if (!dot) return -1;
    *dot = '\0';
    return ce_model_branch_named(parent);
}

void ce_model_rows(int branch, const char *find) {
    g_m.nrows = 0;
    char *f = find && find[0] ? strdup(find) : g_no_find;
    if (!f) f = g_no_find;   // out of memory: the branch's rows, not a find
    if (g_m.find != g_no_find) free(g_m.find);
    g_m.find = f;
    if (g_m.find[0]) {
        g_m.branch = -1;
        for (int i = 0; i < g_m.n; i++)
            if (contains(g_m.e[i].name, g_m.find) || contains(g_m.e[i].value, g_m.find))
                g_m.rows[g_m.nrows++] = i;
    } else {
        g_m.branch = branch >= 0 && branch < g_m.nb ? branch : -1;
        if (g_m.branch >= 0)
            for (int i = g_m.b[branch].first; i < g_m.b[branch].last; i++) g_m.rows[g_m.nrows++] = i;
    }
    for (int r = 0; r < g_m.nrows; r++) ce_model_info(g_m.rows[r]);
}

const struct ce_info *ce_model_info(int i) {
    static const struct ce_info none = { 1, CE_FROM_KERNEL, 0, 0, 0 };
    if (i < 0 || i >= g_m.n) return &none;
    struct ce_info *in = &g_m.info[i];
    if (in->known) return in;
    in->known = 1;
    in->from = CE_FROM_KERNEL;
    if (g_m.e[i].fact) return in;
    // A name the merged registry lists but no file declares is the
    // kernel's: its value is the kernel's, stored in /etc by the kernel.
    static struct uschema s;
    if (!uschema_find(g_m.e[i].name, &s)) return in;
    in->declared = 1;
    in->scope = (signed char)s.scope;
    struct uschema_origin o;
    int from = uschema_origin(&s, &o);
    in->from = from == USCHEMA_FROM_USER ? CE_FROM_USER : from == USCHEMA_FROM_MACHINE ? CE_FROM_MACHINE
             : CE_FROM_DEFAULT;
    static char path[USCHEMA_PATH_MAX];   // PATH_MAX: off the 2 KiB frame budget
    in->person = uschema_target(&s, path, sizeof path) == 1;
    return in;
}

void ce_model_refresh(int i) {
    if (i < 0 || i >= g_m.n || g_m.e[i].fact) return;
    char v[SETTING_ABI_VALUE_MAX];
    if (usetting_get(g_m.e[i].name, v, sizeof v)) strlcpy(g_m.e[i].value, v, sizeof g_m.e[i].value);
    g_m.info[i].known = 0;
    ce_model_info(i);
}

const char *ce_from_word(int from) {
    switch (from) {
    case CE_FROM_USER: return "You";
    case CE_FROM_MACHINE: return "Machine";
    case CE_FROM_DEFAULT: return "Default";
    default: return "Kernel";
    }
}

const char *ce_type_word(const struct usetting_entry *e) {
    if (e->fact) return "fact";
    switch (e->type) {
    case SETTING_ABI_TYPE_ENUM: return "enum";
    case SETTING_ABI_TYPE_INT: return "int";
    case SETTING_ABI_TYPE_KEYCOMBO: return "keys";
    default: return "text";
    }
}
