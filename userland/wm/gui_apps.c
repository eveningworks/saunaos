#include "gui_apps.h"
#include "kapi.h"
#include "etc_config.h"
#include "rt/sys.h"
#include "lib/ulistdir.h"
#include "wm/wm_log.h"
#include "wm/wm_conf.h"
#include "kpath.h"   // k_path_basename, for AppId's default
#include <stdlib.h>

// The Start menu and the desktop icons are built from DATA ON DISK --
// one file per app in /usr/wm/applications/, scanned at desktop startup.
// Adding an app to the desktop is dropping a file there; it is not
// editing this table and rebuilding the kernel.
//
// The format is `name=value` with `#` comments, read through
// kernel/lib/etc_config.c -- the same parser /etc/saunaos.conf uses,
// because a second config format to maintain would buy nothing. It is a
// deliberate small subset of freedesktop.org's .desktop files: same
// idea, same key names where they overlap, none of the localisation or
// MIME machinery that has nothing to attach to here. See
// data/wm/applications/README.md.
//
// TWO KINDS OF ENTRY, and the reason the file format has to know:
//
//   Exec=/bin/wm/apps/calculator   spawn that binary; the process makes
//                                  its own window over TWP.
//
// There is no second form any more. `Exec=builtin:<name>` named a
// kernel-space app whose callbacks were compiled in; it was removed
// when Control Panel -- the last one -- became a ring-3 binary
// (Milestone 41 stage 4's prerequisite), which is exactly what the
// format was shaped to allow without any reader of it changing. An
// entry still carrying `builtin:` now names no binary and is refused
// by the loader below like any other bad path.

#define DESKTOP_DIR "/usr/wm/applications"

// The live registry, rebuilt whole by gui_apps_load() (gui_apps.h).
// Each entry's strings are its own gui_app_entry's, kept beside it.
struct gui_app *gui_app_registry;
int gui_app_registry_count;
static struct gui_app_entry *g_entries;

int wm_app_key(const char *path, const char *id, char *out, int cap) {
    if (!path || !path[0]) return k_snprintf(out, (size_t)cap, "%s", id ? id : "");
    // The program alone: an Exec line's arguments are not its identity.
    int n = 0;
    while (path[n] && path[n] != ' ') n++;
    const char *base = path;
    for (int i = 0; i < n; i++)
        if (path[i] == '/') base = path + i + 1;
    int blen = n - (int)(base - path);
    if (id && id[0] && !((int)k_strlen(id) == blen && k_strncmp(id, base, (size_t)blen) == 0))
        return k_snprintf(out, (size_t)cap, "%.*s:%s", n, path, id);
    return k_snprintf(out, (size_t)cap, "%.*s", n, path);
}

// THE FOLDERS THE START MENU SHOWS, in order, and what each key is
// called on screen. freedesktop's registered category names minus the
// ones nothing here uses; `apps` and `demos` are what this file said
// before folders existed and stay as aliases so an entry written
// against the old README still lands somewhere sensible.
static const struct { const char *key, *label; } g_cat_table[] = {
    { "utility",     "Utilities" },
    { "graphics",    "Graphics" },
    { "network",     "Internet" },      // freedesktop's Network, GNOME's word for it
    { "multimedia",  "Multimedia" },
    { "games",       "Games" },
    { "system",      "System" },
    { "development", "Development" },
    { "apps",        "Applications" },
    { "demos",       "Demos" },
};
#define CAT_TABLE_N ((int)(sizeof g_cat_table / sizeof g_cat_table[0]))

// An unknown category sorts last rather than being dropped -- a typo
// should show up as a misplaced row, not as an app that silently
// vanished.
static int cat_rank(const char *c) {
    for (int i = 0; i < CAT_TABLE_N; i++)
        if (k_strcmp(c, g_cat_table[i].key) == 0) return i;
    return CAT_TABLE_N;
}

const char *gui_app_cat_label_for(const char *key) {
    for (int i = 0; i < CAT_TABLE_N; i++)
        if (k_strcmp(key, g_cat_table[i].key) == 0) return g_cat_table[i].label;
    return key;
}

