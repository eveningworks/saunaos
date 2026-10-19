// `config` -- read, change and find this machine's configuration.
//
// toy-os keeps its settings the way Unix does: plain `name=value` text
// under /etc, editable with `edit` and readable with `cat`. That is
// deliberate and is not changing. What it costs is the thing everyone
// who has used /etc knows -- finding the right file, and knowing which
// keys a file is even allowed to contain. Nothing in a directory of
// text files can answer either question about itself.
//
// So the kernel keeps two registries and this program is their front
// end (api/setting.h, api/config_file.h, over SYS_SETTING):
//
//   * SETTINGS -- typed, validated, applied. `list` shows every one
//     WITH THE FILE IT LIVES IN, which is the index /etc never had.
//   * CONFIG FILES -- the /etc documents themselves, including ones
//     holding no registered setting at all (the timezone database, the
//     keymap tables). `files` is the map; `register` adds to it.
//
// THE DESIGN POINT WORTH KEEPING: this program contains no `name=value`
// parser. Every value it reports comes back from the kernel's one
// parser (kernel/lib/etc_config.c), including the FILE's value as
// distinct from the live one (`diff`). A second parser here would drift
// from that one, and the drift would show up as a setting that `config`
// and the system disagree about -- which is precisely the class of bug
// the shared kernel/lib toolkit exists to prevent. `find` searches raw
// LINES for that reason: grep semantics need no parser at all.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdlib.h>   // qsort -- the tree, by name
#include <ctype.h>
#include <stdio.h>
#include "setting_abi.h" // enum setting_result, struct setting_msg
#include <fcntl.h>
#include <unistd.h>
#include "lib/usetting.h" // the MERGED registry -- see the header
#include "lib/uargs.h"
#include "etc_renamed.h"   // the names from before the dotted ones
#include "query_abi.h"     // walking the facts for a branch

static void put(const char *s) { sys_print(s); }

static void putline(const char *s) { sys_print(s); sys_print("\n"); }

// --- talking to the registry -----------------------------------------

static int op(struct setting_msg *m, uint32_t which) {
    memset(m, 0, sizeof *m);
    m->op = which;
    return usetting_dispatch(m) == 0;
}

// Every path that changes a setting goes through this, so the
// three-way outcome is reported the same way everywhere. SETTING_UNSAVED
// is the one that matters: the change is LIVE but did not reach the
// disk, so it disappears at the next boot -- printing that as plain
// success is the exact lie enum setting_result was introduced to stop.
static int report(const char *name, const char *value, uint32_t result) {
    char line[160];
    switch (result) {
    case SETTING_SAVED:
        snprintf(line, sizeof line, "%s = %s", name, value);
        putline(line);
        return 0;
    case SETTING_UNSAVED:
        snprintf(line, sizeof line, "%s = %s  (applied, but NOT saved -- "
                                     "it will revert at the next boot)", name, value);
        putline(line);
        return 1;
    default:
        snprintf(line, sizeof line, "config: '%s' does not accept '%s'", name, value);
        putline(line);
        return 1;
    }
}

// --- reading whole files ---------------------------------------------
//
// Straight bytes, no interpretation. `show` prints them and `find`
// scans them for a substring; neither needs to know the file format,
// which is what keeps a second parser out of this program.
#define FILE_MAX 8192

