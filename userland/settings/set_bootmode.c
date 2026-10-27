// System Settings: Display > Screen's Resolution on a display that cannot
// change mode after boot -- a VESA framebuffer. The size is GRUB's to set,
// so the row picks one for the next boot and "Restart to try" restarts
// into it once; /bin/wm/system/screentrial asks there whether to keep it
// (lib/ubootmode.h, mockup R2). Written at once, not staged for Apply: a
// restart is the only way it takes effect.
//
// Split out as set_startup.c is: GRUB's state, not a registry setting.
#include "settings/settings_internal.h"
#include "lib/ubootmode.h"
#include "lib/uinitctl.h"
#include "ui/ulog.h"
#include "rt/sys.h"

#define SET_RESOLUTION "system.resolution"
#define NSIZE 32     // "Automatic" and the size table, which is shorter

static struct ubootmode g_bm;
static int g_bm_ok;               // this page shows the boot-time row
static char g_bm_text[NSIZE][48];
static const char *g_bm_items[NSIZE];
static char g_bm_size[NSIZE][UBOOTMODE_SIZE];   // "" for Automatic
static int g_bm_count;
static char g_bm_now[112], g_bm_btn_text[48];

static struct uui_dropdown g_bm_pick;
static struct uui_label g_bm_now_l;
static struct uui_button g_bm_restart;
static struct uui_item g_bm_ctl[3];
static struct uui_layout g_bm_ctl_l;
static struct uui_setting_row g_bm_row;

static int chosen(void) {
    int k = uui_dropdown_selected(&g_bm_pick);
    return k >= 0 && k < g_bm_count ? k : 0;
}

// The line beside the button, and the button, from the choice.
static void refresh(void) {
    const char *size = g_bm_size[chosen()];
    int kept = !strcmp(size, g_bm.kept);
    int refused = size[0] && ubootmode_refused(size);
    int n = snprintf(g_bm_now, sizeof g_bm_now, "Now %dx%d", g_bm.now_w, g_bm.now_h);
    if (refused)
        snprintf(g_bm_now + n, sizeof g_bm_now - (size_t)n, ". %s is not offered by this display", size);
    else if (g_bm.pending[0])
        snprintf(g_bm_now + n, sizeof g_bm_now - (size_t)n, ". %s at the next restart", g_bm.pending);
    uui_label_set_text(&g_bm_now_l, g_bm_now);
    if (!size[0]) snprintf(g_bm_btn_text, sizeof g_bm_btn_text, "Restart with the firmware's size");
    else if (kept) snprintf(g_bm_btn_text, sizeof g_bm_btn_text, "Restart with %s", size);
    else snprintf(g_bm_btn_text, sizeof g_bm_btn_text, "Restart to try %s", size);
    g_bm_restart.label = g_bm_btn_text;
    // Nothing to restart for: the choice is what every boot gets already.
    g_bm_restart.disabled = refused || (kept && !g_bm.pending[0]);
}

void bootmode_page_opened(void) {
    g_bm_ok = 0;
    if (!slot_find(SET_RESOLUTION) || !ubootmode_read(&g_bm)) return;
    g_bm_ok = 1;
    g_bm_count = 0;
    snprintf(g_bm_text[0], sizeof g_bm_text[0], "Automatic");
    g_bm_size[0][0] = 0;
    g_bm_count = 1;
    int w, h, sel = 0;
    const char *want = g_bm.pending[0] ? g_bm.pending : g_bm.kept;
    for (int i = 0; g_bm_count < NSIZE && ubootmode_size_at(i, &w, &h); i++) {
        snprintf(g_bm_size[g_bm_count], UBOOTMODE_SIZE, "%dx%d", w, h);
        snprintf(g_bm_text[g_bm_count], sizeof g_bm_text[0], "%s%s", g_bm_size[g_bm_count],
                 ubootmode_refused(g_bm_size[g_bm_count]) ? "  (not offered)" : "");
        if (!strcmp(g_bm_size[g_bm_count], want)) sel = g_bm_count;
        g_bm_count++;
    }
    for (int i = 0; i < g_bm_count; i++) g_bm_items[i] = g_bm_text[i];
    uui_dropdown_set_items(&g_bm_pick, g_bm_items, g_bm_count);
    uui_dropdown_set_selected(&g_bm_pick, sel);
    refresh();
}

int bootmode_emit_slot(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (!g_bm_ok || g_slot[i].setting < 0 || strcmp(g_name[g_slot[i].setting], SET_RESOLUTION)) return -1;
    g_bm_ctl[0] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_bm_pick, .id = ID_BM_SIZE,
                                     .name = "bm_size" };
    g_bm_ctl[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_bm_now_l, .flags = UUI_FILL_W | UUI_FILL_H,
                                     .name = "bm_now" };
    g_bm_ctl[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_bm_restart, .id = ID_BM_RESTART,
                                     .name = "bm_restart" };
    g_bm_ctl_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_bm_ctl, .count = 3, .gap = 10 };
    g_bm_row.title = g_label[g_slot[i].setting];
    g_bm_row.stacked = 1;
    g_bm_row.control = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_bm_ctl_l, .flags = UUI_FILL_W };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_bm_row,
                                  .flags = UUI_FILL_W, .name = "bm_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_bm_pick, &uui_dropdown_ops };
    if (!g_bm_restart.disabled) focus[(*nfocus)++] = (struct uui_focusable){ &g_bm_restart, &uui_button_ops };
    return n;
}

int bootmode_on_widget(int id) {
    if (id != ID_BM_SIZE || !g_bm_ok) return 0;
    refresh();
    return 1;
}

int bootmode_on_action(int code) {
    if (code != ID_BM_RESTART || !g_bm_ok) return 0;
    const char *size = g_bm_size[chosen()];
    const char *why = 0;
    int rc = !strcmp(size, g_bm.kept) ? ubootmode_cancel()
                                       : ubootmode_try(size[0] ? size : 0, &why);
    if (rc < 0) {
        snprintf(g_status, sizeof g_status, "Could not set the size: %s", why ? why : "grubenv could not be written");
        bootmode_page_opened();
        return 1;
    }
    ulogf("settings: boot size %s, restarting\n", size[0] ? size : "auto");
    sys_sync();
    if (uinitctl_shutdown(1) < 0) snprintf(g_status, sizeof g_status, "Could not restart");
    return 1;
}

int bootmode_fit(void) { return g_bm_ok && uui_setting_row_fit(&g_bm_row); }

void bootmode_init(void) {
    uui_dropdown_init(&g_bm_pick, 0, 0, 0, 0, g_bm_items, 0);
    uui_label_init(&g_bm_now_l, "");
    uui_button_init(&g_bm_restart, 0, 0, 0, 0, "Restart", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_BM_RESTART);
    uui_setting_row_init(&g_bm_row, "Resolution",
                         "This display is set when the computer starts. A new size is tried once; "
                         "if you do not keep it, the old one comes back.",
                         (struct uui_item){ 0 });
}
