// Tab-completion candidate generation -- see api/completion.h for the
// API, for why this is a pure function, and for why every filesystem
// touch goes through `struct completion_env`.
//
// COMPILED TWICE, into the kernel image and into libbase, the same
// as klineedit.c and ansi.c. It must therefore stay freestanding: the
// only includes here are the toolkit's, and memory comes from the env's
// `alloc`/`release` (kmalloc in ring 0, malloc in ring 3).
//
// Structure: split the line into words, decide which "domain" the word
// under the cursor belongs to (commands, a command's argument set, or
// the filesystem), then feed every candidate through add_candidate(),
// which does the prefix filtering and the common-prefix arithmetic in
// one place.
#include "completion.h"
#include "string.h"

// What `insert` points at when there is nothing to type: never freed.
static char g_none[1];

// ---- candidate collection ----

struct completion_collector {
    const struct completion_env *env;
    struct completion_result *out;
    const char *prefix;
    uint32_t prefix_len;
    uint32_t common;   // bytes of candidates[0] that every candidate shares
};

void completion_result_free(const struct completion_env *env, struct completion_result *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) env->release(r->candidates[i]);
    if (r->candidates) env->release(r->candidates);
    if (r->insert && r->insert != g_none) env->release(r->insert);
    r->candidates = 0;
    r->count = r->cap = r->truncated = r->add_space = 0;
    r->insert = g_none;
}

static void collector_init(struct completion_collector *c, const struct completion_env *env,
                           struct completion_result *out, const char *prefix) {
    completion_result_free(env, out);
    c->env = env;
    c->out = out;
    c->prefix = prefix;
    c->prefix_len = (uint32_t)k_strlen(prefix);
    c->common = 0;
}

// Adds head + tail + suffix as a candidate if `match` starts with the
// collector's prefix.
//
// The two differ for paths: the prefix being matched is just the
// filename part ("ti"), while what the caller has to insert is the whole
// word ("/etc/timezones"). Comparing the full text against the filename
// prefix was the bug the first live test caught -- `cat /etc/ti<TAB>`
// silently found nothing.
//
// OUT OF MEMORY DROPS THE CANDIDATE AND SETS `truncated`, which every
// caller shows as "more matches not shown".
static void add_candidate_parts(struct completion_collector *c, const char *match,
                                const char *head, const char *tail, const char *suffix) {
    if (k_strncmp(match, c->prefix, (int)c->prefix_len) != 0) return;
    struct completion_result *out = c->out;

    // A duplicate is stored and dropped after the sort (collector_finish),
    // so a big folder costs n log n, not n squared.
    if (out->count == out->cap) {
        int cap = out->cap ? out->cap * 2 : 16;
        char **grown = cap > out->cap ? c->env->alloc((uint32_t)cap * (uint32_t)sizeof *grown) : 0;
        if (!grown) { out->truncated = 1; return; }
        for (int i = 0; i < out->count; i++) grown[i] = out->candidates[i];
        if (out->candidates) c->env->release(out->candidates);
        out->candidates = grown;
        out->cap = cap;
    }
    size_t hl = k_strlen(head), tl = k_strlen(tail), sl = k_strlen(suffix);
    char *s = c->env->alloc((uint32_t)(hl + tl + sl + 1));
    if (!s) { out->truncated = 1; return; }
    k_memcpy(s, head, hl);
    k_memcpy(s + hl, tail, tl);
    k_memcpy(s + hl + tl, suffix, sl + 1);

    if (out->count == 0) {
        c->common = (uint32_t)(hl + tl + sl);
    } else {
        uint32_t i = 0;
        while (i < c->common && out->candidates[0][i] == s[i]) i++;
        c->common = i;
    }
    out->candidates[out->count++] = s;
}

static void add_candidate(struct completion_collector *c, const char *name) {
    add_candidate_parts(c, name, "", name, "");
}

void completion_add(struct completion_collector *c, const char *name) {
    add_candidate(c, name);
}

// Heapsort of the pointers: in place and n log n, since a folder may
// hold thousands of matches and the list has no cap.
static void sift_down(char **a, int root, int n) {
    for (;;) {
        int child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && k_strcmp(a[child], a[child + 1]) < 0) child++;
        if (k_strcmp(a[root], a[child]) >= 0) return;
        char *t = a[root]; a[root] = a[child]; a[child] = t;
        root = child;
    }
}