// An entry's ShowIn= key -> GUI_SHOW_* bits. Absent means BOTH, which is
// what every entry meant before this key existed.
//
// Words are separated by spaces or commas: `ShowIn=desktop startmenu`.
//
// A key naming nothing recognisable falls back to BOTH and says so,
// rather than rejecting the way this project's parsers normally do. The
// usual rule ("a parser rejects rather than guesses") assumes the two
// outcomes are a value and an error; here the failure directions are
// asymmetric. Hiding an app because its ShowIn= was misspelled makes it
// vanish from the desktop with no visible cause, which is the single
// worst thing this file can do -- an unreachable app looks like a broken
// system (see wm_reap_launched()'s comment for the last time that
// happened). Showing it in both places with a log line is recoverable.
// A `.desktop` key, from `[Desktop Entry]` or from the top level.
//
// The entries shipped here carry no header; freedesktop REQUIRES one,
// so a file copied off a Linux box opens with `[Desktop Entry]`.
// Reading both is what lets either drop in -- see etc_config.h.
static int entry_get(const struct etc_config_buf *cfg, const char *key,
                     char *out, uint32_t cap) {
    return etc_config_buf_get_in_or_top(cfg, "Desktop Entry", key, out, cap);
}

static unsigned parse_show_in(const struct etc_config_buf *cfg, const char *file) {
    char raw[32];
    if (!entry_get(cfg, "ShowIn", raw, sizeof raw)) return GUI_SHOW_ALL;

    unsigned bits = 0;
    const char *p = raw;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        const char *word = p;
        while (*p && *p != ' ' && *p != ',') p++;
        int len = (int)(p - word);

        if (len == 7 && k_strncmp(word, "desktop", 7) == 0) {
            bits |= GUI_SHOW_DESKTOP;
        } else if (len == 9 && k_strncmp(word, "startmenu", 9) == 0) {
            bits |= GUI_SHOW_STARTMENU;
        } else {
            wm_logf("wm: %s has an unknown ShowIn word -- ignored\n", file);
        }
    }

    if (!bits) {
        wm_logf("wm: %s names no valid ShowIn surface -- showing on both\n", file);
        return GUI_SHOW_ALL;
    }
    return bits;
}

// Every value of one entry, back to back, before they are copied into
// the entry's own allocation. No value can be longer than the file, so
// three files' worth holds them all -- the AppId default and the key
// repeat Exec's path. Grown to the largest entry read, never shrunk:
// the WM is one event loop and reads entries one at a time.
static char *g_vals;
static unsigned g_vals_cap, g_used;

static int vals_reserve(uint32_t file_size) {
    uint64_t want = 3ull * ((uint64_t)file_size + 1) + 64;   // + put()'s short defaults
    if (want <= g_vals_cap) return 1;
    if (want > 0xFFFFFFFFull) return 0;
    char *g = realloc(g_vals, (size_t)want);
    if (!g) {
        wm_logf("wm: out of memory reading a %u-byte desktop entry\n", (unsigned)file_size);
        return 0;
    }
    g_vals = g;
    g_vals_cap = (unsigned)want;
    return 1;
}

// The value of `key` stored in g_vals; its offset, or -1 when absent.
static int val(const struct etc_config_buf *cfg, const char *key) {
    unsigned at = g_used;
    if (!entry_get(cfg, key, g_vals + at, (uint32_t)(g_vals_cap - at))) return -1;
    g_used = at + (unsigned)k_strlen(g_vals + at) + 1;
    return (int)at;
}

static int put(const char *v) {
    unsigned at = g_used, n = (unsigned)k_strlen(v);
    if (at + n + 1 > g_vals_cap) return -1;
    k_memcpy(g_vals + at, v, n + 1);
    g_used = at + n + 1;
    return (int)at;
}

void gui_app_entry_free(struct gui_app_entry *e) {
    free(e->mem);
    k_memset(e, 0, sizeof *e);
}

static int read_entry(const struct etc_config_buf *cfg, const char *path,
                      struct gui_app_entry *e);