static int read_file(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t n = read(fd, buf, (size_t)(cap - 1));
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

// --- subcommands ------------------------------------------------------

// --- qualified names -------------------------------------------------
//
// A setting's identity is (namespace, name) -- the namespace being the
// registered name of the file it lives in, so `font.size` in
// /etc/toyos.conf is `system.font.size` (api/setting.h). A bare name
// still works when only one setting has it; when several do, this tool
// REFUSES and lists them rather than picking, because picking would
// make the answer depend on registration order.
//
// The matching is done HERE, over the enumerated registry, rather than
// with a new syscall: the kernel already refuses an ambiguous bare
// name, and what a user needs on top of that refusal is "did you mean
// these two?" -- which needs the list this tool already reads for
// `config list`.
// `bare` set: `want` is read WHOLE as the key, the second reading a
// dotted key needs (`font.size` is not namespace `font`) -- the same
// two readings, in the same order, as the kernel's setting_find().
static int names_match_as(const char *ns, const char *name, const char *want, int bare) {
    const char *dot = 0;
    for (const char *p = want; *p; p++) if (*p == '.') { dot = p; break; }
    if (!dot || bare) return strcmp(name, want) == 0;

    // Qualified: both halves must match. A namespace-less setting (its
    // file has no descriptor) can only ever be addressed bare.
    if (strncmp(ns, want, (size_t)(dot - want)) != 0) return 0;
    if (ns[dot - want] != '\0') return 0;
    return strcmp(name, dot + 1) == 0;
}

static int name_matches_as(const struct setting_msg *m, const char *want, int bare) {
    return names_match_as(m->ns, m->name, want, bare);
}

static int g_read_bare;   // which reading resolve_exact() settled on
static int name_matches(const struct setting_msg *m, const char *want) {
    return name_matches_as(m, want, g_read_bare);
}

// Fills `out` with "<ns>.<name>", or the bare name when there is no
// namespace -- the form this tool prints and accepts.
static void qualified(const struct setting_msg *m, char *out, int cap) {
    if (m->ns[0]) snprintf(out, cap, "%s.%s", m->ns, m->name);
    else          strlcpy(out, m->name, (unsigned)cap);
}

// How many registered settings `want` matches, writing the first one
// into `first` (if given) and printing all of them when there is more
// than one. Returns the count.
static int resolve_exact(const char *want, struct setting_msg *first, int complain);

// The name a setting has NOW for one it had before the dotted names
// (api/etc_renamed.h), with a line saying so; else `want` itself.
static const char *renamed_note(const char *want, char *buf, int cap) {
    if (resolve_exact(want, 0, 0) || !etc_renamed_now(want, buf, (uint32_t)cap)) return want;
    char line[160];
    snprintf(line, sizeof line, "  (%s is now %s)", want, buf);
    sys_eprint(line);
    sys_eprint("\n");
    return buf;
}

static int resolve(const char *want, struct setting_msg *first, int complain) {
    char now[SETTING_ABI_QUALIFIED_MAX];
    return resolve_exact(renamed_note(want, now, sizeof now), first, complain);
}

static int resolve_reading(const char *want, struct setting_msg *first, int complain);

static int resolve_exact(const char *want, struct setting_msg *first, int complain) {
    g_read_bare = 0;
    int hits = resolve_reading(want, first, complain);
    if (hits != 0 || !strchr(want, '.')) return hits;
    g_read_bare = 1;
    hits = resolve_reading(want, first, complain);
    if (hits <= 0) g_read_bare = 0;
    return hits;
}

// One INFO by merged index; 0, or -1.
static int info_at(int index, struct setting_msg *m) {
    memset(m, 0, sizeof *m);
    m->op = SETTING_OP_INFO;
    m->index = (uint32_t)index;
    return usetting_dispatch(m);
}

struct walk {
    const char *want;
    int bare;
    int hits, first;
    int list;            // second pass: print each match with its file
};

static int walk_match(int index, const char *ns, const char *name, void *ctx) {
    struct walk *w = ctx;
    if (!names_match_as(ns, name, w->want, w->bare)) return 0;
    if (w->list) {
        static struct setting_msg m;   // static: a kilobyte, and a frame is budgeted at 2
        if (info_at(index, &m) != 0) return 0;
        char q[SETTING_ABI_QUALIFIED_MAX], line[160];
        qualified(&m, q, sizeof q);
        snprintf(line, sizeof line, "    %-24s in %s", q, m.file);
        putline(line);
        return 0;
    }
    if (w->hits++ == 0) w->first = index;
    return 0;
}

static int resolve_reading(const char *want, struct setting_msg *first, int complain) {
    struct setting_msg m;
    // A QUALIFIED NAME IS LOOKED UP, NOT SEARCHED FOR (usetting_info()).
    // The walk below read every declaration file to answer one name,
    // which was most of a second of `config set` and several under load;
    // it stays for a bare name, a renamed one and one that is not there.
    if (!g_read_bare && strchr(want, '.') && usetting_info(want, &m) == 0) {
        if (first) *first = m;
        return 1;
    }
    // BY NAME FIRST: the walk matches names only, and the full INFO --
    // a declaration read, a value read -- is fetched for the matches.
    struct walk w = { want, g_read_bare, 0, -1, 0 };
    if (usetting_each_name(walk_match, &w) < 0) return -1;
    if (w.hits && first && info_at(w.first, first) != 0) return -1;

    if (w.hits > 1 && complain) {
        char line[160];
        snprintf(line, sizeof line,
                 "config: '%s' is ambiguous -- %d settings have that name:",
                 want, w.hits);
        putline(line);
        w.list = 1;
        usetting_each_name(walk_match, &w);
        putline("config: name one of them in full");
    }
    return w.hits;
}

static int cmd_list(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_COUNT)) { putline("config: registry unavailable"); return 1; }
    int n = m.count;

    // Qualified, because that is the name `config get`/`set` accept in
    // every case -- printing the bare one would show a name that may
    // not be usable on its own.
    putline("SETTING                   VALUE                 FILE");
    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;

        // A value that differs from the file is flagged rather than
        // hidden: it means someone edited /etc and nothing reloaded,
        // which is a normal state here and the commonest reason a
        // setting "isn't working".
        int edited = m.stored[0] && strcmp(m.value, m.stored) != 0;
        char line[200];
        char q[SETTING_ABI_QUALIFIED_MAX];
        qualified(&m, q, sizeof q);
        snprintf(line, sizeof line, "%-25s %-21s %s%s",
                 q, m.value, m.file, edited ? "  (file differs -- see `config diff`)" : "");
        putline(line);
    }
    return 0;
}

