// The left pane's places and their menus: which places it lists (the
// Places list of lib/upins.h, in the person's order; the volumes and
// shares),
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
#define HIDDEN_KEY    "hidden_places"   // the old hide switch, read once to migrate

// HIDDEN PLACES WERE A SWITCH; they are entries unpinned now. A
// files.conf from before still naming some is turned into that once.
static void migrate_hidden(void) {
    char list[128] = "";
    if (!uconf_get(FILES_CONF, HIDDEN_KEY, list, sizeof list) || !list[0]) return;
    for (char *k = strtok(list, ","); k; k = strtok(0, ",")) {
        char entry[40];
        if (!strcmp(k, "recent")) strlcpy(entry, "recent:/", sizeof entry);
        else if (!strcmp(k, "bin")) strlcpy(entry, "trash:/", sizeof entry);
        else if (!strcmp(k, "home")) strlcpy(entry, "/home", sizeof entry);
        else snprintf(entry, sizeof entry, "udir:%s", k);
        upins_remove(entry);
    }
    uconf_unset(FILES_CONF, HIDDEN_KEY);
}

// PLACES IS ONE LIST (lib/upins.h): the defaults and the pins in the
// person's order, then every mounted volume.
void places_build(void) {
    static int migrated;
    if (!migrated) { migrated = 1; migrate_hidden(); }
    uui_places_init(&g_places);
    uui_places_add_pins(&g_places, 1);
    uui_places_refresh(&g_places);
}

// A TREE ROW WHOSE FOLDER HAS GONE -- a pin to a folder deleted since
// the tree was built, most often -- is said so, and the pane stays where
// it was: set_dir() has already taken the path when the listing fails.
// The rebuild drops it from Places (lib/upins.h keeps the entry for when
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

enum kind { K_NONE, K_PANE, K_RECENT, K_BIN, K_PIN, K_VOLUME, K_SHARE, K_FOLDER, K_ADD };
static const char *const KIND_NAME[] = { "none", "pane", "recent", "bin", "place",
                                         "volume", "share", "folder", "add" };

// What the open menu is about, until its command runs: copied, because
// a tree rebuild rewrites the slots it came from.
static enum kind g_kind;
static char g_aim[PATH_MAX_LEN];
static struct uvolinfo g_vol;

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

// An entry's neighbours in the list: whether it can move up or down.
static void list_ends(const char *path, int *first, int *last) {
    *first = *last = 0;
    for (int i = 0; i < g_places.places; i++)
        if (!strcmp(g_places.row[i].path, path)) { *first = i == 0; *last = i == g_places.places - 1; }
}

// What any entry of Places can have done to it.
static int list_rows(struct uui_menu_item *out, int n, int cap) {
    int first, last;
    list_ends(g_aim, &first, &last);
    ADD(UUI_MENU_VERB("Rename...", CMD_PL_RENAME, 0, RENAME));
    if (!first) ADD(UUI_MENU_VERB("Move up", CMD_PL_UP, 0, MOVE_UP));
    if (!last) ADD(UUI_MENU_VERB("Move down", CMD_PL_DOWN, 0, MOVE_DOWN));
    ADD(UUI_MENU_VERB("Unpin from Places", CMD_PL_UNPIN, 0, PIN));
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
    const char *p = g_places.row[pl].path;
    return !strcmp(p, FM_RECENT) ? K_RECENT : !strcmp(p, FM_BIN) ? K_BIN : K_PIN;
}