int gui_app_read_entry(const char *path, struct gui_app_entry *e) {
    k_memset(e, 0, sizeof *e);
    // ONE read, many questions: etc_config_get() re-reads the whole file
    // per key, and a reload runs on every filesystem change.
    struct etc_config_buf cfg;
    if (!wm_conf_load(path, &cfg)) return 0;
    int ok = vals_reserve(cfg.size) && read_entry(&cfg, path, e);
    etc_config_buf_free(&cfg);
    return ok;
}

static int read_entry(const struct etc_config_buf *cfg, const char *path,
                      struct gui_app_entry *e) {
    const char *file = k_path_basename(path);

    char nodisplay[8], raw[16];
    if (entry_get(cfg, "NoDisplay", nodisplay, sizeof nodisplay) && nodisplay[0] == '1')
        return 0;
    g_used = 0;
    int name = val(cfg, "Name"), exec = val(cfg, "Exec");
    if (name < 0 || exec < 0) return 0;
    // Every app is a ring-3 binary now; the retired `builtin:` form is
    // refused LOUDLY rather than shown as a row that does nothing.
    if (k_strncmp(g_vals + exec, "builtin:", 8) == 0) {
        wm_logf("wm: %s uses the retired builtin: form -- ignored\n", file);
        return 0;
    }
    int cat = val(cfg, "Category");
    if (cat < 0) cat = put("utility");
    // OPTIONAL, and empty is a normal answer: an app with nothing to
    // say about itself shows its name alone rather than a placeholder.
    int comment = val(cfg, "Comment");
    if (comment < 0) comment = put("");
    int handles = val(cfg, "Handles");
    if (handles < 0) handles = put("");
    // A one-character Icon= is a GLYPH, anything longer is a NAME. Both
    // are kept; drawing prefers the file and falls back to the character.
    int icon = val(cfg, "Icon");
    if (icon < 0) icon = put("");
    e->glyph = g_vals[icon];
    if (g_vals[icon] && !g_vals[icon + 1]) icon = put("");
    // AppId defaults to the basename of Exec, which is right everywhere
    // but Shapes -- see gui_apps.h on why the entry gets to say.
    int appid = val(cfg, "AppId");
    if (appid < 0) appid = put(k_path_basename(g_vals + exec));
    unsigned room = g_vals_cap - g_used;
    int key = (int)g_used;
    unsigned klen = (unsigned)wm_app_key(g_vals + exec, g_vals + appid, g_vals + key, (int)room);
    if (cat < 0 || comment < 0 || handles < 0 || icon < 0 || appid < 0 || klen >= room) return 0;
    g_used += klen + 1;

    e->mem = malloc(g_used);
    if (!e->mem) return 0;
    k_memcpy(e->mem, g_vals, g_used);
    e->name = e->mem + name;
    e->exec = e->mem + exec;
    e->category = e->mem + cat;
    e->comment = e->mem + comment;
    e->handles = e->mem + handles;
    e->icon = e->mem + icon;
    e->app_id = e->mem + appid;
    e->key = e->mem + key;
    // DEFAULT ON, so only `RememberGeometry=false` turns it off: a typo
    // is harmless rather than silently disabling a behaviour.
    e->remember_geometry = 1;
    if (entry_get(cfg, "RememberGeometry", raw, sizeof raw) &&
        (k_strcmp(raw, "false") == 0 || k_strcmp(raw, "0") == 0 || k_strcmp(raw, "no") == 0))
        e->remember_geometry = 0;
    e->show_in = parse_show_in(cfg, file);
    return 1;
}

