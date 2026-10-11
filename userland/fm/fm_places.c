// The left pane's places and their menus: which places it lists (the
// named folders, minus any hidden; the pins; the volumes and shares),
// and the context menu a right-click on any row of it opens -- one per
// KIND of row, Explorer's navigation pane and Dolphin's Places panel:
// the open verbs, that row's own verbs, Properties last.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include "lib/upins.h"
#include "lib/uvolinfo.h"
#include "lib/udirs.h"
#include "ui/uui_verbs.h"
#include "lib/uconf.h"
#include "lib/uclip.h"
#include "lib/ulaunch.h"
#include "ui/ulog.h"
#include "userfs_abi.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#define DISKS_EXEC    "/bin/wm/apps/disks"
#define SETTINGS_EXEC "/bin/wm/system/settings"
#define HIDDEN_KEY    "hidden_places"

// The named places, in the order listed: Recent, Home, the personal
// folders (lib/udirs.h, asked by name), the Recycle Bin. `key` is what
// files.conf's hidden_places names, so a label can change without
// unhiding anything; a personal folder's path is filled in at build.
static struct { const char *key, *label, *icon; const char *path; int udir; } NAMED[] = {
    { "recent", "Recent", "place-recent", FM_RECENT, -1 },
    { "home",   "Home",   "place-home",   "/home",   -1 },
    { 0, 0, 0, 0, UDIR_DESKTOP },
    { 0, 0, 0, 0, UDIR_DOCUMENTS },
    { 0, 0, 0, 0, UDIR_MUSIC },
    { 0, 0, 0, 0, UDIR_PICTURES },
    { 0, 0, 0, 0, UDIR_VIDEOS },
    { 0, 0, 0, 0, UDIR_DOWNLOADS },
    { "bin", "Recycle Bin", "place-trash", FM_BIN, -1 },
};
#define N_NAMED ((int)(sizeof NAMED / sizeof NAMED[0]))

static void named_fill(void) {
    for (int i = 0; i < N_NAMED; i++) {
        if (NAMED[i].udir < 0) continue;
        enum udir d = (enum udir)NAMED[i].udir;
        NAMED[i].key = udir_key(d);
        NAMED[i].label = udir_label(d);
        NAMED[i].icon = udir_icon(d);
        NAMED[i].path = udir_path(d);
    }
}