// The FACT fallback: `config get mem.frame_free`.
//
// Facts and settings share an addressing scheme -- (namespace, name) --
// so one command can read both, and only settings can be written. A
// `config set` on a fact fails because no setting has that name, which
// needs no special case.
//
// IT LABELS THE ANSWER. A fact prints with its provenance, never in the
// shape a setting prints in, because the two have different lifetimes: a
// setting survives a reboot and a fact does not EXIST between them. A
// reader who cannot tell which they just read has been told something
// misleading, however correct the number is.
//
// Returns 1 if this was a fact and it was handled, 0 to fall through to
// the "no setting named" message.
static int try_fact(const char *name) {
    unsigned long long value = 0;
    unsigned type = 0;
    char line[160];

    if (sys_query_field_get(name, &value, &type) == 0) {
        const char *what = (type == QUERY_TYPE_BYTES) ? "a byte count" : "a count";
        snprintf(line, sizeof line, "%llu", value);
        putline(line);
        snprintf(line, sizeof line,
                 "  (kernel fact, read-only -- %s, not stored in any file)", what);
        putline(line);
        return 1;
    }
    // ENOTSUP is a DIFFERENT answer from "no such fact": the class is
    // there and is a table, so send the reader to a tool that can show
    // one rather than leaving them hunting for a typo they did not make.
    if (sys_errno() == ENOTSUP) {
        snprintf(line, sizeof line,
                 "config: '%s' is a list of records, not a single value", name);
        putline(line);
        putline("  read it with a command built for it (e.g. `meminfo --list`)");
        return 1;
    }
    return 0;
}

// --- branches: `config get desktop.transparency` ----------------------
//
// A dotted name is a PATH (docs/settings-and-queries.md, "Names"):
// `desktop.transparency.taskbar` is a setting under the branch
// `desktop.transparency`, which may hold a value of its own -- the
// Windows registry's shape, a key with a default value -- and settings
// and kernel facts share the one tree, BSD sysctl's arrangement. A
// branch prints every name under it; a bare namespace prints ONE level
// with counts, dconf list's shape, unless --recursive.

// GROWN AS THE REGISTRY IS READ: a fixed table dropped every name past
// its size without a word.
static struct tree_ent {
    char name[SETTING_ABI_QUALIFIED_MAX];
    char value[SETTING_ABI_VALUE_MAX];
    char fact;               // a kernel fact: read-only, never stored
} *g_tree;
static int g_tree_n, g_tree_cap;
static int g_recursive;

static void tree_add(const char *name, const char *value, int fact) {
    if (g_tree_n == g_tree_cap) {
        int cap = g_tree_cap ? g_tree_cap * 2 : 256;
        struct tree_ent *t = realloc(g_tree, (size_t)cap * sizeof *t);
        if (!t) return;
        g_tree = t;
        g_tree_cap = cap;
    }
    struct tree_ent *e = &g_tree[g_tree_n++];
    strlcpy(e->name, name, sizeof e->name);
    strlcpy(e->value, value, sizeof e->value);
    e->fact = (char)fact;
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct tree_ent *)a)->name, ((const struct tree_ent *)b)->name);
}

static void tree_collect(void) {
    static struct setting_msg m;
    g_tree_n = 0;
    memset(&m, 0, sizeof m);
    if (op(&m, SETTING_OP_COUNT)) {
        int n = (int)m.count;
        for (int i = 0; i < n; i++) {
            memset(&m, 0, sizeof m);
            m.op = SETTING_OP_INFO;
            m.index = (uint32_t)i;
            if (usetting_dispatch(&m) != 0) continue;
            char q[SETTING_ABI_QUALIFIED_MAX];
            qualified(&m, q, sizeof q);
            tree_add(q, m.value, 0);
        }
    }
    // Every SCALAR provider's named fields; a list has no single value.
    struct query_provider_info p;
    QUERY_FOREACH(QUERY_PROVIDERS, p, i) {
        if (p.flags & QUERY_F_LIST) continue;
        int nf = sys_query_field_count(p.cls);
        for (int f = 0; f < nf; f++) {
            char field[QUERY_FIELD_PATH_MAX], q[SETTING_ABI_QUALIFIED_MAX], v[32];
            unsigned type = 0;
            unsigned long long val = 0;
            if (sys_query_field_info(p.cls, (unsigned)f, field, &type) != 0) continue;
            snprintf(q, sizeof q, "%s.%s", p.name, field);
            if (sys_query_field_get(q, &val, &type) != 0) continue;
            snprintf(v, sizeof v, "%llu", val);
            tree_add(q, v, 1);
        }
    }
    // By NAME, as dconf lists: registry order is an accident of
    // registration and of the directory, and differs between machines.
    qsort(g_tree, (size_t)g_tree_n, sizeof g_tree[0], by_name);
}

static int under(const char *name, const char *branch, size_t blen) {
    return (!blen || (!strncmp(name, branch, blen) && name[blen] == '.'));
}

static void tree_row(const char *name, const char *value, int fact, const char *more) {
    char line[200];
    snprintf(line, sizeof line, "%-38s %s%s%s", name, value, fact ? "  (fact)" : "", more);
    putline(line);
}

