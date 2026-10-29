// See start_store.h.
#include "start_store.h"
#include "wm_conf.h"
#include "wm_log.h"
#include "gui_apps.h"
#include "kapi.h"
#include <stdlib.h>
#include <string.h>

#define START_CONF "/etc/start-menu.conf"

// One row per app the machine has launched, as many as there are, each
// id its own allocation. An id not in the registry any more stays in the
// FILE (an app may come back) but is not read into memory.
struct run_row {
    char *id;
    uint32_t count;
    uint32_t seq;
};

static struct run_row *g_runs;
static int g_run_count, g_run_cap;

static char **g_pins;
static int g_pin_count, g_pin_cap;

// Room for one more in a growing array; 0 when out of memory.
static int grow(void **arr, int *cap, int count, size_t each) {
    if (count < *cap) return 1;
    int n = *cap ? *cap * 2 : 16;
    void *g = realloc(*arr, each * (size_t)n);
    if (!g) return 0;
    *arr = g;
    *cap = n;
    return 1;
}

static int add_pin(const char *id, size_t n) {
    if (!grow((void **)&g_pins, &g_pin_cap, g_pin_count, sizeof *g_pins)) return 0;
    char *c = malloc(n + 1);
    if (!c) return 0;
    k_memcpy(c, id, n);
    c[n] = '\0';
    g_pins[g_pin_count++] = c;
    return 1;
}

static void clear(void) {
    for (int i = 0; i < g_run_count; i++) free(g_runs[i].id);
    for (int i = 0; i < g_pin_count; i++) free(g_pins[i]);
    g_run_count = g_pin_count = 0;
}

// `run.<id>`, allocated: free() it. NULL when out of memory.
static char *run_key(const char *id) {
    size_t n = k_strlen(id) + 5;
    char *k = malloc(n);
    if (k) k_snprintf(k, n, "run.%s", id);
    return k;
}

static uint32_t g_seq;     // the newest sequence number handed out
static int g_loaded;

// --- the file ---------------------------------------------------------

// `pinned` is a WORD LIST rather than one key per pin, because the
// ORDER is the value: a list keeps it in one place, where `pin.3=x`
// would encode it as index numbers that have to be renumbered whenever
// something in the middle is removed.
static void parse_pins(const char *raw) {
    const char *p = raw;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *id = p;
        while (*p && *p != ' ' && *p != ',') p++;
        if (!add_pin(id, (size_t)(p - id))) return;
    }
}

static struct run_row *row_for(const char *app_id, int create) {
    if (!app_id || !app_id[0]) return 0;
    for (int i = 0; i < g_run_count; i++)
        if (k_strcmp(g_runs[i].id, app_id) == 0) return &g_runs[i];
    if (!create || !grow((void **)&g_runs, &g_run_cap, g_run_count, sizeof *g_runs)) return 0;
    char *id = malloc(k_strlen(app_id) + 1);
    if (!id) return 0;
    k_strlcpy(id, app_id, k_strlen(app_id) + 1);
    struct run_row *r = &g_runs[g_run_count++];
    r->id = id;
    r->count = 0;
    r->seq = 0;
    return r;
}

// `run.<id> = <count> <seq>`. Two numbers on one line rather than two
// keys: they are written together, always, and a row that had one
// without the other would be a state this code would then have to have
// an opinion about.
static void parse_run(const char *key, const char *value) {
    struct run_row *r = row_for(key, 1);
    if (!r) return;
    uint32_t count = 0, seq = 0;
    const char *p = value;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') count = count * 10 + (uint32_t)(*p++ - '0');
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') seq = seq * 10 + (uint32_t)(*p++ - '0');
    r->count = count;
    r->seq = seq;
    if (seq > g_seq) g_seq = seq;
}

