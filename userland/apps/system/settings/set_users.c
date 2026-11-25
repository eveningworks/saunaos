// System Settings: System > Users (users D2b, mockup U1) -- the account,
// its password and whether it signs in automatically. The page is the
// registry's Users group, `login.autologin` its one setting; this draws
// it as three cards in place of that setting's text row, and each change
// is saved at once (the Remote Desktop page's rule): a password is
// privd's to keep (base/upriv.h), not a value Apply could stage.
//
// One account until users D3, which lists the others here.
#include "apps/system/settings/settings_internal.h"
#include "base/upriv.h"
#include "base/usetting.h"
#include "ui/uui_newpassword.h"
#include "ui/uui_switch.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SET_AUTOLOGIN "login.autologin"

enum { U_SET = 1, U_CHANGE, U_REMOVE };   // what the buttons open
// The dialog's ids: its buttons' codes and its fields', unique in the window.
enum { UD_OK = 1, UD_CANCEL, UD_CURRENT, UD_NEW, UD_NEW_FIELD, UD_NEW_CONFIRM };

static char *g_acct = "", *g_real = "";   // owned once loaded; sized to the names
static char g_acct_desc[512], g_pw_desc[96];
static int g_names_owned;
static int g_has_pw = -1;

static struct uui_setting_row g_acct_row, g_pw_row, g_auto_row;
static struct uui_button g_set_b, g_change_b, g_remove_b;
static struct uui_item g_pw_btns[2];
static struct uui_layout g_pw_btn_row;
static struct uui_switch g_auto_sw;

// Above ID_CONTROL_BASE's slot ids (settings_internal.h).
#define ID_US_SET    900
#define ID_US_CHANGE 901
#define ID_US_REMOVE 902
#define ID_US_AUTO   903

static int in_wheel(const char *name) {
    struct group *g = getgrnam("wheel");
    for (int i = 0; g && g->gr_mem && g->gr_mem[i]; i++)
        if (!strcmp(g->gr_mem[i], name)) return 1;
    return 0;
}

void users_init(void) {
    uui_button_init(&g_set_b, 0, 0, 0, 0, "Set password...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_US_SET);
    uui_button_init(&g_change_b, 0, 0, 0, 0, "Change...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_US_CHANGE);
    uui_button_init(&g_remove_b, 0, 0, 0, 0, "Remove...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_US_REMOVE);
    g_pw_btn_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_pw_btns, .count = 1 };
    uui_setting_row_init(&g_acct_row, g_real, g_acct_desc, (struct uui_item){ 0 });
    uui_setting_row_init(&g_pw_row, "Password", g_pw_desc,
                         (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_pw_btn_row,
                                            .name = "users_pw_buttons" });
    uui_switch_init(&g_auto_sw, 0);
    uui_setting_row_init(&g_auto_row, "Sign in automatically",
                         "Start the desktop at boot without the login screen",
                         (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_auto_sw, .id = ID_US_AUTO,
                                            .name = "users_autologin" });
}

// The page opened again: whether there is a password is asked afresh.
void users_forget(void) { g_has_pw = -1; }

void users_load(void) {
    struct passwd *pw = getpwuid(getuid());
    char *acct = strdup(pw ? pw->pw_name : "?");
    char *real = strdup(pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos : pw ? pw->pw_name : "?");
    if (!acct || !real) { free(acct); free(real); return; }
    if (g_names_owned) { free(g_acct); free(g_real); }
    g_acct = acct;
    g_real = real;
    g_names_owned = 1;
    char *comma = strchr(g_real, ',');
    if (comma) *comma = '\0';
    g_acct_row.title = g_real;   // the row keeps the pointer it was given
    snprintf(g_acct_desc, sizeof g_acct_desc, "%s -- %s -- home %s", g_acct,
             in_wheel(g_acct) ? "Administrator (in wheel)" : "Standard account", pw ? pw->pw_dir : "?");
    g_has_pw = upriv_has_password(g_acct);
    snprintf(g_pw_desc, sizeof g_pw_desc, "%s",
             g_has_pw == 1 ? "Set -- the login screen, sudo and administrator changes ask for it"
             : g_has_pw == 0 ? "Not set -- this account signs in without one"
             : "Unknown -- privd did not answer");
    if (g_has_pw == 1) {
        g_pw_btns[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_change_b, .id = ID_US_CHANGE,
                                          .name = "users_change" };
        g_pw_btns[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_remove_b, .id = ID_US_REMOVE,
                                          .name = "users_remove" };
        g_pw_btn_row.count = 2;
    } else {
        g_pw_btns[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_set_b, .id = ID_US_SET,
                                          .name = "users_set" };
        g_pw_btn_row.count = 1;
    }
    char who[64] = "";
    if (!usetting_get(SET_AUTOLOGIN, who, sizeof who)) snprintf(who, sizeof who, "user");
    g_auto_sw.on = !strcmp(who, g_acct);
    ulogf("settings: users %s password %d autologin %d\n", g_acct, g_has_pw, g_auto_sw.on);
}

// In place of login.autologin's own row; -1 for every other slot.
int users_emit_slot(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (g_slot[i].setting < 0 || strcmp(g_name[g_slot[i].setting], SET_AUTOLOGIN)) return -1;
    if (g_has_pw < 0) users_load();
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_acct_row,
                                  .flags = UUI_FILL_W, .name = "users_account" };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_pw_row,
                                  .flags = UUI_FILL_W, .name = "users_password" };
    for (int k = 0; k < g_pw_btn_row.count; k++)
        focus[(*nfocus)++] = (struct uui_focusable){ g_pw_btns[k].widget, &uui_button_ops };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_auto_row,
                                  .flags = UUI_FILL_W, .name = "users_auto" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_auto_sw, &uui_switch_ops };
    return n;
}