static void sort_candidates(struct completion_result *out) {
    char **a = out->candidates;
    int n = out->count;
    for (int i = n / 2 - 1; i >= 0; i--) sift_down(a, i, n);
    for (int end = n - 1; end > 0; end--) {
        char *t = a[0]; a[0] = a[end]; a[end] = t;
        sift_down(a, 0, end);
    }
}

// A NAME OFFERED TWICE IS OFFERED ONCE. Several domains overlap by
// design -- a name can be both a builtin and a program in PATH -- and
// without this each is listed twice. Sorted, a duplicate is a neighbour.
static void drop_duplicates(const struct completion_env *env, struct completion_result *out) {
    int w = 0;
    for (int i = 0; i < out->count; i++) {
        if (w > 0 && !k_strcmp(out->candidates[w - 1], out->candidates[i])) {
            env->release(out->candidates[i]);
            continue;
        }
        out->candidates[w++] = out->candidates[i];
    }
    out->count = w;
}

// Turns the common prefix into what to actually TYPE: the part of it the
// user has not typed already. Every candidate shares it, so it is read
// off any one of them.
static void collector_finish(struct completion_collector *c) {
    struct completion_result *out = c->out;
    out->insert = g_none;
    if (out->count == 0) return;

    // Fewer agreed bytes than are typed can only happen when the prefix
    // being matched is not the prefix being replaced (the path case).
    if (c->common > c->prefix_len) {
        uint32_t extra = c->common - c->prefix_len;
        char *ins = c->env->alloc(extra + 1);
        if (ins) {
            k_memcpy(ins, out->candidates[0] + c->prefix_len, extra);
            ins[extra] = '\0';
            out->insert = ins;
        } else {
            out->truncated = 1;   // nothing typed, and no space after it either
        }
    }
    sort_candidates(out);
    drop_duplicates(c->env, out);

    if (out->count == 1 && !out->truncated) {
        size_t clen = k_strlen(out->candidates[0]);
        out->add_space = (clen > 0 && out->candidates[0][clen - 1] == '/') ? 0 : 1;
    }
}

// Copies `n` bytes of `s` into a fresh string from the env, or NULL.
static char *dup_n(const struct completion_env *env, const char *s, size_t n) {
    char *d = env->alloc((uint32_t)n + 1);
    if (!d) return 0;
    k_memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

// ---- filesystem candidates ----

// The listing callback takes no context pointer -- it matches ring 0's
// fs_list(), which has none -- so the in-flight collector is reachable
// from a file-scope pointer. Safe because nothing re-enters completion
// mid-listing, and NOT for the reason it looks like. This file is
// compiled TWICE, so each ring has its own copy of these statics: the
// kernel's is reached only from kernel/shell/shell.c (the rescue shell, on no
// syscall path), and ring 3's lives in one single-threaded process's
// own address space. A ring-3 process preempting the rescue shell
// mid-Tab therefore cannot touch the statics it is standing on --
// which is what keeps this true now a syscall can be preempted.
static struct completion_collector *g_active;
static const char *g_dir_prefix; // the part of the word before the last '/'
static int g_skip_dirs;          // walking PATH: a directory is not a command
static int g_only_dirs;          // completing `cd`: a file is not a directory

static void list_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    // A DIRECTORY IS NEVER A COMMAND. Walking PATH used to offer
    // /bin/wm as the candidate `wm/`, which cannot be a first word.
    if (g_skip_dirs && is_dir) return;
    if (g_only_dirs && !is_dir) return;
    // The filename prefix has no '/' (the word was split at its last
    // one), so matching the bare name is matching "name/" too. What is
    // stored is the directory part typed, the name, and a '/' for a
    // directory so the next Tab descends.
    add_candidate_parts(g_active, name, g_dir_prefix, name, is_dir ? "/" : "");
}