// Prints `branch`'s subtree and returns how many names were under it
// (0: not a branch, nothing printed). An empty branch is the root.
static int cmd_tree(const char *branch) {
    tree_collect();
    size_t blen = strlen(branch);
    int n = 0;
    for (int i = 0; i < g_tree_n; i++) n += under(g_tree[i].name, branch, blen) != 0;
    if (!n) return 0;

    int one_level = !g_recursive && !strchr(branch, '.');
    if (!one_level) {
        for (int i = 0; i < g_tree_n; i++)
            if (under(g_tree[i].name, branch, blen))
                tree_row(g_tree[i].name, g_tree[i].value, g_tree[i].fact, "");
        return n;
    }
    // ONE LEVEL: each child once, in first-seen order, with its own
    // value if it has one and how many names sit under it.
    for (int i = 0; i < g_tree_n; i++) {
        const char *name = g_tree[i].name;
        if (!under(name, branch, blen)) continue;
        const char *seg = name + (blen ? blen + 1 : 0);
        size_t slen = strcspn(seg, ".");
        char child[SETTING_ABI_QUALIFIED_MAX];
        snprintf(child, sizeof child, "%.*s", (int)(seg - name + slen), name);
        int seen = 0;
        for (int k = 0; k < i && !seen; k++)
            seen = under(g_tree[k].name, branch, blen) && (!strcmp(g_tree[k].name, child) ||
                   (!strncmp(g_tree[k].name, child, strlen(child)) && g_tree[k].name[strlen(child)] == '.'));
        if (seen) continue;
        const struct tree_ent *own = 0;
        int below = 0, facts = 0;
        size_t clen = strlen(child);
        for (int k = 0; k < g_tree_n; k++) {
            if (!strcmp(g_tree[k].name, child)) own = &g_tree[k];
            else if (under(g_tree[k].name, child, clen)) { below++; facts += g_tree[k].fact; }
        }
        char more[48] = "";
        if (below) snprintf(more, sizeof more, "  (+%d under it)", below);
        if (own) tree_row(child, own->value, own->fact, more);
        else {
            char label[SETTING_ABI_QUALIFIED_MAX + 2], what[40];
            snprintf(label, sizeof label, "%s.*", child);
            snprintf(what, sizeof what, "%d %s%s", below, facts == below ? "fact" : "setting",
                     below == 1 ? "" : "s");
            tree_row(label, what, 0, "");
        }
    }
    return n;
}

static int cmd_get_one(const char *name);

// WHETHER ANYTHING IS NAMED UNDER `n`, by name alone: the settings
// library's names-only answer (usetting_has_under()) and the facts' field
// names. A leaf -- what `get` is asked almost every time -- is then
// answered without tree_collect() reading the whole registry. A yes takes
// the full path, so a stray file costs time, never a wrong answer.
static int any_under(const char *n) {
    size_t len = strlen(n);
    if (usetting_has_under(n)) return 1;
    struct query_provider_info p;
    QUERY_FOREACH(QUERY_PROVIDERS, p, i) {
        if (p.flags & QUERY_F_LIST) continue;
        int nf = sys_query_field_count(p.cls);
        for (int f = 0; f < nf; f++) {
            char field[QUERY_FIELD_PATH_MAX], q[SETTING_ABI_QUALIFIED_MAX];
            unsigned type = 0;
            if (sys_query_field_info(p.cls, (unsigned)f, field, &type) != 0) continue;
            snprintf(q, sizeof q, "%s.%s", p.name, field);
            if (under(q, n, len)) return 1;
        }
    }
    return 0;
}

static int cmd_get(const char *name) {
    char now[SETTING_ABI_QUALIFIED_MAX];
    const char *n = name;
    if (resolve_exact(name, 0, 0) == 0) {
        unsigned long long v;
        unsigned t;
        if (sys_query_field_get(name, &v, &t) != 0 && etc_renamed_now(name, now, sizeof now)) {
            n = now;   // an old name of a SETTING; a branch never had one
            char line[160];
            snprintf(line, sizeof line, "  (%s is now %s)\n", name, now);
            sys_eprint(line);
        }
    }
    // A branch, or a name that is both a value and a branch: the tree.
    // A leaf keeps the one-value answer below, which scripts read.
    if (!any_under(n)) return cmd_get_one(n);
    tree_collect();
    size_t len = strlen(n);
    int kids = 0, own = 0;
    for (int i = 0; i < g_tree_n; i++) {
        kids |= under(g_tree[i].name, n, len);
        own |= !strcmp(g_tree[i].name, n);
    }
    if (!kids) return cmd_get_one(n);
    // A NAME WITH A VALUE AND CHILDREN prints the value FIRST, exactly
    // as a leaf does -- the bare line a script reads (`config get
    // desktop.transparency` is still `off`) -- and then what is under it.
    if (own && cmd_get_one(n) != 0) return 1;
    return cmd_tree(n) ? 0 : 1;
}

