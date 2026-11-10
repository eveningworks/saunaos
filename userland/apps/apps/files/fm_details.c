// The details pane: a preview of the selection and its facts, down the
// right of the window -- Explorer's (Alt+Shift+P) and Dolphin's
// Information panel. Nothing selected, it describes the folder itself;
// several marked, their count and total. One item is drawn by the same
// widget as Properties (ui/uui_fileinfo.h), so the two cannot disagree.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/utheme.h"
#include "ui/uui_label.h"
#include "base/human.h"
#include "base/udate.h"
#include "base/ufiletype.h"
#include "media/icon_cache.h"
#include "media/ufileinfo.h"
#include "ui/uui_fileinfo.h"
#include "ui/uui_volinfo.h"
#include "net/uvolinfo.h"
#include "userfs_abi.h"
#include "base/ulog.h"
#include "kpath.h"
#include <string.h>
#include <stdio.h>

static int g_dx, g_dy, g_dw, g_dh;   // the pane's rect, from layout_all()

#define PAD 12

// One item -- the selection, or the folder itself -- is the shared facts
// widget in its compact form (ui/uui_fileinfo.h, Properties' hero and
// General rows). Several marked keep their count and total, which no
// single-file view can say.
static struct ufileinfo g_fi;
static struct uui_fileinfo g_info;
static char g_fi_key[UUI_FILEVIEW_PATH_MAX + 32];   // path + mtime it was read for

// A VOLUME'S ROOT WITH NOTHING SELECTED is described as the volume -- its
// card, shared with Properties (ui/uui_volinfo.h) -- not as a folder:
// what "/net/media" is, is a share of //nas/media. Re-read at most once
// a second, since a share's state and every volume's free space move on
// their own and this runs on every paint.
static struct uvolinfo g_vol;
static struct uui_volinfo g_vcard;
static int g_vol_on;
static unsigned long long g_vol_ns;

// The first button's verb on a volume card: what that kind can have done.
enum { VACT_NONE, VACT_DISCONNECT, VACT_SIGNIN, VACT_DISKS, VACT_EJECT };
static int vol_act(void) {
    if (g_vol.image) return VACT_EJECT;   // a disc image's CD drive
    if (g_vol.remote) return g_vol.state == USERFS_ST_CONNECTED ? VACT_DISCONNECT : VACT_SIGNIN;
    return g_vol.persistent ? VACT_DISKS : VACT_NONE;
}

int details_width(int cw) {
    int w = ugfx_char_advance('n') * 28;
    return w > cw / 3 ? cw / 3 : w;
}

static int line_h(void)    { return ugfx_char_h() + 4; }

// The path the pane describes, or 0 with several marked.
static int described(char *path, int cap, const struct ulist_entry **ent) {
    struct uui_fileview *fv = active();
    const struct ulist_entry *e = uui_fileview_selected_entry(fv);
    const char *dir = uui_fileview_dir(fv);
    *ent = e;
    if (uui_fileview_mark_count(fv) > 1) return 0;
    // The selection's REAL path -- in the Recycle Bin, where it lies in
    // the bin, not "trash:/" joined with its name.
    if (e && in_archive(fv)) return 0;   // a member has no path to read facts from
    if (e) return uui_fileview_selected_path(fv, path, cap);
    if (in_bin(fv) || in_search(fv) || in_archive(fv) || in_recent(fv) || in_quick(fv)) return 0;   // a virtual folder is nothing to describe
    strlcpy(path, dir, (size_t)cap);
    return 1;
}