// Completes `word` as a path. The word is split at its last '/': the
// part before it names the directory to list (resolved against the
// shell's cwd), the part after it is the prefix to match. Candidates
// come back as whole words (directory part included) so the caller can
// treat them uniformly with every other domain.
static void complete_path(const struct completion_env *env,
                           struct completion_collector *c, const char *word,
                           int dirs_only) {
    int last_slash = -1;
    for (int i = 0; word[i]; i++) if (word[i] == '/') last_slash = i;

    // dlen keeps the leading '/' for a root-relative word.
    int dlen = last_slash < 0 ? 0 : last_slash == 0 ? 1 : last_slash;
    char *dir_part = dup_n(env, word, (size_t)dlen);
    char *dir_prefix = dup_n(env, word, (size_t)(last_slash + 1));
    char *resolved = env->alloc(COMPLETION_PATH_MAX);
    if (!dir_part || !dir_prefix || !resolved) {
        c->out->truncated = 1;
        goto out;
    }
    if (!env->resolve(dir_part[0] ? dir_part : 0, resolved, COMPLETION_PATH_MAX)) goto out;
    if (!env->is_dir(resolved)) goto out;

    // Re-point the collector's prefix at the filename part only; the
    // callback prepends dir_prefix so candidates stay whole words.
    const char *file_prefix = last_slash < 0 ? word : word + last_slash + 1;
    struct completion_collector inner = *c;
    inner.prefix = file_prefix;
    inner.prefix_len = (uint32_t)k_strlen(file_prefix);

    g_active = &inner;
    g_dir_prefix = dir_prefix;
    g_only_dirs = dirs_only;
    env->list_dir(resolved, list_cb);
    g_only_dirs = 0;
    g_active = 0;

    // add_candidate() matched against the filename part, but the caller
    // is replacing the whole word -- so the prefix the common-prefix
    // arithmetic must subtract is the whole word, not just the filename.
    *c = inner;
    c->prefix = word;
    c->prefix_len = (uint32_t)k_strlen(word);
out:
    if (dir_part) env->release(dir_part);
    if (dir_prefix) env->release(dir_prefix);
    if (resolved) env->release(resolved);
}

// Everything a bare name can resolve to: whatever the env declares as
// non-file names, then every executable in every PATH directory.
//
// A name appearing in more than one PATH directory is offered once --
// add_candidate_parts() dedupes. That is not the same as PATH order
// becoming irrelevant: the FIRST match still wins at run time, and
// completion is only saying the name exists.
static void complete_executables(const struct completion_env *env,
                                  struct completion_collector *c) {
    if (env->extra_count && env->extra_name)
        for (int i = 0, n = env->extra_count(); i < n; i++)
            add_candidate(c, env->extra_name(i));

    if (!env->path_count || !env->path_dir) return;

    struct completion_collector inner = *c;
    g_active = &inner;
    g_dir_prefix = g_none;
    g_skip_dirs = 1;
    for (int i = 0, n = env->path_count(); i < n; i++) {
        const char *dir = env->path_dir(i);
        if (dir && env->is_dir(dir)) env->list_dir(dir, list_cb);
    }
    g_skip_dirs = 0;
    g_active = 0;
    *c = inner;
}

// ---- entry point ----

int completion_run_env(const struct completion_env *env, const char *line,
                        int cursor, struct completion_result *out) {
    struct completion_collector c;

    if (cursor < 0) cursor = 0;
    // Find the word under the cursor: back up to the previous space.
    int start = cursor;
    while (start > 0 && line[start - 1] != ' ') start--;

    char *word = dup_n(env, line + start, (size_t)(cursor - start));

    // Which argument is this? Count the words before it, skipping runs
    // of spaces so "cat   /et<TAB>" behaves like "cat /et<TAB>".
    char *cmd = 0;
    int arg_index = 0;
    {
        int i = 0, words = 0;
        while (i < start) {
            while (i < start && line[i] == ' ') i++;
            if (i >= start) break;
            int wstart = i;
            while (i < start && line[i] != ' ') i++;
            if (words == 0) cmd = dup_n(env, line + wstart, (size_t)(i - wstart));
            words++;
        }
        arg_index = words; // 0 = the command itself, 1 = first argument, ...
    }

    collector_init(&c, env, out, word ? word : "");
    if (!word || (arg_index > 0 && !cmd)) {
        out->truncated = 1;   // out of memory: nothing offered, and said so
    } else if (arg_index == 0) {
        if (env->commands)
            for (int i = 0; env->commands[i]; i++) add_candidate(&c, env->commands[i]);
        complete_executables(env, &c);
    } else {
        enum completion_domain d = COMPLETION_PATHS;
        if (env->arg_domain) d = env->arg_domain(&c, cmd, arg_index);
        if (d == COMPLETION_PATHS)    complete_path(env, &c, word, 0);
        else if (d == COMPLETION_DIRS) complete_path(env, &c, word, 1);
        // FILLED and NONE both leave the collector as the env left it.
    }

    collector_finish(&c);
    if (word) env->release(word);
    if (cmd) env->release(cmd);
    return out->count;
}