static int cmd_get_one(const char *name) {
    // STATIC, NOT LOCAL. `struct setting_msg` is over a kilobyte, and
    // the resolve-then-act commands hold two at once -- against
    // USERLAND_CFLAGS' 2048-byte frame budget and a 16 KiB ring-3 stack
    // with ONE guard page below it. This program is single-threaded and
    // never recurses, so the storage costs a page of .bss and buys the
    // whole frame back. (Trimming a field off the ABI to stay under the
    // warning would be treating the symptom: the message is a kilobyte
    // because it usefully carries a kilobyte.)
    static struct setting_msg found;
    int hits = resolve(name, &found, 1);
    if (hits < 0) { putline("config: registry unavailable"); return 1; }
    if (hits > 1) return 1;             // resolve() listed them
    if (hits == 0) {
        // A SETTING WINS over a fact of the same name. Not that one can
        // exist today -- the registries are separate and nothing checks
        // across them -- but resolving settings first means the writable
        // thing is what `config get` reports, so `get` and `set` can
        // never be talking about different objects.
        if (try_fact(name)) return 0;
        char line[128];
        snprintf(line, sizeof line, "config: no setting named '%s'", name);
        putline(line);
        putline("  (`config list` shows settings; a kernel fact is read-only)");
        return 1;
    }
    // The live value, not the one INFO reported a moment ago: GET is
    // what a caller means by "what is it now".
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    qualified(&found, m.name, sizeof m.name);
    if (usetting_dispatch(&m) != 0) {
        putline("config: registry unavailable");
        return 1;
    }
    putline(m.value);
    // THE VALUE ALONE WOULD BE A HALF-TRUTH here. `get` returns what
    // `set` last accepted -- which is the rule a setting must follow --
    // but on a machine where the setting cannot take effect that number
    // describes an intention, not the machine. Say which. Second line,
    // parenthesised, so the first line stays the bare value a script
    // reads.
    if (found.unavailable[0]) {
        char why[SETTING_ABI_DESC_MAX + 32];
        snprintf(why, sizeof why, "  (not in effect: %s)", found.unavailable);
        putline(why);
    }
    return 0;
}

static int cmd_set(const char *name, const char *value) {
    // A WRITE refuses an ambiguous name outright. Reading the wrong
    // setting shows you a wrong answer; writing the wrong one changes
    // something you did not mean to change and persists it -- which is
    // why macOS `defaults` makes the domain mandatory for a write and
    // optional for nothing.
    static struct setting_msg found;
    int hits = resolve(name, &found, 1);
    if (hits < 0) { putline("config: registry unavailable"); return 1; }
    if (hits > 1) return 1;             // resolve() listed them

    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    // Qualified when it resolved, verbatim when it did not -- an
    // unknown name has to reach the registry so the refusal comes from
    // the one place that decides what exists.
    if (hits == 1) qualified(&found, m.name, sizeof m.name);
    else           strlcpy(m.name, name, sizeof m.name);
    strlcpy(m.value, value, sizeof m.value);
    if (usetting_dispatch(&m) != 0) {
        // The registry refuses a name it does not have the same way it
        // reports being down; which one it was is known here.
        if (hits == 0) {
            char line[128];
            snprintf(line, sizeof line, "config: no setting named '%s'", name);
            putline(line);
            return 1;
        }
        putline("config: registry unavailable");
        return 1;
    }

    // A rejected value is worth more than "no": the registry knows the
    // legal ones, so say what they are rather than making the user go
    // and look.
    if (m.result == SETTING_INVALID) {
        // A SETTING THIS MACHINE CANNOT CHANGE IS NOT A BAD VALUE, and
        // saying "does not accept 'off' -- try one of: on off" would be
        // absurd. The registry knows why and says so; report it and
        // stop, rather than listing choices none of which would work.
        if (hits == 1 && found.unavailable[0]) {
            char why[SETTING_ABI_DESC_MAX + 32];
            snprintf(why, sizeof why, "config: %s", found.unavailable);
            putline(why);
            return 1;
        }
        int rc = report(name, value, m.result);
        // `found` IS REUSED here rather than a third message declared.
        // struct setting_msg passed a kilobyte when it gained a
        // description, so three of them on one frame is 2160 bytes
        // against USERLAND_CFLAGS' 2048-byte budget -- on a 16 KiB
        // ring-3 stack with a single guard page below it. `found` has
        // done its job by now (its name was copied into `m` above), and
        // the inner `ch` stays separate because it is live at the same
        // time as this one.
        if (!op(&found, SETTING_OP_COUNT)) return rc;
        int n = found.count;
        for (int i = 0; i < n; i++) {
            memset(&found, 0, sizeof found);
            found.op = SETTING_OP_INFO;
            found.index = i;
            if (usetting_dispatch(&found) != 0) continue;
            if (!name_matches(&found, name)) continue;
            // AN INT HAS NO CHOICES TO LIST -- it has a RANGE, and
            // saying so is the whole point of the refusal. Without this
            // a user who typed 0 was told only that 0 was not accepted,
            // with nothing to suggest what would be.
            if (found.type == SETTING_ABI_TYPE_INT) {
                char r[80];
                snprintf(r, sizeof r, "  try %d..%d%s%s (in steps of %d)\n",
                         (int)found.imin, (int)found.imax,
                         found.unit[0] ? " " : "", found.unit,
                         (int)(found.istep > 0 ? found.istep : 1));
                put(r);
                break;
            }
            if (found.type != SETTING_ABI_TYPE_ENUM || found.count <= 0) break;

            put("  try one of:");
            for (int c = 0; c < found.count; c++) {
                struct setting_msg ch;
                memset(&ch, 0, sizeof ch);
                ch.op = SETTING_OP_CHOICE;
                ch.index = i;
                ch.choice = c;
                if (usetting_dispatch(&ch) != 0) break;
                put(" ");
                // The DISPLAY name, which falls back to the value, so a
                // refusal lists what a reader will see in the UI.
                put(ch.label[0] ? ch.label : ch.value);
            }
            put("\n");
            break;
        }
        return rc;
    }
    return report(name, value, m.result);
}