int users_fit(void) {
    int changed = 0;
    if (uui_setting_row_fit(&g_acct_row)) changed = 1;
    if (uui_setting_row_fit(&g_pw_row)) changed = 1;
    if (uui_setting_row_fit(&g_auto_row)) changed = 1;
    return changed;
}

// --- the dialog: Set, Change or Remove -------------------------------------

static int g_mode;
static struct uapp_window *g_ud_win;
static struct uui_textbox g_cur;
static struct uui_newpassword g_np;
static struct uui_label g_cur_l, g_ud_msg, g_ud_gap;
static struct uui_button g_ud_ok, g_ud_cancel;
static struct uui_item UD_CUR[2], UD_BTNS[3], UD[4];
static struct uui_layout UD_CUR_ROW, UD_BTN_ROW, UD_LAYOUT;
static struct uui_focusable UD_FOCUS[6];
static struct uui_focus g_ud_focus;
static char g_ud_msg_text[96];
static char *g_w_cur, *g_w_new;   // the worker's copies, wiped after

static void ud_close(struct uapp_window *w) {
    uui_textbox_set_text(&g_cur, "");
    uui_newpassword_clear(&g_np);
    uapp_window_close(w);
    if (g_app) uapp_redraw(g_app);
}

// OK greys until what it acts on is there: Set and Change want the new
// password typed twice alike, Change and Remove the current one.
static void ud_refresh(void) {
    int cur_ok = g_mode == U_SET || uui_textbox_text(&g_cur)[0];
    int new_ok = g_mode == U_REMOVE || uui_newpassword_state(&g_np) == UUI_NEWPW_MATCH;
    g_ud_ok.disabled = !(cur_ok && new_ok);
}

static int passwd_work(void *arg) {
    (void)arg;
    int rc = upriv_passwd(g_acct, g_w_cur, g_w_new ? g_w_new : "");
    return rc < 0 ? -errno : 0;
}

static void wipe(char **p) {
    if (*p) { memset(*p, 0, strlen(*p)); free(*p); *p = 0; }
}