// A cheap answer to "could anything in this directory have changed?",
// costing ONE directory listing and no per-file reads.
//
// The desktop reloads when its directory watch fires (wm_watch.h). That
// was the GLOBAL fs generation -- it bumps for any write anywhere, so
// saving a setting made
// the desktop re-read and re-parse every .desktop file. Under KVM each
// of those reads competes with whatever ring-3 app just did the saving,
// and the reload measured 2.5 SECONDS with the desktop frozen for all
// of it. Almost every one of those reloads had nothing to reload: the
// directory was byte-for-byte what it already was.
//
// WHAT IT CANNOT SEE: an edit that leaves a file's SIZE unchanged, and
// leaves the set of names unchanged -- fs_list() reports names and
// sizes, and reading each file to do better is the cost this exists to
// avoid. The live reload is a convenience (the alternative was no live
// reload at all), so missing a same-size edit until the next real
// change is the right trade against freezing the desktop on every
// unrelated write. fs_stat()'s mtime would close it at the price of a
// stat per entry, which is the same shape of cost again.
static uint64_t g_fp;

static void fingerprint_cb(const char *name, uint32_t size, int is_dir) {
    if (is_dir) return;
    // FNV-1a over the name, then fold in the size. Order-independent
    // addition would collide on a swap; this is order-DEPENDENT, which
    // is fine because fs_list() walks in stable table order.
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)size * 2654435761ULL;
    g_fp = (g_fp * 31) + h;
}

uint64_t gui_apps_dir_fingerprint(void) {
    g_fp = 1;
    struct sys_dirent *ents;
    int n = ulistdir(DESKTOP_DIR, &ents);
    for (int i = 0; i < n; i++) fingerprint_cb(ents[i].name, ents[i].size, (int)ents[i].is_dir);
    free(ents);
    return g_fp;
}

static int is_entry_file(const struct sys_dirent *d) {
    // Only *.desktop, so a README or an editor's leftover is ignored
    // rather than parsed into a blank menu row.
    int n = (int)k_strlen(d->name);
    return !d->is_dir && n >= 9 && k_strcmp(d->name + n - 8, ".desktop") == 0;
}

// (category rank, name): the Start menu's row order is something tests
// and users both read, and "whatever order the filesystem felt like" is
// not an order.
static int entry_before(const struct gui_app_entry *a, const struct gui_app_entry *b) {
    int ra = cat_rank(a->category), rb = cat_rank(b->category);
    return ra != rb ? ra < rb : k_strcmp(a->name, b->name) < 0;
}

void gui_apps_load(void) {
    struct sys_dirent *ents;
    int n = ulistdir(DESKTOP_DIR, &ents);
    struct gui_app_entry *entries = n > 0 ? malloc(sizeof *entries * (size_t)n) : 0;
    struct gui_app *apps = n > 0 ? malloc(sizeof *apps * (size_t)n) : 0;
    if (n > 0 && (!entries || !apps)) {
        // Out of memory: keep the registry that is already there.
        wm_logf("wm: no memory to reload " DESKTOP_DIR "\n");
        free(entries);
        free(apps);
        free(ents);
        return;
    }

    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!is_entry_file(&ents[i])) continue;
        char path[sizeof DESKTOP_DIR + sizeof ents[i].name + 1];
        k_snprintf(path, sizeof path, "%s/%s", DESKTOP_DIR, ents[i].name);
        uint64_t t = sys_ticks();
        if (gui_app_read_entry(path, &entries[count])) count++;
        uint64_t d = sys_ticks() - t;
        if (d * 10 >= 100)
            wm_logf("wm: entry %s took %u ms\n", ents[i].name, (uint32_t)(d * 10));
    }
    free(ents);

    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && entry_before(&entries[j], &entries[j - 1]); j--) {
            struct gui_app_entry t = entries[j];
            entries[j] = entries[j - 1];
            entries[j - 1] = t;
        }

    for (int i = 0; i < count; i++) {
        const struct gui_app_entry *e = &entries[i];
        struct gui_app *a = &apps[i];
        k_memset(a, 0, sizeof *a);
        a->name = e->name;
        a->icon = e->glyph;
        a->icon_name = e->icon;
        a->app_id = e->app_id;
        a->key = e->key;
        a->category = e->category;
        a->comment = e->comment;
        a->handles = e->handles;
        a->resizable = 1;
        a->remember_geometry = e->remember_geometry;
        a->show_in = e->show_in;
        a->exec_path = e->exec;
    }

    for (int i = 0; i < gui_app_registry_count; i++) gui_app_entry_free(&g_entries[i]);
    free(g_entries);
    free(gui_app_registry);
    g_entries = entries;
    gui_app_registry = apps;
    gui_app_registry_count = count;

    if (count == 0) {
        // No entries: a disk without /usr/wm, or a RAM-only boot with
        // nothing seeded. Say so -- an empty Start menu with no
        // explanation looks like the desktop is broken, and this is the
        // one message that distinguishes "no apps installed" from it.
        sys_eprint("wm: no desktop entries in " DESKTOP_DIR " -- Start menu is empty\n");
    } else {
        wm_logf("wm: %d desktop entries from " DESKTOP_DIR "\n", count);
    }
}

