// The Network rows: adding a share (ui/uui_netshare.h), signing in when
// one asks, saying so when one does not answer, and the three things a
// share's context menu does. The shares themselves are mounts smbfsd
// keeps under /net, so everything else -- listing, copying, opening --
// is the File Manager's ordinary code on an ordinary path.
//
// THE STATE IS THE KERNEL'S (QUERY_FSINFO's remote_state, what the
// share's worker last reported), read fresh here and never cached: a
// sidebar refresh must never be what connects to a server, and that
// record answers without waking one.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/uui_netshare.h"
#include "ui/uui_dialog.h"
#include "lib/unetmount.h"
#include "lib/icon_cache.h"
#include "ui/ulog.h"
#include "kpath.h"
#include "query_abi.h"
#include "userfs_abi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct uui_netshare g_ns;
struct uui_infobar g_netbar;
static int g_netbar_ready;
// Where to go once a sign-in succeeds: the folder that asked.
static int g_pend_pane = -1;
static char g_pend_dir[PATH_MAX_LEN];
// The "not answering" dialog: which share, and its two lines.
static int g_offline_up;
static char g_offline_point[UUI_PLACES_PATH_MAX];
static char g_offline_rows[2][160];
static const char *g_offline_ptrs[2] = { g_offline_rows[0], g_offline_rows[1] };
static char g_offline_title[96];
static unsigned g_sig;
static int g_ticks;

// The network share `dir` is in: its mount, fresh from the kernel. 0 if
// it is in none.
static int share_of(const char *dir, struct query_fsinfo *out) {
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, out, sizeof *out) > 0; i++) {
        if (!(out->flags & QUERY_FS_REMOTE)) continue;
        size_t n = strlen(out->point);
        if (!strncmp(dir, out->point, n) && (dir[n] == '\0' || dir[n] == '/')) return 1;
    }
    return 0;
}

