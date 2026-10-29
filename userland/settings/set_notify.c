// System Settings: Desktop > Notifications' "Apps" card -- every app that
// has notified, and for the one chosen whether it shows notifications,
// keeps them in the bell's history only, or neither. Windows' and KDE's
// per-app switches; the list is /etc/notify.conf's `app.<name>` lines,
// which the compositor writes the first time an app notifies
// (userland/wm/wm_notify.h). A choice is written AT ONCE, like the
// keyboard layouts list beside it on its own page -- it is not a staged
// setting, there being one per app.
// Split out as set_trash.c is: a page that needs more than its settings'
// generic rows.
#include "settings/settings_internal.h"
#include "lib/uconf.h"
#include "lib/ugrow.h"
#include "ui/uui_listbox.h"
#include "ui/uui_segmented.h"
#include <stdio.h>
#include <stdlib.h>

#define NOTIFY_CONF   "/etc/notify.conf"
#define SET_DND_FOR   "desktop.notifications.dnd_for"

static const char *const MODES[3]      = { "On", "History only", "Off" };
static const char *const MODE_VALUE[3] = { "on", "quiet", "off" };

struct nf_app { char key[48]; char row[80]; int mode; };
static struct nf_app *g_apps;      // grown on demand
static int g_napps, g_apps_cap;
static const char **g_rows;
static int g_rows_cap;
static int g_on_page;

static struct uui_listbox g_list;
static struct uui_segmented g_mode;
static struct uui_label g_empty;
static struct uui_item COL[2];
static struct uui_layout g_col;
static struct uui_setting_row g_row;

static int mode_of(const char *v) {
    return !strcmp(v, "off") ? 2 : !strcmp(v, "quiet") ? 1 : 0;
}

static void row_text(struct nf_app *a) {
    // The key holds `_` for what a key cannot; a person's name reads better with spaces.
    char name[48];
    snprintf(name, sizeof name, "%s", a->key + 4);
    for (char *p = name; *p; p++) if (*p == '_') *p = ' ';
    snprintf(a->row, sizeof a->row, "%s \xe2\x80\x94 %s", name, MODES[a->mode]);
}

// The `app.` lines of /etc/notify.conf, in file order.
static void load(void) {
    g_napps = 0;
    FILE *f = fopen(NOTIFY_CONF, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "app.", 4)) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!UGROW(g_apps, g_apps_cap, g_napps + 1)) break;
        struct nf_app *a = &g_apps[g_napps++];
        snprintf(a->key, sizeof a->key, "%s", line);
        a->mode = mode_of(v);
        row_text(a);
    }
    fclose(f);
    if (!UGROW(g_rows, g_rows_cap, g_napps + 1)) { g_napps = 0; return; }
    for (int i = 0; i < g_napps; i++) g_rows[i] = g_apps[i].row;
    uui_listbox_set_items(&g_list, g_rows, g_napps);
    if (g_list.selected >= g_napps) g_list.selected = g_napps - 1;
    if (g_list.selected < 0 && g_napps) g_list.selected = 0;
}

static void sync_mode(void) {
    int s = g_list.selected;
    g_mode.selected = s >= 0 && s < g_napps ? g_apps[s].mode : -1;
    g_mode.disabled = !(s >= 0 && s < g_napps);
}

void notify_init(void) {
    uui_listbox_init(&g_list, 0, 0, 0, 0, 0, 0);
    g_list.sel_bg = UUI_ROLE(accent);
    g_list.sel_fg = UUI_ROLE(accent_text);
    uui_segmented_init(&g_mode, MODES, 3, -1);
    uui_label_init(&g_empty, "No app has sent a notification yet.");
    g_row.desc_rows = 1;
}

void notify_page_opened(void) {
    g_on_page = slot_named(SET_DND_FOR) >= 0;
    if (!g_on_page) return;
    load();
    sync_mode();
}

int notify_emit_after(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (!g_on_page || i != slot_named(SET_DND_FOR)) return n;
    int rows = g_napps < 3 ? 3 : g_napps > 8 ? 8 : g_napps;   // past eight it scrolls
    g_list.h = rows * uui_listbox_row_h(&g_list) + 4;
    if (g_napps) {
        COL[0] = (struct uui_item){ .ops = &uui_listbox_ops, .widget = &g_list, .id = ID_NF_LIST,
                                    .flags = UUI_FILL_W, .main_size = g_list.h, .name = "nf_apps" };
        COL[1] = (struct uui_item){ .ops = &uui_segmented_ops, .widget = &g_mode, .id = ID_NF_MODE,
                                    .name = "nf_mode" };
        g_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = COL, .count = 2 };
    } else {
        COL[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_empty, .name = "nf_none" };
        g_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = COL, .count = 1 };
    }
    g_row.title = "Apps";
    g_row.desc = "Choose an app, then whether its notifications show, go to the bell's history only, or are dropped";
    g_row.stacked = 1;
    g_row.control = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_col, .flags = UUI_FILL_W };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_row,
                                  .flags = UUI_FILL_W, .name = "nf_apps_card" };
    if (g_napps) {
        focus[(*nfocus)++] = (struct uui_focusable){ &g_list, &uui_listbox_focus_ops };
        focus[(*nfocus)++] = (struct uui_focusable){ &g_mode, &uui_segmented_ops };
    }
    return n;
}

int notify_on_widget(int id) {
    if (id == ID_NF_LIST) { sync_mode(); return 1; }
    if (id != ID_NF_MODE) return 0;
    int s = g_list.selected;
    if (s < 0 || s >= g_napps || g_mode.selected < 0 || g_mode.selected > 2) return 1;
    struct nf_app *a = &g_apps[s];
    a->mode = g_mode.selected;
    uconf_set(NOTIFY_CONF, a->key, MODE_VALUE[a->mode]);
    ulogf("settings: notify %s=%s\n", a->key, MODE_VALUE[a->mode]);
    row_text(a);
    return 1;
}