int places_ctx_items(struct uui_menu_item *out, int cap, int nid) {
    int n = 0;
    g_kind = kind_of(nid);
    g_aim[0] = '\0';
    if (g_kind != K_PANE) strlcpy(g_aim, g_tree_path[nid], sizeof g_aim);
    switch (g_kind) {
    case K_PANE:
        if (upins_missing_defaults() > 0)
            ADD(UUI_MENU_VERB("Restore default places", CMD_PL_RESTORE_DEFAULTS, 0, SHOW));
        ADD(UUI_MENU_VERB("Folder tree", CMD_VIEW_TREE, 0, TREE));
        break;
    case K_ADD:
        ADD(UUI_MENU_VERB("Add network share...", CMD_PL_ADD_SHARE, 0, NEW_FILE));
        break;
    case K_RECENT:
        n = open_rows(out, n, cap, 0);
        SEP;
        ADD(UUI_MENU_VERB("Clear list", CMD_PL_CLEAR_RECENT, 0, CLEAR));
        SEP;
        n = list_rows(out, n, cap);
        break;
    case K_BIN:
        n = open_rows(out, n, cap, 0);
        SEP;
        ADD(UUI_MENU_VERB("Restore all", CMD_PL_RESTORE_ALL, 0, RESTORE));
        ADD(UUI_MENU_VERB("Empty Recycle Bin", CMD_PL_EMPTY_BIN, 0, EMPTY_BIN));
        ADD(UUI_MENU_VERB("Recycle Bin settings...", CMD_PL_BIN_SETTINGS, 0, SETTINGS));
        SEP;
        n = list_rows(out, n, cap);
        break;
    case K_PIN:
        n = open_rows(out, n, cap, 1);
        SEP;
        n = list_rows(out, n, cap);
        SEP;
        n = path_rows(out, n, cap);
        SEP;
        ADD(UUI_MENU_VERB("Properties", CMD_PL_PROPERTIES, 0, PROPERTIES));
        break;
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
    else if (e) set_note("could not rename it");
    else if (label[0]) snprintf(g_stat_note, sizeof g_stat_note, "renamed to %s", label);
    else set_note("its own name again");
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
    case CMD_PL_RESTORE_DEFAULTS: {
        int back = upins_restore_defaults();
        if (back > 0) snprintf(g_stat_note, sizeof g_stat_note, "%d place%s back", back, back == 1 ? "" : "s");
        rebuild();
        break;
    }
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
        open_prompt(CMD_PL_RENAME, "Rename in Places", text);
        break;
    case CMD_PL_UP:
    case CMD_PL_DOWN:
        if (upins_move(path, code == CMD_PL_UP ? -1 : 1)) set_note("could not move it");
        rebuild();
        break;
    case CMD_PL_PIN:
    case CMD_PL_UNPIN: {
        int e = code == CMD_PL_PIN ? upins_add(path) : upins_remove(path);
        if (e == -ENOSPC) set_note("Places is full -- unpin one first");
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
        char *const argv[] = { "settings", "system.trash.max_size", 0 };
        spawn_argv(SETTINGS_EXEC, argv, "Settings");
        break;
    }
    default:
        break;
    }
    ulogf("files: placecmd %d %s\n", code, path[0] ? path : "-");
}

// --- reordering by drag (uui_tree_set_reorder) -------------------------------
//
// THREE GROUPS MOVE BY DRAG, each within itself: the Places list (any
// row, into any gap of it -- Explorer's Quick access), the volumes under
// This computer and the shares under Network (Dolphin's Devices). A
// folder dragged in from a pane lands only in a Places gap, and is pinned
// there; dropped ONTO a row it is moved into that folder, as ever.

enum { G_NONE = -1, G_LIST, G_VOLUMES, G_SHARES };

static int group_of(int nid) {
    int pl = tree_place_of(nid);
    if (pl < 0) return G_NONE;
    if (pl < g_places.places) return G_LIST;
    if (pl < g_places.network) return G_VOLUMES;
    return pl < g_places.count ? G_SHARES : G_NONE;
}

static int places_can_drag(void *ctx, int nid) {
    (void)ctx;
    return group_of(nid) != G_NONE;
}

// The gap after `after`, before `before` (-1 at an end): beside a row of
// the dragged row's own group -- files: of the Places list.
static int places_can_insert(void *ctx, int from, int after, int before) {
    (void)ctx;
    int g = from >= 0 ? group_of(from) : G_LIST;
    if (g == G_NONE) return 0;
    if (before >= 0 && group_of(before) == g)
        return after < 0 || group_of(after) == g || g != G_LIST;
    return after >= 0 && group_of(after) == g;
}