// "//nas/media" -> "nas".
static void server_of(const char *source, char *out, size_t cap) {
    const char *p = source;
    while (*p == '/' || *p == '\\') p++;
    if (!strncmp(source, "smb://", 6)) p = source + 6;
    size_t n = strcspn(p, "/\\:");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void open_signin(int pane, const char *dir, const struct query_fsinfo *fs) {
    char server[96], user[64] = "";
    server_of(fs->source, server, sizeof server);
    struct unetmount m;
    if (unetmount_load(k_path_basename(fs->point), &m)) snprintf(user, sizeof user, "%s", m.user);
    g_pend_pane = pane;
    strlcpy(g_pend_dir, dir, sizeof g_pend_dir);
    g_dialog_kind = DIALOG_NETSHARE;
    uui_netshare_signin(&g_ns, &g_dialog, fs->point, server, user);
    ulogf("files: netshare signin %s\n", fs->point);
}

static void open_offline(const struct query_fsinfo *fs) {
    char server[96];
    server_of(fs->source, server, sizeof server);
    g_offline_up = 1;
    strlcpy(g_offline_point, fs->point, sizeof g_offline_point);
    snprintf(g_offline_title, sizeof g_offline_title, "%s is not answering", server);
    snprintf(g_offline_rows[0], sizeof g_offline_rows[0], "%s",
             fs->note[0] ? fs->note : "The share could not be opened.");
    snprintf(g_offline_rows[1], sizeof g_offline_rows[1],
             fs->remote_state == USERFS_ST_UNREACHABLE ? "It may be asleep or switched off." : "");
    static const struct uui_dialog_button btns[] = {
        { "Close", DLG_CANCEL, 0 }, { "Try again", DLG_OK, UUI_DLG_PRIMARY },
    };
    g_dialog_kind = DIALOG_NETSHARE;
    uui_dialog_open(&g_dialog, g_offline_title, g_offline_ptrs, g_offline_rows[1][0] ? 2 : 1,
                    btns, 2, 1, DLG_CANCEL);
    uui_dialog_set_picture(&g_dialog, icon_get("drive-network", ugfx_char_h() * 4));
    ulogf("files: netshare offline %s state %d\n", fs->point, (int)fs->remote_state);
}

static int ctl(const char *point, uint32_t code) {
    struct userfs_ctl *c = calloc(1, sizeof *c);
    if (!c) return -ENOMEM;
    strlcpy(c->point, point, sizeof c->point);
    c->code = code;
    int64_t rc = sys_userfs(USERFS_CTL, (uint64_t)(uintptr_t)c, 0);
    free(c);
    return rc < 0 ? -sys_errno() : 0;
}

void net_add_share(void) {
    g_pend_pane = -1;
    g_dialog_kind = DIALOG_NETSHARE;
    uui_netshare_add(&g_ns, &g_dialog);
    ulogf("files: netshare add\n");
}

int net_intercept(int pane, const char *dir) {
    if (!strcmp(dir, FM_NET_ADD)) { net_add_share(); return 1; }
    struct query_fsinfo fs;
    if (!share_of(dir, &fs)) return 0;
    if (fs.remote_state == USERFS_ST_NEED_LOGIN) { open_signin(pane, dir, &fs); return 1; }
    return 0;
}

// After a listing: did opening the share ask for a password, or find
// nobody there?
void net_after_goto(int pane, const char *dir) {
    struct query_fsinfo fs;
    if (uui_dialog_is_open(&g_dialog) || !share_of(dir, &fs)) return;
    if (fs.remote_state == USERFS_ST_NEED_LOGIN) open_signin(pane, dir, &fs);
    else if (fs.remote_state == USERFS_ST_UNREACHABLE || fs.remote_state == USERFS_ST_FAILED)
        open_offline(&fs);
}

void net_answer(int code) {
    if (g_offline_up) {
        g_offline_up = 0;
        if (code == DLG_OK) {
            ctl(g_offline_point, USERFS_CTL_RECONNECT);    // forget the failure, try afresh
            fm_goto(g_active, uui_fileview_dir(active()));
        }
        return;
    }
    uui_netshare_answer(&g_ns, code);
}

int net_in_share(const char *dir) {
    struct query_fsinfo fs;
    return share_of(dir, &fs);
}

// Where the share menu points: a tree row's share, or (empty) the one
// the active pane is in.
static char g_target[UUI_PLACES_PATH_MAX];

void net_aim(const char *point) { strlcpy(g_target, point, sizeof g_target); }

void net_command(int code) {
    struct query_fsinfo fs;
    const char *dir = g_target[0] ? g_target : uui_fileview_dir(active());
    int found = share_of(dir, &fs);
    g_target[0] = '\0';
    if (!found) return;
    const char *name = k_path_basename(fs.point);
    if (code == CMD_NET_OPEN) {
        fm_goto(g_active, fs.point);
        return;
    }
    if (code == CMD_NET_SIGNIN) {
        open_signin(g_active, dir, &fs);
    } else if (code == CMD_NET_DISCONNECT) {
        ctl(fs.point, USERFS_CTL_RECONNECT);
        snprintf(g_stat_note, sizeof g_stat_note, "disconnected from %s", fs.source);
    } else if (code == CMD_NET_FORGET) {
        char point[UUI_PLACES_PATH_MAX];
        strlcpy(point, fs.point, sizeof point);
        // Leave the share first: a pane in it would keep it in use.
        for (int p = 0; p < 2; p++) {
            struct query_fsinfo in;
            if (share_of(uui_fileview_dir(&g_pane[p]), &in) && !strcmp(in.point, point))
                fm_goto(p, "/home");
        }
        struct unetmount m;
        if (unetmount_load(name, &m) && !strncmp(point, UNETMOUNT_ROOT "/", strlen(UNETMOUNT_ROOT) + 1)) {
            unetmount_remove(name);           // smbfsd unmounts it within a couple of seconds
            snprintf(g_stat_note, sizeof g_stat_note, "forgot the share %s", name);
        } else if (sys_umount(point) == 0) {   // a session share, or one mounted by hand
            snprintf(g_stat_note, sizeof g_stat_note, "closed %s", point);
        } else {
            snprintf(g_stat_note, sizeof g_stat_note, "%s is in use -- close what is open there", point);
        }
        uui_places_refresh(&g_places);
        tree_rebuild();
    }
}

// What the Network rows say, folded to one number: a share that came,
// went, connected or started asking for a password changes it.
static unsigned signature(void) {
    struct query_fsinfo fs;
    unsigned h = 2166136261u;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_REMOTE)) continue;
        for (const char *p = fs.point; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        h = (h ^ (unsigned)fs.remote_state) * 16777619u;
        h = (h ^ (unsigned)(fs.total_bytes >> 20)) * 16777619u;
    }
    return h;
}