// Re-read the facts only when the item (or its mtime) changed: the
// picture's headers are a file read, and this runs on every paint.
static void refresh(void) {
    static int inited;
    if (!inited) { uui_fileinfo_init(&g_info); g_info.compact = 1; uui_volinfo_init(&g_vcard, &g_vol, 1); inited = 1; }
    char path[UUI_FILEVIEW_PATH_MAX];
    const struct ulist_entry *e;
    g_vol_on = 0;
    if (!described(path, sizeof path, &e)) { g_fi_key[0] = 0; return; }
    if (!e) {
        unsigned long long now = sys_monotonic_ns();
        if (strcmp(g_vol.point, path) || now - g_vol_ns >= 1000000000ull) {
            g_vol_ns = now;
            if (!uvolinfo_load(path, &g_vol)) g_vol.point[0] = '\0';
            else if (strcmp(g_vol.point, path)) g_vol.point[0] = '\0';
        }
        if (!strcmp(g_vol.point, path)) { g_vol_on = 1; g_fi_key[0] = 0; return; }
    }
    char key[sizeof g_fi_key];
    // The mounts' signature too: a disc image's facts say whether it is mounted.
    snprintf(key, sizeof key, "%s|%u|%x", path, e ? (unsigned)e->modified.second + 60u * e->modified.minute : 0u,
             places_signature());
    if (strcmp(key, g_fi_key)) {
        strlcpy(g_fi_key, key, sizeof g_fi_key);
        ufileinfo_load(&g_fi, path, UFI_HEADERS | UFI_DETAILS);
        if (!e) g_fi.files = uui_fileview_count(active());   // the folder itself: its items
        uui_fileinfo_set(&g_info, &g_fi);
    }
    g_info.preview = 0;
    char parent[UUI_FILEVIEW_PATH_MAX];
    if (e && !e->is_dir && g_opt.folder.thumbs && k_path_dirname(path, parent, sizeof parent))
        g_info.preview = pane_thumb(0, parent, e, uui_fileinfo_preview_px(&g_info, g_dw));
}

// Where the buttons go: under whatever the pane drew.
static int buttons_y(void) {
    if (g_vol_on) return g_dy + uui_volinfo_height(&g_vcard, g_dw) + 4;
    if (g_fi_key[0]) return g_dy + uui_fileinfo_height(&g_info, g_dw) + 4;
    return g_dy + PAD + (g_dw - 2 * PAD) * 3 / 5 + 10 + 3 * line_h() + 8 + 2 * line_h() + 10;
}

void details_layout(int x, int y, int w, int h) {
    g_dx = x; g_dy = y; g_dw = w; g_dh = h;
    refresh();
    uui_fileinfo_set_geometry(&g_info, x + 1, y, w - 1, uui_fileinfo_height(&g_info, w - 1));
    g_vcard.x = x + 1; g_vcard.y = y; g_vcard.w = w - 1; g_vcard.h = uui_volinfo_height(&g_vcard, w - 1);
    static const char *const VERB[] = { "Open", "Disconnect", "Sign in...", "Open in Disks", "Eject" };
    // A disc image not mounted: its first verb mounts it (Open then goes there).
    g_dp_open.label = g_vol_on ? VERB[vol_act()]
                    : g_fi_key[0] && g_fi.disc && !g_fi.disc_point[0] ? "Mount" : "Open";
    static char logged[UFI_PATH + 16];   // what a test reads, once per change
    char now[sizeof logged];
    int disc = !g_vol_on && g_fi_key[0] && g_fi.disc;
    snprintf(now, sizeof now, "%s|%d|%s", g_vol_on ? g_vol.point : disc ? g_fi.path : "",
             g_vol_on ? vol_act() : 0, g_dp_open.label);
    if (strcmp(now, logged)) {
        strlcpy(logged, now, sizeof logged);
        if (g_vol_on) ulogf("files: details volume %s button %s\n", g_vol.point, g_dp_open.label);
        if (disc) ulogf("files: details disc %s label %s button %s\n", g_fi.path, g_fi.disc_label,
                        g_dp_open.label);
    }
    int bw, bh, bw2;
    uui_button_natural_size(&g_dp_open, &bw, &bh);
    uui_button_natural_size(&g_dp_props, &bw2, &bh);
    int by = buttons_y();
    uui_button_set_geometry(&g_dp_open, x + PAD, by, bw, bh);
    uui_button_set_geometry(&g_dp_props, x + PAD + bw + 6, by, bw2, bh);
    // Nothing to open when nothing is chosen.
    g_dp_open.disabled = uui_fileview_selected_name(active()) == 0;
    // An archive's member has no Properties of its own (arc_item_flags).
    g_dp_props.disabled = g_dp_open.disabled || in_archive(active());
    if (g_vol_on) {
        g_dp_open.disabled = vol_act() == VACT_NONE;
        g_dp_props.disabled = 0;
    }
}