void start_store_load(void) {
    clear();
    g_seq = 0;
    g_loaded = 1;

    // ONE read, many questions -- etc_config_get() re-reads the whole
    // document per key, and this file has a row per app.
    struct etc_config_buf cfg;
    if (!wm_conf_load(START_CONF, &cfg)) return;   // no file yet is the normal first boot

    char *raw = malloc(cfg.size + 1);   // a value is never longer than its file
    if (raw && etc_config_buf_get(&cfg, "pinned", raw, cfg.size + 1)) parse_pins(raw);
    free(raw);

    // ASKED PER INSTALLED APP, because the config parser answers for a
    // key and does not enumerate them. What that costs is history for
    // an app that is not installed RIGHT NOW: it stays in the file --
    // wm_conf_set() preserves every line it was not asked about -- and
    // comes back with the app, it is simply not in memory meanwhile.
    // Which is the correct behaviour anyway: the menu can only list
    // apps that exist.
    char value[32];
    for (int i = 0; i < gui_app_registry_count; i++) {
        const char *id = gui_app_registry[i].app_id;
        if (!id || !id[0]) continue;
        char *key = run_key(id);
        if (key && etc_config_buf_get(&cfg, key, value, sizeof value)) parse_run(id, value);
        free(key);
    }
    etc_config_buf_free(&cfg);
}

static void write_pins(void) {
    size_t cap = 1;
    for (int i = 0; i < g_pin_count; i++) cap += k_strlen(g_pins[i]) + 1;
    char *list = malloc(cap);
    if (!list) { wm_logf("wm: no memory to write %s\n", START_CONF); return; }
    size_t used = 0;
    for (int i = 0; i < g_pin_count; i++) {
        if (used) list[used++] = ' ';
        size_t n = k_strlen(g_pins[i]);
        k_memcpy(list + used, g_pins[i], n);
        used += n;
    }
    list[used] = '\0';
    if (!wm_conf_set(START_CONF, "pinned", list))
        wm_logf("wm: could not write %s\n", START_CONF);
    free(list);
}

// --- favourites -------------------------------------------------------

int start_store_pin_index(const char *app_id) {
    if (!g_loaded) start_store_load();
    if (!app_id) return -1;
    for (int i = 0; i < g_pin_count; i++)
        if (k_strcmp(g_pins[i], app_id) == 0) return i;
    return -1;
}

int start_store_is_pinned(const char *app_id) {
    return start_store_pin_index(app_id) >= 0;
}

int start_store_pin_count(void) {
    if (!g_loaded) start_store_load();
    return g_pin_count;
}

const char *start_store_pin_at(int n) {
    if (!g_loaded) start_store_load();
    if (n < 0 || n >= g_pin_count) return 0;
    return g_pins[n];
}

void start_store_pin(const char *app_id) {
    if (!app_id || !app_id[0]) return;
    if (start_store_is_pinned(app_id)) return;
    if (!add_pin(app_id, k_strlen(app_id))) return;
    write_pins();
}

void start_store_unpin(const char *app_id) {
    int i = start_store_pin_index(app_id);
    if (i < 0) return;
    free(g_pins[i]);
    for (int j = i; j < g_pin_count - 1; j++) g_pins[j] = g_pins[j + 1];
    g_pin_count--;
    write_pins();
}

// --- launches ---------------------------------------------------------

void start_store_record_launch(const char *app_id) {
    if (!g_loaded) start_store_load();
    struct run_row *r = row_for(app_id, 1);
    if (!r) return;
    r->count++;
    r->seq = ++g_seq;

    char *key = run_key(r->id), value[32];
    k_snprintf(value, sizeof value, "%u %u", r->count, r->seq);
    if (!key || !wm_conf_set(START_CONF, key, value))
        wm_logf("wm: could not record %s in %s\n", r->id, START_CONF);
    free(key);
}

uint32_t start_store_launch_count(const char *app_id) {
    if (!g_loaded) start_store_load();
    struct run_row *r = row_for(app_id, 0);
    return r ? r->count : 0;
}

uint32_t start_store_last_seq(const char *app_id) {
    if (!g_loaded) start_store_load();
    struct run_row *r = row_for(app_id, 0);
    return r ? r->seq : 0;
}

void start_store_forget_launches(void) {
    if (!g_loaded) start_store_load();
    if (!g_run_count) return;
    for (int i = 0; i < g_run_count; i++) {
        char *key = run_key(g_runs[i].id);
        if (key) wm_conf_unset(START_CONF, key);
        free(key);
        free(g_runs[i].id);
    }
    g_run_count = 0;
    g_seq = 0;
}
