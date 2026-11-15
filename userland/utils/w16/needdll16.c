// needdll16.c -- "<program> needs a file": the dialog w16 shows, before the program is loaded, when
// a DLL it imports is nowhere the loader looks (w16_missing_dll(), module16.c). SaunaOS ships no
// vendor runtime and fetches none (docs/decisions/gui.md, "Windows' own DLLs are w16's code; a
// program's redistributables run as shipped"), so the person supplies it: Choose file... opens the
// toolkit's file chooser, the file is checked to be an NE library and copied into drive C:'s
// WINDOWS\SYSTEM under the name the program imports, and the load goes on; Cancel ends w16 as a
// missing DLL always did. The design is the "w16: a missing runtime DLL" canvas's B.
//
// A SaunaOS window, not a 3.1 one: it is w16 speaking, not the program. It is opened on the MAIN
// (uapp) thread before the task exists, so nothing here locks g_w16. The sandbox needs nothing
// more for it: a run already reads the whole tree and writes drive C: (w16.c).
#include "w16_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "base/ulog.h"
#include "common/w16prefs.h"
#include "media/icon_cache.h"
#include "ui/uapp.h"
#include "ui/uui_button.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_image.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/utheme.h"

enum { ND_CHOOSE = 1, ND_CANCEL };
enum { KV_FILE, KV_WHAT, KV_BY, KV_TO, KV_ROWS };

static struct nd {
    struct uapp *app;
    struct uapp_window *win;
    void (*done)(struct uapp *, int);
    char mod[16], by[16];
    char title[96], head[48], body[400], note[200];
    char val[KV_ROWS][120];
    struct uui_image icon;
    struct uui_label head_l, body_l, note_l, gap;
    struct uui_label key_l[KV_ROWS], val_l[KV_ROWS];
    struct uui_button choose, cancel;
    struct uui_item kv_items[KV_ROWS][2], kv_rows[KV_ROWS], text_items[4], top_items[2], btn_items[3], items[3];
    struct uui_layout kv_row[KV_ROWS], kv, text, top, btns, root;
    struct uui_focusable focus_items[2];
    struct uui_focus focus;
} g_nd;

static struct uui_filedialog g_nd_chooser;   // ~20 KB: file scope

static const char *const KV_KEYS[KV_ROWS] = { "File", "What it is", "Needed by", "Copied to" };

static void nd_finish(int supplied) {
    if (g_nd.win) uapp_window_close(g_nd.win);
    g_nd.win = 0;
    if (g_nd.done) g_nd.done(g_nd.app, supplied);
}

// The chosen file into drive C:'s WINDOWS\SYSTEM as MODULE.DLL -- once it is seen to be a Windows
// library. 0, or -1 with the reason in the note line.
static int nd_copy(const char *from) {
    uint32_t size = 0;
    uint8_t *img = w16_read_file(from, &size);
    static struct ne_module m;
    if (!img || ne_parse(img, size, &m) < 0 || !(m.flags & 0x8000)) {
        snprintf(g_nd.note, sizeof g_nd.note, "%s is not a Windows 3.x DLL.", strrchr(from, '/') ? strrchr(from, '/') + 1 : from);
        free(img);
        return -1;
    }
    char dir[512], to[600];
    int ok = w16_windows_dir(g_w16_drive_c, 1, 1, dir, sizeof dir) == 0 &&
             snprintf(to, sizeof to, "%s/%s.DLL", dir, g_nd.mod) < (int)sizeof to;
    FILE *f = ok ? fopen(to, "wb") : 0;
    ok = f && fwrite(img, 1, size, f) == size;
    if (f && fclose(f) != 0) ok = 0;
    int err = errno;
    free(img);
    if (!ok) {
        if (f) remove(to);
        snprintf(g_nd.note, sizeof g_nd.note, "Could not copy it to C:\\WINDOWS\\SYSTEM: %s.", strerror(err));
        return -1;
    }
    ulogf("w16: copied %s to %s\n", from, to);
    return 0;
}

static void nd_controls(int on) {
    g_nd.choose.disabled = g_nd.cancel.disabled = !on;
    if (g_nd.win) uapp_window_redraw(g_nd.win);
}

static void nd_chosen(void *ctx, const char *path) {
    (void)ctx;
    nd_controls(1);
    if (!path) return;
    if (nd_copy(path) == 0) { nd_finish(1); return; }
    ulogf("w16: refused %s for %s.DLL: %s\n", path, g_nd.mod, g_nd.note);
    if (g_nd.win) uapp_window_redraw(g_nd.win);
}