static void passwd_done(struct uapp *a, int rc, int err, void *arg) {
    (void)arg; (void)err;
    wipe(&g_w_cur);
    wipe(&g_w_new);
    ulogf("settings: users password %s result %d\n",
          g_mode == U_REMOVE ? "remove" : g_mode == U_SET ? "set" : "change", rc);
    if (rc == -EACCES && g_ud_win && uapp_window_is_open(g_ud_win)) {
        snprintf(g_ud_msg_text, sizeof g_ud_msg_text, "The current password is wrong.");
        uui_textbox_set_text(&g_cur, "");
        g_ud_ok.disabled = 1;
        uapp_window_redraw(g_ud_win);
        return;
    }
    snprintf(g_status, sizeof g_status, "%s",
             rc ? "The password was not changed."
             : g_mode == U_REMOVE ? "The password was removed."
             : "The password was saved. The login screen asks for it from now on.");
    if (g_ud_win && uapp_window_is_open(g_ud_win)) ud_close(g_ud_win);
    users_load();
    relayout_page();
    uapp_redraw(a);
}

static void ud_ok(struct uapp_window *w) {
    ud_refresh();
    if (g_ud_ok.disabled || g_w_cur) return;   // nothing to act on, or already asking
    g_w_cur = strdup(g_mode == U_SET ? "" : uui_textbox_text(&g_cur));
    g_w_new = g_mode == U_REMOVE ? 0 : strdup(uui_newpassword_text(&g_np));
    if (!g_w_cur || (g_mode != U_REMOVE && !g_w_new)) { wipe(&g_w_cur); wipe(&g_w_new); return; }
    g_ud_msg_text[0] = '\0';
    g_ud_ok.disabled = 1;
    if (uapp_run_then(g_app, passwd_work, 0, passwd_done) != 0) {
        wipe(&g_w_cur);
        wipe(&g_w_new);
        snprintf(g_ud_msg_text, sizeof g_ud_msg_text, "Could not ask privd.");
    }
    uapp_window_redraw(w);
}

static void ud_on_action(struct uapp_window *w, int code) {
    if (code == UD_OK) ud_ok(w);
    if (code == UD_CANCEL) ud_close(w);
}

static void ud_on_widget(struct uapp_window *w, int id, int reason) {
    (void)id; (void)reason;
    g_ud_msg_text[0] = '\0';
    ud_refresh();
    uapp_window_redraw(w);
}

// EVERY KEY re-checks Set: a field inside the new-password pair is not one
// of the window's own items, so its change reaches on_key, not on_widget.
static void ud_on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)key; (void)mods;
    ud_refresh();
    uapp_window_redraw(w);
}