int net_tick(void) {
    char point[UUI_PLACES_PATH_MAX];
    int changed = 0;
    switch (uui_netshare_poll(&g_ns, point, sizeof point)) {
    case UUI_NETSHARE_OPEN: {
        int pane = g_pend_pane >= 0 ? g_pend_pane : g_active;
        char dest[PATH_MAX_LEN];
        strlcpy(dest, g_pend_pane >= 0 ? g_pend_dir : point, sizeof dest);
        g_pend_pane = -1;
        uui_places_refresh(&g_places);
        tree_rebuild();
        fm_goto(pane, dest);
        ulogf("files: netshare open %s items %d\n", dest, uui_fileview_count(&g_pane[pane]));
        changed = 1;
        break;
    }
    case UUI_NETSHARE_CLOSED:
        g_pend_pane = -1;
        changed = 1;
        break;
    case UUI_NETSHARE_BUSY:
        if (g_ticks % 15 == 0) set_note("signing in...");
        break;
    default:
        break;
    }
    // Twice a second: the rows' states, which change with no write to
    // any folder, so the File Manager's generation watch never sees them.
    if (++g_ticks % 15 == 0) {
        unsigned sig = signature();
        if (sig != g_sig) {
            g_sig = sig;
            int count = g_places.count;
            uui_places_refresh(&g_places);
            if (g_places.count != count) tree_rebuild();
            else tree_refresh_meters();
            changed = 1;
        }
    }
    return changed;
}

// ---- shares opened for the session (lib/unetmount.h's addresses) ----------

int net_goto_address(int pane, const char *typed) {
    if (!unetmount_is_address(typed)) return 0;
    char path[PATH_MAX_LEN];
    int rc = unetmount_open_address(typed, path, sizeof path);
    if (rc) {
        snprintf(g_stat_note, sizeof g_stat_note, rc == -EINVAL ? "not a share address: %s"
                 : "could not open %s", typed);
        return 1;
    }
    uui_places_refresh(&g_places);
    tree_rebuild();
    if (!fm_goto(pane, path)) snprintf(g_stat_note, sizeof g_stat_note, "no such folder in the share");
    ulogf("files: netshare address %s -> %s\n", typed, path);
    return 1;
}

int net_bar_wanted(void) {
    if (!g_netbar_ready) {
        uui_infobar_init(&g_netbar, "tb-info", "Opened for this session -- it is gone after a restart.",
                         "Save to Network");
        g_netbar_ready = 1;
    }
    struct query_fsinfo fs;
    return share_of(uui_fileview_dir(active()), &fs) && unetmount_is_session(fs.point);
}

void net_save_session(void) {
    struct query_fsinfo fs;
    if (!share_of(uui_fileview_dir(active()), &fs)) return;
    const char *name = k_path_basename(fs.point);
    struct userfs_ctl *c = calloc(1, sizeof *c);
    if (!c) return;
    strlcpy(c->point, fs.point, sizeof c->point);
    c->code = USERFS_CTL_SAVE;
    c->len = (uint32_t)strlen(name) + 1;
    memcpy(c->data, name, c->len);
    int64_t rc = sys_userfs(USERFS_CTL, (uint64_t)(uintptr_t)c, 0);
    int e = rc < 0 ? sys_errno() : 0;
    free(c);
    if (!rc) snprintf(g_stat_note, sizeof g_stat_note, "saved as %s -- it comes back after a restart", name);
    else snprintf(g_stat_note, sizeof g_stat_note, e == EEXIST ? "a share called %s is already saved"
                  : "could not save %s", name);
    ulogf("files: netshare save %s rc %d\n", fs.point, e);
    uui_places_refresh(&g_places);
    tree_rebuild();
}