static int is_dll(void *ctx, const char *dir, const struct ulist_entry *e) {
    (void)ctx;
    (void)dir;
    size_t n = strlen(e->name);
    return e->is_dir || (n > 4 && !strcasecmp(e->name + n - 4, ".dll"));
}

static void nd_choose(void) {
    static const struct uui_filedialog_filter FILTERS[] = {
        { "Windows libraries (*.DLL)", is_dll, 0, 0 }, UUI_FILEDIALOG_ALL_FILES,
    };
    if (uui_filedialog_is_open(&g_nd_chooser)) return;
    // Where a program carries its runtime: its own folder, its disks.
    static char start[512], title[64];
    snprintf(start, sizeof start, "%s", g_w16.path ? g_w16.path : "/");
    char *slash = strrchr(start, '/');
    if (slash) *(slash == start ? slash + 1 : slash) = 0;
    snprintf(title, sizeof title, "Choose %s.DLL", g_nd.mod);
    struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_OPEN, .title = title, .start_dir = start,
                                     .filters = FILTERS, .filter_count = 2, .no_mount = 1 };
    g_nd.note[0] = 0;
    if (uui_filedialog_open(g_nd.app, &g_nd_chooser, &o, nd_chosen, 0)) nd_controls(0);
}

static void nd_action(struct uapp_window *w, int code) {
    (void)w;
    if (code == ND_CHOOSE) nd_choose();
    else if (code == ND_CANCEL) nd_finish(0);
}

static void nd_key(struct uapp_window *w, int key, unsigned mods) {
    (void)w;
    (void)mods;
    if (uui_filedialog_is_open(&g_nd_chooser)) return;
    if (key == '\n' || key == '\r') nd_choose();       // Choose file... is the default
    else if (key == 0x1b) nd_finish(0);                 // Esc cancels a dialog
}

static void nd_close(struct uapp_window *w) {
    (void)w;
    if (uui_filedialog_is_open(&g_nd_chooser)) return;   // the chooser answers first
    nd_finish(0);
}