static void users_open_dialog(struct uapp *a, int mode) {
    if (g_ud_win && uapp_window_is_open(g_ud_win)) return;
    g_mode = mode;
    uui_textbox_init(&g_cur, "");
    g_cur.masked = 1;
    g_cur.ed.flags |= UUI_EDIT_MASKED;
    uui_newpassword_init(&g_np, "New password", "Confirm");
    uui_newpassword_set_ids(&g_np, UD_NEW_FIELD, UD_NEW_CONFIRM);   // each key re-checks OK (ud_on_widget)
    uui_label_init(&g_cur_l, "Current password");
    g_ud_msg_text[0] = '\0';
    uui_label_init(&g_ud_msg, g_ud_msg_text);
    g_ud_msg.fg = utheme_severity(UTHEME_SEV_ERROR);
    uui_label_init(&g_ud_gap, "");
    uui_button_init(&g_ud_ok, 0, 0, 0, 0, mode == U_REMOVE ? "Remove" : mode == U_SET ? "Set" : "Change",
                    UUI_ROLE(accent), UUI_ROLE(accent_text), UD_OK);
    uui_button_init(&g_ud_cancel, 0, 0, 0, 0, "Cancel", UUI_ROLE(control_bg), UUI_ROLE(text), UD_CANCEL);
    int cap = ugfx_text_width("Current password") + ugfx_char_w();

    UD_CUR[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cur_l, .main_size = cap };
    UD_CUR[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_cur, .flags = UUI_FILL_W,
                                   .id = UD_CURRENT, .name = "users_current" };
    UD_CUR_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = UD_CUR, .count = 2 };
    UD_BTNS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_ud_gap, .flags = UUI_FILL_W };
    UD_BTNS[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ud_ok, .id = UD_OK, .name = "users_ok" };
    UD_BTNS[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ud_cancel, .id = UD_CANCEL,
                                    .name = "users_cancel" };
    UD_BTN_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = UD_BTNS, .count = 3 };
    int n = 0, nf = 0;
    if (mode != U_SET) {
        UD[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &UD_CUR_ROW, .flags = UUI_FILL_W };
        UD_FOCUS[nf++] = (struct uui_focusable){ &g_cur, &uui_textbox_focus_ops };
    }
    if (mode != U_REMOVE) {
        UD[n++] = (struct uui_item){ .ops = &uui_newpassword_ops, .widget = &g_np, .flags = UUI_FILL_W,
                                     .id = UD_NEW, .name = "users_new" };
        nf += uui_newpassword_focusables(&g_np, &UD_FOCUS[nf]);
    }
    UD[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_ud_msg, .flags = UUI_FILL_W,
                                 .name = "users_message" };
    UD[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &UD_BTN_ROW, .flags = UUI_FILL_W };
    UD_FOCUS[nf++] = (struct uui_focusable){ &g_ud_ok, &uui_button_ops };
    UD_FOCUS[nf++] = (struct uui_focusable){ &g_ud_cancel, &uui_button_ops };
    UD_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = UD, .count = n };
    uui_focus_init(&g_ud_focus, UD_FOCUS, nf);
    uui_focus_set(&g_ud_focus, 0);   // the first field takes the keys at once
    ud_refresh();

    int w = 0, h = 0;
    uui_layout_natural_size(&UD_LAYOUT, &w, &h);
    if (w < ugfx_char_w() * 44) w = ugfx_char_w() * 44;
    g_ud_win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = mode == U_REMOVE ? "Remove password" : mode == U_SET ? "Set password" : "Change password",
        .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = UD, .widget_count = n, .layout = &UD_LAYOUT, .focus = &g_ud_focus,
        .on_widget = ud_on_widget, .on_action = ud_on_action, .on_key = ud_on_key, .on_close = ud_close,
        .log_prefix = "settings.users",
    });
    ulogf("settings: users dialog %s %s\n", mode == U_REMOVE ? "remove" : mode == U_SET ? "set" : "change",
          g_ud_win ? "open" : "FAILED");
}

// --- the page's events -------------------------------------------------------

int users_on_action(int code) {
    if (code == ID_US_SET) { users_open_dialog(g_app, U_SET); return 1; }
    if (code == ID_US_CHANGE) { users_open_dialog(g_app, U_CHANGE); return 1; }
    if (code == ID_US_REMOVE) { users_open_dialog(g_app, U_REMOVE); return 1; }
    return 0;
}

// /etc/login.conf is root's: privd writes it (UPRIV_ACT_AUTOLOGIN), asking
// first when the switch goes ON -- off the frame, as the prompt may wait.
static int g_auto_want;

static int autologin_work(void *arg) {
    (void)arg;
    const char *args[] = { g_auto_want ? g_acct : "", 0 };
    return upriv_action(UPRIV_ACT_AUTOLOGIN, args, 0) < 0 ? -errno : 0;
}

static void autologin_done(struct uapp *a, int rc, int err, void *arg) {
    (void)arg; (void)err;
    snprintf(g_status, sizeof g_status, rc ? "Automatic sign-in was not changed."
             : g_auto_want ? "%s signs in automatically at boot." : "The login screen comes up at boot.",
             g_real);
    ulogf("settings: users autologin %d result %d\n", g_auto_want, rc);
    users_load();
    relayout_page();
    uapp_redraw(a);
}

int users_on_widget(int id) {
    if (id != ID_US_AUTO) return 0;
    g_auto_want = g_auto_sw.on;
    if (uapp_run_then(g_app, autologin_work, 0, autologin_done) != 0) users_load();
    return 1;
}