// The details pane's buttons on a volume card: its own verb, and its
// Properties. 0 when the pane shows a file, for files.c's own answer.
int details_button(struct uapp *a, int id) {
    if (!g_vol_on) return 0;
    char point[UVOL_POINT];
    strlcpy(point, g_vol.point, sizeof point);
    if (id == ID_DP_PROPS) {
        if (uapp_spawn(a, PROPERTIES_EXEC, point) < 0) set_note("could not open Properties");
        return 1;
    }
    switch (vol_act()) {
    case VACT_DISCONNECT: net_aim(point); net_command(CMD_NET_DISCONNECT); break;
    case VACT_SIGNIN:     net_aim(point); net_command(CMD_NET_SIGNIN); break;
    case VACT_EJECT:      disc_eject(point); break;
    case VACT_DISKS: {
        char *const argv[] = { "disks", point, 0 };
        if (sys_spawn_argv("/bin/wm/apps/disks", argv) < 0) set_note("could not start Disks");
        break;
    }
    default: break;
    }
    g_vol_ns = 0;   // its state is about to change: read it again at once
    return 1;
}

// Several marked: their count, total size and folder.
static void draw_marked(struct ugfx_surface *s, uint32_t bg) {
    struct uui_fileview *fv = active();
    int marks = uui_fileview_mark_count(fv);
    int bx = g_dx + PAD, by = g_dy + PAD, bw = g_dw - 2 * PAD, bh = (g_dw - 2 * PAD) * 3 / 5;
    const struct uimg *ico = icon_get("file", bh * 2 / 3);
    if (ico) ugfx_blit_alpha(s, bx + (bw - ico->w) / 2, by + (bh - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
    char title[UUI_FILEVIEW_PATH_MAX], h[24];
    const char *one = uui_fileview_selected_name(fv);
    if (marks) snprintf(title, sizeof title, "%d items selected", marks);
    else if (in_archive(fv) && one) snprintf(title, sizeof title, "%s", one);   // a member: its name
    else snprintf(title, sizeof title, "Nothing selected");
    int y = by + bh + 10;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, bx, y, bw, title, UTHEME_TEXT, bg);
    ugfx_set_font(was);
    y += 3 * line_h() + 8;
    unsigned long long bytes = marks ? fm_marked_bytes(fv) : 0;
    const struct ulist_entry *sel = uui_fileview_selected_entry(fv);
    int member = !marks && in_archive(fv) && sel && !sel->is_dir;
    if (member) bytes = sel->size;
    human_size(h, sizeof h, bytes);
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int kw = ugfx_char_advance('n') * 9;
    if (marks || member) {   // nothing selected has no size worth a "0B"
        ugfx_draw_string_clipped(s, bx, y, kw, "Size", dim, bg);
        ugfx_draw_string_clipped(s, bx + kw, y, bw - kw, h, UTHEME_TEXT, bg);
        y += line_h();
    }
    ugfx_draw_string_clipped(s, bx, y, kw, "Location", dim, bg);
    // A virtual folder by its name, not its "trash:/" spelling.
    char archive[UUI_FILEVIEW_PATH_MAX];
    const char *inner;
    const char *where = in_bin(fv) ? "Recycle Bin" : in_search(fv) ? "Search results"
                      : in_recent(fv) ? "Recent" : in_quick(fv) ? "Quick access"
                      : arc_split(uui_fileview_dir(fv), archive, sizeof archive, &inner)
                        ? k_path_basename(archive) : uui_fileview_dir(fv);
    ugfx_draw_string_elided(s, bx + kw, y, bw - kw, where, UTHEME_TEXT, bg);
}

void details_draw(struct ugfx_surface *s) {
    uint32_t bg = ugfx_blend(UTHEME_PANEL_BG, UTHEME_WHITE, 128);   // a side pane, a step lighter
    ugfx_fill_rect(s, g_dx, g_dy, g_dw, g_dh, bg);
    ugfx_fill_rect(s, g_dx, g_dy, 1, g_dh, UTHEME_BORDER);
    ugfx_set_clip_rect(s, g_dx, g_dy, g_dw, g_dh);
    if (g_vol_on) uui_volinfo_draw(s, &g_vcard);
    else if (g_fi_key[0]) uui_fileinfo_draw(s, &g_info);
    else draw_marked(s, bg);
    ugfx_clear_clip_rect(s);
}