// The program's short name ("WinTune") and its file ("WT20.EXE"), for the title and the first line.
static void prog_names(const struct ne_module *pre, char *name, size_t ncap, char *file, size_t fcap) {
    char about[128];
    char *t = ne_description(pre, about, sizeof about) == 0 && about[0] ? w16_short_title(about) : 0;
    snprintf(name, ncap, "%s", t && t[0] ? t : pre->name);
    free(t);
    const char *b = g_w16.path ? strrchr(g_w16.path, '/') : 0;
    snprintf(file, fcap, "%s", b ? b + 1 : g_w16.path ? g_w16.path : "");
    for (char *p = file; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}

void w16_needdll_open(struct uapp *a, const struct ne_module *pre, const char *mod, const char *by,
                      void (*done)(struct uapp *, int)) {
    struct nd *n = &g_nd;
    if (n->win) { uapp_window_close(n->win); n->win = 0; }
    n->app = a;
    n->done = done;
    snprintf(n->mod, sizeof n->mod, "%s", mod);
    snprintf(n->by, sizeof n->by, "%s", by);
    const char *who = 0, *what = w16_dll_what(mod, &who);
    char name[64], file[64];
    prog_names(pre, name, sizeof name, file, sizeof file);
    snprintf(n->title, sizeof n->title, "%s needs a file", name);
    snprintf(n->head, sizeof n->head, "%s.DLL is missing", mod);
    char need[96];
    if (who) snprintf(need, sizeof need, "the %s", what);
    else snprintf(need, sizeof need, "this DLL");
    int len = by[0] ? snprintf(n->body, sizeof n->body, "%s (%s) loads %s.DLL, which needs %s to start.", name, file, by, need)
                    : snprintf(n->body, sizeof n->body, "%s (%s) needs %s to start.", name, file, need);
    if (len > 0 && len < (int)sizeof n->body)
        snprintf(n->body + len, sizeof n->body - (size_t)len,
                 " SaunaOS does not include it: it comes with the programs that use it, on their disks or in"
                 " their folders.");
    snprintf(n->val[KV_FILE], sizeof n->val[0], "%s.DLL", mod);
    if (who) snprintf(n->val[KV_WHAT], sizeof n->val[0], "%s (%s)", what, who);
    else snprintf(n->val[KV_WHAT], sizeof n->val[0], "A DLL %s needs", by[0] ? "it" : "the program");
    snprintf(n->val[KV_BY], sizeof n->val[0], "%s.DLL", by);
    snprintf(n->val[KV_TO], sizeof n->val[0], "C:\\WINDOWS\\SYSTEM");
    n->note[0] = 0;

    int ch = ugfx_char_h(), cw = ugfx_char_w(), key_w = 0;
    uui_image_init(&n->icon, icon_get("badge-warning", ch * 2 + ch / 2), UIMG_FIT_CONTAIN);
    n->icon.bg = UTHEME_PANEL_BG;                        // a dialog window's ground
    uui_label_init(&n->head_l, n->head);
    n->head_l.bold = 1;
    uui_label_init(&n->body_l, n->body);
    uui_label_set_wrap(&n->body_l, 3);
    uui_label_init(&n->note_l, n->note);
    n->note_l.fg = UUI_ROLE(severity[UTHEME_SEV_ERROR]);
    uui_label_init(&n->gap, "");
    for (int i = 0; i < KV_ROWS; i++) {
        int w = ugfx_text_width(KV_KEYS[i]);
        if (w > key_w) key_w = w;
    }
    int rows = 0;
    for (int i = 0; i < KV_ROWS; i++) {
        if (i == KV_BY && !by[0]) continue;
        uui_label_init(&n->key_l[i], KV_KEYS[i]);
        n->key_l[i].dim = 1;
        uui_label_init(&n->val_l[i], n->val[i]);
        n->kv_items[i][0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->key_l[i], .main_size = key_w + cw * 2 };
        n->kv_items[i][1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->val_l[i], .flags = UUI_FILL_W,
                                               .name = i == KV_FILE ? "needdll_file" : i == KV_TO ? "needdll_to" : 0 };
        n->kv_row[i] = (struct uui_layout){ .dir = UUI_ROW, .items = n->kv_items[i], .count = 2 };
        n->kv_rows[rows++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->kv_row[i], .flags = UUI_FILL_W };
    }
    n->kv = (struct uui_layout){ .dir = UUI_COLUMN, .items = n->kv_rows, .count = rows, .gap = ch / 4,
                                 .gap_unit = 0 };
    n->text_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->head_l, .name = "needdll_head" };
    n->text_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->body_l, .flags = UUI_FILL_W,
                                          .name = "needdll_body" };
    n->text_items[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->kv, .flags = UUI_FILL_W };
    n->text_items[3] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->note_l, .flags = UUI_FILL_W,
                                          .name = "needdll_note" };
    n->text = (struct uui_layout){ .dir = UUI_COLUMN, .items = n->text_items, .count = 4 };
    n->top_items[0] = (struct uui_item){ .ops = &uui_image_ops, .widget = &n->icon, .main_size = ch * 3 };
    n->top_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->text, .flags = UUI_FILL_W };
    n->top = (struct uui_layout){ .dir = UUI_ROW, .items = n->top_items, .count = 2 };
    uui_button_init(&n->choose, 0, 0, 0, 0, "Choose file...", UUI_ROLE(accent), UUI_ROLE(accent_text), ND_CHOOSE);
    uui_button_init(&n->cancel, 0, 0, 0, 0, "Cancel", UUI_ROLE(control_bg), UUI_ROLE(text), ND_CANCEL);
    n->btn_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->gap, .flags = UUI_FILL_W };
    n->btn_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &n->choose, .id = ND_CHOOSE,
                                         .name = "needdll_choose" };
    n->btn_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &n->cancel, .id = ND_CANCEL,
                                         .name = "needdll_cancel" };
    n->btns = (struct uui_layout){ .dir = UUI_ROW, .items = n->btn_items, .count = 3 };
    n->items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->top, .flags = UUI_FILL_W | UUI_FILL_H };
    n->items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->gap };
    n->items[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->btns, .flags = UUI_FILL_W };
    n->root = (struct uui_layout){ .dir = UUI_COLUMN, .items = n->items, .count = 3 };
    n->focus_items[0] = (struct uui_focusable){ &n->choose, &uui_button_ops };
    n->focus_items[1] = (struct uui_focusable){ &n->cancel, &uui_button_ops };
    uui_focus_init(&n->focus, n->focus_items, 2);
    uui_focus_set(&n->focus, 0);
    int w = 0, h = 0;
    uui_layout_natural_size(&n->root, &w, &h);
    if (w < cw * 60) w = cw * 60;
    n->win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = n->title, .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = n->items, .widget_count = 3, .layout = &n->root, .focus = &n->focus,
        .on_action = nd_action, .on_key = nd_key, .on_close = nd_close,
        .log_prefix = "needdll",
    });
    ulogf("w16: needs %s.DLL%s%s: %s\n", mod, by[0] ? " for " : "", by, n->win ? "asked" : "no window to ask in");
    if (!n->win) done(a, 0);
}