static int cmd_unset(const char *name) {
    // Same rule as `set`: removing the wrong setting's key is a write.
    static struct setting_msg found;
    int hits = resolve(name, &found, 1);
    if (hits < 0) { putline("config: registry unavailable"); return 1; }
    if (hits > 1) return 1;             // resolve() listed them

    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    if (hits == 1) qualified(&found, m.name, sizeof m.name);
    else           strlcpy(m.name, name, sizeof m.name);
    if (usetting_dispatch(&m) != 0) { putline("config: registry unavailable"); return 1; }

    char line[160];
    if (m.result != SETTING_SAVED) {
        snprintf(line, sizeof line,
                 "config: '%s' is not a setting, or was not stored anyway", name);
        putline(line);
        return 1;
    }
    // Honest about the half it cannot do: removing the key means the
    // BUILT-IN default applies at the next boot, but nothing here can
    // ask a live subsystem to go back to one.
    snprintf(line, sizeof line,
             "%s removed from its file -- the built-in default applies at the next boot "
             "(the current value stays in effect until then)", name);
    putline(line);
    return 0;
}

static int cmd_where(const char *name) {
    static struct setting_msg found;
    int hits = resolve(name, &found, 1);
    if (hits < 0) { putline("config: registry unavailable"); return 1; }
    if (hits > 1) return 1;             // resolve() listed them
    if (hits == 1) {
        putline(found.file); // just the path, so it is usable in a command
        return 0;
    }
    char line[128];
    snprintf(line, sizeof line, "config: no setting named '%s'", name);
    putline(line);
    return 1;
}

static int cmd_diff(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_COUNT)) return 1;
    int n = m.count, differ = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;
        if (!m.stored[0] || strcmp(m.value, m.stored) == 0) continue;

        char line[200];
        char q[SETTING_ABI_QUALIFIED_MAX];
        qualified(&m, q, sizeof q);
        snprintf(line, sizeof line, "%-25s live=%-16s file=%s", q, m.value, m.stored);
        putline(line);
        differ++;
    }
    if (!differ) {
        putline("Every setting matches its file.");
        return 0;
    }
    putline("");
    putline("`config reload` applies the file values above.");
    return 0;
}

static int cmd_reload(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_RELOAD)) { putline("config: reload failed"); return 1; }

    // The rejected count is the load-bearing part: a typo in a file
    // someone just edited must not look like the setting simply not
    // working. `count` is how many values the owning subsystem refused.
    char line[128];
    if (m.count > 0) {
        snprintf(line, sizeof line,
                 "Reloaded -- %d value(s) REFUSED (see `config diff`); "
                 "the previous working value is still in effect for those.", m.count);
        putline(line);
        return 1;
    }
    putline("Reloaded.");
    return 0;
}

static int cmd_files(void) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_FILE_COUNT)) return 1;
    int n = m.count;

    putline("NAME        PATH                   SOURCE    DESCRIPTION");
    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_FILE_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;

        char line[220];
        snprintf(line, sizeof line, "%-11s %-22s %-9s %s%s",
                 m.name, m.file,
                 m.type ? "built-in" : "config.d",
                 m.label,
                 // A descriptor may name a file nothing has written yet.
                 // Reported rather than filtered: hiding it leaves
                 // "where will my settings go?" unanswerable until after
                 // the first save.
                 m.count ? "" : "  (not created yet)");
        putline(line);
    }
    return 0;
}