void places_reorder_init(void) {
    uui_tree_set_reorder(&g_tree, places_can_drag, places_can_insert, 0);
}

// A volume or a share dropped in its group's gap: the group's points in
// their new order, saved (lib/upins.h's dev: lines).
// Before `before` when it is of the group, else right after `after` --
// with the folder tree open, a volume's own folders sit between it and
// the next, so the gap under it names a folder below.
static void mounts_reorder(int g, int from, int after, int before) {
    static char pts[UUI_PLACES_MAX][64];
    int lo = g == G_VOLUMES ? g_places.places : g_places.network;
    int hi = g == G_VOLUMES ? g_places.network : g_places.count;
    const char *moved = g_places.row[tree_place_of(from)].path;
    const char *ahead = before >= 0 && group_of(before) == g ? g_places.row[tree_place_of(before)].path : 0;
    const char *behind = !ahead && after >= 0 && group_of(after) == g ? g_places.row[tree_place_of(after)].path : 0;
    int n = 0, put = 0;
    for (int i = lo; i < hi && n < UUI_PLACES_MAX - 1; i++) {
        const char *p = g_places.row[i].path;
        if (!strcmp(p, moved)) continue;
        if (ahead && !strcmp(p, ahead)) { strlcpy(pts[n++], moved, sizeof pts[0]); put = 1; }
        strlcpy(pts[n++], p, sizeof pts[0]);
        if (behind && !strcmp(p, behind)) { strlcpy(pts[n++], moved, sizeof pts[0]); put = 1; }
    }
    if (!put) strlcpy(pts[n++], moved, sizeof pts[0]);
    int e = upins_dev_set_order(pts, n);
    ulogf("files: placedrop mount %s %s %s rc %d\n", moved, ahead ? "before" : "after",
          ahead ? ahead : behind ? behind : "-", e);
}

// A drop into a gap of the list: a row moved, or the source pane's
// folders pinned there. 1 when it was one (the caller then does nothing
// else with the drop).
int places_drop(struct uui_fileview *src) {
    int from, after, before;
    if (!uui_tree_drop_insert(&g_tree, &from, &after, &before)) return 0;
    char before_path[PATH_MAX_LEN] = "";
    if (before >= 0 && before < g_tree_count && group_of(before) == G_LIST)
        strlcpy(before_path, g_tree_path[before], sizeof before_path);
    const char *at = before_path[0] ? before_path : 0;
    if (from >= 0 && from < g_tree_count && group_of(from) != G_LIST) {
        mounts_reorder(group_of(from), from, after, before);
    } else if (from >= 0 && from < g_tree_count) {
        char moved[PATH_MAX_LEN];
        strlcpy(moved, g_tree_path[from], sizeof moved);
        int e = upins_move_before(moved, at);
        ulogf("files: placedrop move %s before %s rc %d\n", moved, at ? at : "-", e);
    } else if (src) {
        // The marked folders, or the selected one; files are not places.
        int marks = uui_fileview_mark_count(src), pinned = 0, full = 0;
        for (int m = 0; m < (marks ? marks : 1); m++) {
            char path[PATH_MAX_LEN];
            int ok = marks ? uui_fileview_marked_path(src, m, path, sizeof path) && uui_fileview_marked_is_dir(src, m)
                           : uui_fileview_selected_path(src, path, sizeof path) && uui_fileview_selected_is_dir(src);
            if (!ok) continue;
            int e = upins_add_before(path, at);
            if (e == -EEXIST) e = upins_move_before(path, at);   // there already: moved instead
            if (e == -ENOSPC) { full = 1; break; }
            if (!e) pinned++;
            ulogf("files: placedrop pin %s before %s rc %d\n", path, at ? at : "-", e);
        }
        if (full) set_note("Places is full -- unpin one first");
        else if (!pinned) set_note("only folders go in Places");
        else snprintf(g_stat_note, sizeof g_stat_note, "pinned %d folder%s", pinned, pinned == 1 ? "" : "s");
    }
    rebuild();
    return 1;
}