// --- per-surface views of the registry (see gui_apps.h) ---------------
//
// A linear scan per call, which is the right trade at this size: the
// registry holds a dozen entries, the callers are a menu draw and a hit
// test, and the alternative -- a cached filtered index array -- is a
// second copy of the registry that can go stale when gui_apps_load()
// re-runs. Correct by construction beats fast at eleven items.

int gui_app_shows_in(const struct gui_app *app, unsigned surface) {
    return app && (app->show_in & surface) != 0;
}

int gui_app_visible_count(unsigned surface) {
    int n = 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (gui_app_shows_in(&gui_app_registry[i], surface)) n++;
    }
    return n;
}

struct gui_app *gui_app_visible_at(unsigned surface, int n) {
    if (n < 0) return 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (!gui_app_shows_in(&gui_app_registry[i], surface)) continue;
        if (n-- == 0) return &gui_app_registry[i];
    }
    return 0; // out of range: a no-op, never a clamp onto the last entry
}

// --- the category view (see gui_apps.h) -------------------------------

// The distinct categories among the entries `surface` shows: how many,
// and the `want`th in *out (want -1: count only). The registry is
// already sorted by (rank, name), so this comes out in display order; a
// category counts at its FIRST entry, deduped against every earlier
// entry rather than the previous one, because two different unknown keys
// share a rank and interleave.
static int cat_list(unsigned surface, const char **out, int want) {
    int n = 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        struct gui_app *a = &gui_app_registry[i];
        if (!gui_app_shows_in(a, surface) || !a->category) continue;
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) {
            struct gui_app *b = &gui_app_registry[j];
            dup = gui_app_shows_in(b, surface) && b->category && k_strcmp(b->category, a->category) == 0;
        }
        if (dup) continue;
        if (n == want) *out = a->category;
        n++;
    }
    return n;
}

struct gui_app *gui_app_by_id(unsigned surface, const char *app_id) {
    if (!app_id || !app_id[0]) return 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        struct gui_app *a = &gui_app_registry[i];
        if (!gui_app_shows_in(a, surface) || !a->app_id) continue;
        if (k_strcmp(a->app_id, app_id) == 0) return a;
    }
    return 0;
}

int gui_app_cat_count(unsigned surface) {
    return cat_list(surface, 0, -1);
}

const char *gui_app_cat_key(unsigned surface, int n) {
    if (n < 0) return 0;
    const char *key = 0;
    cat_list(surface, &key, n);
    return key;
}

const char *gui_app_cat_label(unsigned surface, int n) {
    const char *key = gui_app_cat_key(surface, n);
    return key ? gui_app_cat_label_for(key) : 0;
}

int gui_app_cat_size(unsigned surface, const char *key) {
    if (!key) return 0;
    int n = 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        struct gui_app *a = &gui_app_registry[i];
        if (gui_app_shows_in(a, surface) && a->category &&
            k_strcmp(a->category, key) == 0) n++;
    }
    return n;
}

struct gui_app *gui_app_cat_at(unsigned surface, const char *key, int n) {
    if (!key || n < 0) return 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        struct gui_app *a = &gui_app_registry[i];
        if (!gui_app_shows_in(a, surface) || !a->category) continue;
        if (k_strcmp(a->category, key) != 0) continue;
        if (n-- == 0) return a;
    }
    return 0;
}