static int cmd_show(const char *which) {
    struct setting_msg m;
    char path[SETTING_ABI_FILE_MAX];

    // Accept either the umbrella name (`config show system`) or a bare
    // path, so this composes with `config where`.
    path[0] = '\0';
    if (which[0] == '/') {
        strlcpy(path, which, sizeof path);
    } else {
        if (!op(&m, SETTING_OP_FILE_COUNT)) return 1;
        int n = m.count;
        for (int i = 0; i < n; i++) {
            memset(&m, 0, sizeof m);
            m.op = SETTING_OP_FILE_INFO;
            m.index = i;
            if (usetting_dispatch(&m) != 0) continue;
            if (strcmp(m.name, which) != 0) continue;
            strlcpy(path, m.file, sizeof path);
            break;
        }
    }
    if (!path[0]) {
        char line[160];
        snprintf(line, sizeof line,
                 "config: no config file called '%s' -- `config files` lists them", which);
        putline(line);
        return 1;
    }

    static char buf[FILE_MAX];
    int n = read_file(path, buf, sizeof buf);
    if (n < 0) {
        char line[160];
        snprintf(line, sizeof line, "config: %s does not exist yet", path);
        putline(line);
        return 1;
    }
    char head[160];
    snprintf(head, sizeof head, "--- %s ---", path);
    putline(head);
    put(buf);
    if (n > 0 && buf[n - 1] != '\n') put("\n");
    return 0;
}