// Which named places are hidden: a comma list in files.conf.
static int is_hidden(const char *list, const char *key) {
    size_t n = strlen(key);
    for (const char *p = list; *p; ) {
        if (!strncmp(p, key, n) && (p[n] == ',' || p[n] == '\0')) return 1;
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return 0;
}

static void hidden_list(char *out, int cap) {
    out[0] = '\0';
    uconf_get(FILES_CONF, HIDDEN_KEY, out, (uint32_t)cap);
}

static int named_of(const char *path) {
    named_fill();
    for (int i = 0; i < N_NAMED; i++) if (!strcmp(NAMED[i].path, path)) return i;
    return -1;
}

void places_build(void) {
    char hidden[128];
    hidden_list(hidden, sizeof hidden);
    named_fill();
    uui_places_init(&g_places);
    for (int i = 0; i < N_NAMED; i++)
        if (!is_hidden(hidden, NAMED[i].key))
            uui_places_add(&g_places, NAMED[i].label, NAMED[i].icon, NAMED[i].path);
    uui_places_add_pins(&g_places);
    uui_places_refresh(&g_places);
}

// A TREE ROW WHOSE FOLDER HAS GONE -- a pin to a folder deleted since
// the tree was built, most often -- is said so, and the pane stays where
// it was: set_dir() has already taken the path when the listing fails.
// The rebuild drops it from Pinned (lib/upins.h keeps the pin for when
// the folder is back).
void tree_goto(const char *path) {
    char was[PATH_MAX_LEN], gone[PATH_MAX_LEN];
    strlcpy(was, uui_fileview_dir(active()), sizeof was);
    strlcpy(gone, path, sizeof gone);   // `path` is a tree slot the rebuild rewrites
    if (fm_goto(g_active, gone)) {
        // A volume or share entered from here is described AS ONE by the
        // details pane (fm_details.c), which needs the folder itself in
        // view rather than its first row.
        struct uvolinfo v;
        if (g_dpane && uvolinfo_load(gone, &v)) uui_fileview_select_none(active());
        return;
    }
    uui_fileview_set_dir(active(), was);
    snprintf(g_stat_note, sizeof g_stat_note, "%s is not there any more -- moved or deleted",
             k_path_basename(gone));
    places_build();
    tree_rebuild();
}

// --- the menus --------------------------------------------------------------

enum kind { K_NONE, K_PANE, K_NAMED, K_RECENT, K_BIN, K_PIN, K_VOLUME, K_SHARE, K_FOLDER, K_ADD };
static const char *const KIND_NAME[] = { "none", "pane", "place", "recent", "bin", "pin",
                                         "volume", "share", "folder", "add" };

// What the open menu is about, until its command runs: copied, because
// a tree rebuild rewrites the slots it came from.
static enum kind g_kind;
static char g_aim[PATH_MAX_LEN];
static struct uvolinfo g_vol;
static char g_show_label[96];

#define ADD(...) do { if (n < cap) out[n++] = (struct uui_menu_item)__VA_ARGS__; } while (0)
#define SEP      ADD(UUI_MENU_SEP)

static int open_rows(struct uui_menu_item *out, int n, int cap, int other) {
    ADD(UUI_MENU_VERB("Open", CMD_PL_OPEN, 0, OPEN));
    ADD(UUI_MENU_VERB("Open in new tab", CMD_PL_OPEN_TAB, 0, OPEN_TAB));
    if (other && !g_single) ADD(UUI_MENU_VERB("Open in other pane", CMD_PL_OPEN_OTHER, 0, OTHER_PANE));
    return n;
}

static int path_rows(struct uui_menu_item *out, int n, int cap) {
    ADD(UUI_MENU_VERB("Copy path", CMD_PL_COPY_PATH, 0, COPY_PATH));
    ADD(UUI_MENU_VERB("Open in Terminal", CMD_PL_TERMINAL, 0, TERMINAL));
    return n;
}

// Each pin's neighbours: whether it can move up or down at all.
static void pin_ends(const char *path, int *first, int *last) {
    int lo = g_places.places - g_places.pinned;
    *first = *last = 0;
    for (int i = lo; i < g_places.places; i++)
        if (!strcmp(g_places.row[i].path, path)) { *first = i == lo; *last = i == g_places.places - 1; }
}

// "Show hidden places (Music, Pictures)": the names, as many as fit.
static int show_hidden_row(struct uui_menu_item *out, int n, int cap) {
    char hidden[128];
    hidden_list(hidden, sizeof hidden);
    named_fill();
    int k = 0;
    size_t len = 0;
    g_show_label[0] = '\0';
    for (int i = 0; i < N_NAMED; i++) {
        if (!is_hidden(hidden, NAMED[i].key)) continue;
        len = strlen(g_show_label);
        snprintf(g_show_label + len, sizeof g_show_label - len, "%s%s", k ? ", " : "Show hidden places (",
                 NAMED[i].label);
        k++;
    }
    if (!k) return n;
    len = strlen(g_show_label);
    snprintf(g_show_label + len, sizeof g_show_label - len, ")");
    ADD(UUI_MENU_VERB(g_show_label, CMD_PL_SHOW_HIDDEN, 0, SHOW));
    return n;
}

// The row's kind: a heading or the empty pane below the rows is the
// pane itself; a place row by where it sits in g_places.
static enum kind kind_of(int nid) {
    if (nid < 0 || nid >= g_tree_count) return K_PANE;
    int pl = tree_place_of(nid);
    if (pl == TREE_HEADER) return K_PANE;
    if (pl == TREE_ADD_ROW) return K_ADD;
    if (pl < 0) return K_FOLDER;
    if (pl >= g_places.network) return K_SHARE;
    if (pl >= g_places.places) return K_VOLUME;
    if (pl >= g_places.places - g_places.pinned) return K_PIN;
    const char *p = g_places.row[pl].path;
    return !strcmp(p, FM_RECENT) ? K_RECENT : !strcmp(p, FM_BIN) ? K_BIN : K_NAMED;
}

int places_ctx_items(struct uui_menu_item *out, int cap, int nid) {
    int n = 0;
    g_kind = kind_of(nid);
    g_aim[0] = '\0';
    if (g_kind != K_PANE) strlcpy(g_aim, g_tree_path[nid], sizeof g_aim);
    switch (g_kind) {
    case K_PANE:
        n = show_hidden_row(out, n, cap);
        ADD(UUI_MENU_VERB("Folder tree", CMD_VIEW_TREE, 0, TREE));
        break;
    case K_ADD:
        ADD(UUI_MENU_VERB("Add network share...", CMD_PL_ADD_SHARE, 0, NEW_FILE));
        break;
    case K_RECENT:
        n = open_rows(out, n, cap, 0);
        SEP;
        ADD(UUI_MENU_VERB("Hide from Places", CMD_PL_HIDE, 0, HIDE));
        ADD(UUI_MENU_VERB("Clear list", CMD_PL_CLEAR_RECENT, 0, CLEAR));
        break;
    case K_BIN:
        n = open_rows(out, n, cap, 0);
        SEP;
        ADD(UUI_MENU_VERB("Restore all", CMD_PL_RESTORE_ALL, 0, RESTORE));
        ADD(UUI_MENU_VERB("Empty Recycle Bin", CMD_PL_EMPTY_BIN, 0, EMPTY_BIN));
        SEP;
        ADD(UUI_MENU_VERB("Hide from Places", CMD_PL_HIDE, 0, HIDE));
        ADD(UUI_MENU_VERB("Recycle Bin settings...", CMD_PL_BIN_SETTINGS, 0, SETTINGS));
        break;
    case K_NAMED:
        n = open_rows(out, n, cap, 1);
        SEP;
        ADD(UUI_MENU_VERB("Hide from Places", CMD_PL_HIDE, 0, HIDE));
        SEP;
        n = path_rows(out, n, cap);
        SEP;
        ADD(UUI_MENU_VERB("Properties", CMD_PL_PROPERTIES, 0, PROPERTIES));
        break;
    case K_PIN: {
        int first, last;
        pin_ends(g_aim, &first, &last);
        n = open_rows(out, n, cap, 1);
        SEP;
        ADD(UUI_MENU_VERB("Rename pin...", CMD_PL_RENAME, 0, RENAME));
        if (!first) ADD(UUI_MENU_VERB("Move up", CMD_PL_UP, 0, MOVE_UP));
        if (!last) ADD(UUI_MENU_VERB("Move down", CMD_PL_DOWN, 0, MOVE_DOWN));
        ADD(UUI_MENU_VERB("Unpin from Places", CMD_PL_UNPIN, 0, PIN));
        SEP;
        n = path_rows(out, n, cap);
        SEP;
        ADD(UUI_MENU_VERB("Properties", CMD_PL_PROPERTIES, 0, PROPERTIES));
        break;
    }
    case K_FOLDER:
        n = open_rows(out, n, cap, 1);
        SEP;
        if (upins_has(g_aim)) ADD(UUI_MENU_VERB("Unpin from Places", CMD_PL_UNPIN, 0, PIN));
        else ADD(UUI_MENU_VERB("Pin to Places", CMD_PL_PIN, 0, PIN));
        n = path_rows(out, n, cap);
        SEP;
        ADD(UUI_MENU_VERB("Properties", CMD_PL_PROPERTIES, 0, PROPERTIES));
        break;
    case K_VOLUME:
        uvolinfo_load(g_aim, &g_vol);
        n = open_rows(out, n, cap, 1);
        SEP;
        n = path_rows(out, n, cap);
        // Disks shows a disk's volumes; one in memory has nothing for it.
        if (g_vol.persistent || g_vol.disks_mounted) {
            SEP;
            ADD(UUI_MENU_VERB("Open in Disks", CMD_PL_DISKS, 0, DISKS));
            if (g_vol.persistent) ADD(UUI_MENU_VERB("Check...", CMD_PL_CHECK, 0, CHECK));
            if (g_vol.disks_mounted) ADD(UUI_MENU_VERB("Unmount", CMD_PL_UNMOUNT, 0, UNMOUNT));
        }
        SEP;
        ADD(UUI_MENU_VERB("Properties", CMD_PL_PROPERTIES, 0, PROPERTIES));
        break;
    case K_SHARE:
        uvolinfo_load(g_aim, &g_vol);
        n = open_rows(out, n, cap, 0);
        SEP;
        ADD(UUI_MENU_VERB("Sign in...", CMD_PL_SIGNIN, 0, SIGN_IN));
        if (g_vol.state == USERFS_ST_CONNECTED) ADD(UUI_MENU_VERB("Disconnect", CMD_PL_DISCONNECT, 0, DISCONNECT));
        ADD(UUI_MENU_VERB("Copy address", CMD_PL_COPY_ADDRESS, 0, LINK));
        SEP;
        ADD(UUI_MENU_VERB("Forget this share", CMD_PL_FORGET, 0, FORGET));
        SEP;
        ADD(UUI_MENU_VERB("Properties", CMD_PL_PROPERTIES, 0, PROPERTIES));
        break;
    default:
        break;
    }
    // What a test reads: the kind, the row's path, and every label.
    char rows[400] = "";
    for (int i = 0; i < n; i++) {
        size_t l = strlen(rows);
        snprintf(rows + l, sizeof rows - l, "%s%s", i ? "|" : "", out[i].label ? out[i].label : "-");
    }
    ulogf("files: placemenu %s %s rows %s\n", KIND_NAME[g_kind], g_aim[0] ? g_aim : "-", rows);
    return n;
}

// --- the commands -----------------------------------------------------------

static void rebuild(void) {
    places_build();
    tree_rebuild();
}

static int spawn_argv(const char *exec, char *const *argv, const char *what) {
    int pid = sys_spawn_argv(exec, argv);
    if (pid < 0) snprintf(g_stat_note, sizeof g_stat_note, "could not start %s", what);
    return pid;
}

// The Terminal starts in the folder: a spawned process takes this one's
// working directory, so this one stands there for the spawn.
static void open_terminal(struct uapp *a, const char *dir) {
    char was[PATH_MAX_LEN];
    if (!getcwd(was, sizeof was)) strlcpy(was, "/", sizeof was);
    if (chdir(dir) != 0) { set_note("cannot open a Terminal there"); return; }
    if (uapp_spawn(a, ULAUNCH_TERMINAL_EXEC, 0) < 0) set_note("could not start the Terminal");
    chdir(was);
}

static void set_hidden(const char *key, int hide) {
    char list[128], out[128] = "";
    hidden_list(list, sizeof list);
    for (int i = 0; i < N_NAMED; i++) {
        int h = !strcmp(NAMED[i].key, key) ? hide : is_hidden(list, NAMED[i].key);
        if (!h) continue;
        size_t l = strlen(out);
        snprintf(out + l, sizeof out - l, "%s%s", l ? "," : "", NAMED[i].key);
    }
    if (out[0]) uconf_set(FILES_CONF, HIDDEN_KEY, out);
    else uconf_unset(FILES_CONF, HIDDEN_KEY);
}

// Leave a volume about to go: a pane standing in it would keep it busy.
static void leave(const char *point) {
    size_t n = strlen(point);
    for (int p = 0; p < 2; p++) {
        const char *d = uui_fileview_dir(&g_pane[p]);
        if (!strncmp(d, point, n) && (d[n] == '\0' || d[n] == '/')) fm_goto(p, "/home");
    }
}

static void unmount(void) {
    char point[UVOL_POINT];
    strlcpy(point, g_aim, sizeof point);
    leave(point);
    int rc = sys_umount(point);
    if (rc < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not unmount %s: %s", point, strerror(-rc));
    } else {
        // The folder Disks made for it goes with it, as Disks does.
        if (!strncmp(point, "/mnt/", 5)) sys_unlink(point);
        snprintf(g_stat_note, sizeof g_stat_note, "unmounted %s", point);
    }
    ulogf("files: unmount %s rc %d\n", point, rc);
    rebuild();
}

// The pin's own name, typed: answer_prompt() hands it here.
void places_rename_pin(const char *label) {
    int e = upins_set_label(g_aim, label);
    if (e == -EINVAL) set_note("a name cannot hold a tab");
    else if (e) set_note("could not rename the pin");
    else snprintf(g_stat_note, sizeof g_stat_note, "renamed the pin to %s",
                  label[0] ? label : k_path_basename(g_aim));
    rebuild();
}

void places_command(struct uapp *a, int code) {
    const char *path = g_aim;
    char text[96];
    // dispatch-ok: ONE CASE PER ROW OF THE MENUS ABOVE, the CMD_PL_* range
    // and nothing else; the bodies are not uniform (a goto, a spawn, a
    // prompt, a file write), as files.c's do_command() says of its own.
    switch (code) {
    case CMD_PL_OPEN:
        tree_goto(path);
        tree_reveal_path(uui_fileview_dir(active()));
        break;
    case CMD_PL_OPEN_TAB:   tab_new(a, path); break;
    case CMD_PL_OPEN_OTHER:
        g_active = !g_active;
        tree_goto(path);
        break;
    case CMD_PL_HIDE: {
        int i = named_of(path);
        if (i < 0) break;
        set_hidden(NAMED[i].key, 1);
        snprintf(g_stat_note, sizeof g_stat_note, "%s hidden -- right-click the pane to show it again",
                 NAMED[i].label);
        rebuild();
        break;
    }
    case CMD_PL_SHOW_HIDDEN:
        uconf_unset(FILES_CONF, HIDDEN_KEY);
        set_note("every place shown");
        rebuild();
        break;
    case CMD_PL_COPY_PATH:
        set_note(uclip_set_text(path, (int)strlen(path)) ? "path copied" : "the clipboard refused it");
        break;
    case CMD_PL_COPY_ADDRESS:
        uvolinfo_address(&g_vol, text, sizeof text);
        set_note(uclip_set_text(text, (int)strlen(text)) ? "address copied" : "the clipboard refused it");
        break;
    case CMD_PL_TERMINAL:   open_terminal(a, path); break;
    case CMD_PL_PROPERTIES:
        if (uapp_spawn(a, PROPERTIES_EXEC, path) < 0) set_note("could not open Properties");
        break;
    case CMD_PL_RENAME:
        upins_label(path, text, sizeof text);
        open_prompt(CMD_PL_RENAME, "Rename pin", text);
        break;
    case CMD_PL_UP:
    case CMD_PL_DOWN:
        if (upins_move(path, code == CMD_PL_UP ? -1 : 1)) set_note("could not move the pin");
        rebuild();
        break;
    case CMD_PL_PIN:
    case CMD_PL_UNPIN: {
        int e = code == CMD_PL_PIN ? upins_add(path) : upins_remove(path);
        if (e == -ENOSPC) set_note("Places holds 8 pins -- unpin one first");
        else if (e && e != -EEXIST && e != -ENOENT) set_note("could not change the pin");
        else snprintf(g_stat_note, sizeof g_stat_note, code == CMD_PL_PIN ? "pinned %s" : "unpinned %s",
                      k_path_basename(path));
        rebuild();
        break;
    }
    case CMD_PL_DISKS: {
        char *const argv[] = { "disks", g_aim, 0 };
        spawn_argv(DISKS_EXEC, argv, "Disks");
        break;
    }
    case CMD_PL_CHECK: {
        char *const argv[] = { "disks", "--check", g_aim, 0 };
        spawn_argv(DISKS_EXEC, argv, "Disks");
        break;
    }
    case CMD_PL_UNMOUNT:    unmount(); break;
    case CMD_PL_SIGNIN:     net_aim(path); net_command(CMD_NET_SIGNIN); break;
    case CMD_PL_DISCONNECT: net_aim(path); net_command(CMD_NET_DISCONNECT); break;
    case CMD_PL_FORGET:     net_aim(path); net_command(CMD_NET_FORGET); break;
    case CMD_PL_ADD_SHARE:  net_add_share(); break;
    // The bin's verbs act on its listing, so the pane goes there first.
    case CMD_PL_RESTORE_ALL:
        if (fm_goto(g_active, FM_BIN)) bin_restore(1);
        break;
    case CMD_PL_EMPTY_BIN:
        if (fm_goto(g_active, FM_BIN)) do_empty_bin();
        break;
    case CMD_PL_CLEAR_RECENT: recent_clear(); break;
    case CMD_PL_BIN_SETTINGS: {
        char *const argv[] = { "settings", "system.trash_max_size", 0 };
        spawn_argv(SETTINGS_EXEC, argv, "Settings");
        break;
    }
    default:
        break;
    }
    ulogf("files: placecmd %d %s\n", code, path[0] ? path : "-");
}