// Case-insensitive substring, so `config find HELSINKI` finds a
// lowercase city -- the same tolerance tz_find_by_name() has, and for
// the same reason: it is what someone actually types.
static int line_contains(const char *hay, int hay_len, const char *needle) {
    int nl = (int)strlen(needle);
    if (nl == 0 || nl > hay_len) return 0;
    for (int i = 0; i + nl <= hay_len; i++) {
        int j = 0;
        while (j < nl) {
            if (tolower(hay[i + j]) != tolower(needle[j])) break;
            j++;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static int cmd_find(const char *text) {
    struct setting_msg m;
    if (!op(&m, SETTING_OP_FILE_COUNT)) return 1;
    int n = m.count, hits = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_FILE_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;
        if (!m.count) continue; // not created yet

        static char buf[FILE_MAX];
        char path[SETTING_ABI_FILE_MAX];
        strlcpy(path, m.file, sizeof path);
        int len = read_file(path, buf, sizeof buf);
        if (len <= 0) continue; // a directory, or unreadable -- see below

        // Whole LINES, grep-style. Matching keys and values with one
        // search is not a shortcut: it is what makes "which config has
        // this value?" and "which config has this setting name?" the
        // same question, which is how they were asked for.
        int start = 0;
        for (int p = 0; p <= len; p++) {
            if (p != len && buf[p] != '\n') continue;
            int line_len = p - start;
            if (line_len > 0 && line_contains(buf + start, line_len, text)) {
                char out[240];
                int copy = line_len;
                if (copy > (int)sizeof out - 80) copy = (int)sizeof out - 80;
                char frag[160];
                memcpy(frag, buf + start, (size_t)copy);
                frag[copy] = '\0';
                snprintf(out, sizeof out, "%s:%s", path, frag);
                putline(out);
                hits++;
            }
            start = p + 1;
        }
    }
    if (!hits) {
        char line[160];
        snprintf(line, sizeof line, "Nothing matching '%s' in any registered config file.", text);
        putline(line);
        return 1;
    }
    return 0;
}

// --- registering a config file ---------------------------------------
//
// Writing a descriptor file IS the registration -- there is no syscall
// for it, on purpose. A config file is declared by a file, the same way
// an app is declared by a `.desktop` entry, which is what lets a ring-3
// program (the window manager, after Milestone 41) declare its own
// config with no kernel change.

static int descriptor_path(const char *name, char *out, int cap) {
    if (!name[0]) return 0;
    for (const char *p = name; *p; p++) {
        // A name is an identifier, not a path: one that could contain a
        // slash would let `config register` write outside /etc/config.d.
        if (*p == '/' || *p == '.' || *p == ' ') return 0;
    }
    return snprintf(out, (size_t)cap, "/etc/config.d/%s.conf", name) > 0;
}

static int cmd_register(const char *name, const char *path, const char *desc) {
    char dpath[96];
    if (!descriptor_path(name, dpath, sizeof dpath)) {
        putline("config: a name must be a plain word -- no '/', '.' or spaces");
        return 1;
    }
    if (path[0] != '/') {
        putline("config: the path must be absolute");
        return 1;
    }

    char body[320];
    snprintf(body, sizeof body,
             "# Registered by `config register`. Delete this file (or run\n"
             "# `config unregister %s`) to remove it from `config files`.\n"
             "Name=%s\nPath=%s\nDescription=%s\n",
             name, name, path, desc ? desc : "");

    int fd = open(dpath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        putline("config: could not write the descriptor -- does /etc/config.d exist?");
        return 1;
    }
    int64_t w = write(fd, body, strlen(body));
    close(fd);
    if (w < 0) { putline("config: writing the descriptor failed"); return 1; }

    // The kernel rescans on reload, so do it here rather than leaving
    // the user to discover that `config files` has not changed yet.
    struct setting_msg m;
    op(&m, SETTING_OP_RELOAD);

    char line[200];
    snprintf(line, sizeof line, "Registered '%s' -> %s (descriptor: %s)", name, path, dpath);
    putline(line);
    return 0;
}

static int cmd_unregister(const char *name) {
    char dpath[96];
    if (!descriptor_path(name, dpath, sizeof dpath)) {
        putline("config: a name must be a plain word");
        return 1;
    }
    if (unlink(dpath) != 0) {
        char line[200];
        snprintf(line, sizeof line,
                 "config: '%s' has no descriptor in /etc/config.d "
                 "(a BUILT-IN cannot be unregistered -- see `config files`)", name);
        putline(line);
        return 1;
    }
    struct setting_msg m;
    op(&m, SETTING_OP_RELOAD);
    char line[160];
    snprintf(line, sizeof line, "Unregistered '%s'.", name);
    putline(line);
    return 0;
}

// --- usage ------------------------------------------------------------

static const struct uargs_cmd CMDS[] = {
    { 0, 0, "Settings (typed, validated, applied)" },
    { "list",       0,                         "every setting, its value, and its file" },
    { "get",        "[NAME]",                  "one value, or every name under a branch" },
    { "set",        "NAME VALUE",              "validate, apply and persist (also NAME=VALUE)" },
    { "unset",      "NAME",                    "forget it, so the built-in default returns" },
    { "where",      "NAME",                    "just the file holding it" },
    { "diff",       0,                         "settings whose file differs from what is live" },
    { "reload",     0,                         "re-read the files after editing them by hand" },
    { 0, 0, "Config files" },
    { "files",      0,                         "every known config file, and where it came from" },
    { "show",       "NAME|PATH",               "print one, verbatim" },
    { "find",       "TEXT",                    "search names AND values across all of them" },
    { "register",   "NAME PATH [DESCRIPTION]", "make a file known to `config files`" },
    { "unregister", "NAME",                    "forget a registered file" },
    { "help",       0,                         "this page, as -h does" },
    { 0 },
};

static const struct uargs_opt OPTS[] = {
    { "recursive", 'r', 0, "with get: everything under a namespace, not one level", &g_recursive, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "config",
    .opts = OPTS,
    .usage = "COMMAND [ARG]...",
    .summary = "Read, change and find this machine's configuration.",
    .cmds = CMDS,
    .notes = "Settings are ordinary text under /etc -- `edit` them freely, then\n"
             "`config reload`. `config diff` shows what an edit has not applied yet.",
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    // The positionals from here: v[0] is the command.
    char **v = a.argv;
    int n = a.argc;
    if (n == 0 || !strcmp(v[0], "help")) { uargs_help(&PROG); return 0; }
    const char *cmd = v[0];
    if (!strcmp(cmd, "list"))   return cmd_list();
    if (!strcmp(cmd, "diff"))   return cmd_diff();
    if (!strcmp(cmd, "reload")) return cmd_reload();
    if (!strcmp(cmd, "files"))  return cmd_files();

    if (!strcmp(cmd, "get")) {
        int k = 1;
        if (k < n && (!strcmp(v[k], "-r") || !strcmp(v[k], "--recursive"))) { g_recursive = 1; k++; }
        if (k >= n) return cmd_tree("") ? 0 : 1;    // every namespace, one level
        return cmd_get(v[k]);
    }
    if (!strcmp(cmd, "where") && n >= 2) return cmd_where(v[1]);
    if (!strcmp(cmd, "unset") && n >= 2) return cmd_unset(v[1]);
    if (!strcmp(cmd, "show") && n >= 2)  return cmd_show(v[1]);
    if (!strcmp(cmd, "find") && n >= 2)  return cmd_find(v[1]);
    if (!strcmp(cmd, "unregister") && n >= 2) return cmd_unregister(v[1]);

    if (!strcmp(cmd, "register") && n >= 3) {
        // The description is the whole REST of the line, rejoined --
        // it is prose, and the shell split it on spaces. Taking only
        // v[3] silently registered "Settings" for a description
        // someone typed as "Settings for my app".
        char desc[SETTING_ABI_FILE_MAX * 2];
        size_t used = 0;
        desc[0] = '\0';
        for (int i = 3; i < n && used + 1 < sizeof desc; i++) {
            used += snprintf(desc + used, sizeof desc - used, "%s%s",
                             used ? " " : "", v[i]);
        }
        return cmd_register(v[1], v[2], desc);
    }

    if (!strcmp(cmd, "set") && n >= 2) {
        // Both spellings, because both are what people type. `name=value`
        // is split here rather than in the shell so the whole thing can
        // arrive as one argument.
        char name[SETTING_ABI_NAME_MAX];
        const char *eq = 0;
        for (const char *p = v[1]; *p; p++) { if (*p == '=') { eq = p; break; } }
        if (eq) {
            int n = (int)(eq - v[1]);
            if (n >= (int)sizeof name) { putline("config: name too long"); return 1; }
            memcpy(name, v[1], (size_t)n);
            name[n] = '\0';
            return cmd_set(name, eq + 1);
        }
        if (n >= 3) return cmd_set(v[1], v[2]);
        return uargs_error(&PROG, "set needs a value -- set NAME VALUE");
    }

    return uargs_error(&PROG, "'%s' needs more arguments", cmd);
}
