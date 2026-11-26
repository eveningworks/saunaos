// System Settings: System > Users (users D2b, mockup U1) -- the account,
// its password, what administrator prompts ask, and whether it signs in
// automatically. The page is the registry's Users group; this draws its
// cards in place of `login.autologin`'s text row, `login.admin_prompt`'s
// row among them, and each change is saved at once (the Remote Desktop
// page's rule): both live in root's /etc/login.conf and a password is
// privd's to keep (base/upriv.h), none a value Apply could stage.
//
// Under them, OTHER ACCOUNTS (users D3a, mockup S1 -- GNOME's Users panel
// and Windows' "Other users"): a row per person, their type and Remove...,
// and Add account...; each change is a privd action (base/uusers.h),
// asked of an administrator, so a standard account sees them greyed.
#include "apps/system/settings/settings_internal.h"
#include "base/upriv.h"
#include "base/usetting.h"
#include "base/uusers.h"
#include "ui/uui_newaccount.h"
#include "ui/uui_newpassword.h"
#include "ui/uui_switch.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SET_AUTOLOGIN "login.autologin"
#define SET_ADMIN_PROMPT "login.admin_prompt"

enum { U_SET = 1, U_CHANGE, U_REMOVE };   // what the buttons open
// The dialog's ids: its buttons' codes and its fields', unique in the window.
enum { UD_OK = 1, UD_CANCEL, UD_CURRENT, UD_NEW, UD_NEW_FIELD, UD_NEW_CONFIRM };

static char *g_acct = "", *g_real = "";   // owned once loaded; sized to the names
static char g_acct_desc[512], g_pw_desc[96];
static int g_names_owned;
static int g_has_pw = -1;

static struct uui_setting_row g_acct_row, g_pw_row, g_auto_row, g_ap_row;
static struct uui_dropdown g_ap_dd;
static int g_ap_slot = -1, g_ap_cur = -1;   // the registry's slot for it; its value's index
static struct uui_button g_set_b, g_change_b, g_remove_b;
static struct uui_item g_pw_btns[2];
static struct uui_layout g_pw_btn_row;
static struct uui_switch g_auto_sw;

// Above ID_CONTROL_BASE's slot ids (settings_internal.h).
#define ID_US_SET    900
#define ID_US_CHANGE 901
#define ID_US_REMOVE 902
#define ID_US_AUTO   903
#define ID_US_ADMIN  904
#define ID_US_ADD    905
#define ID_US_ROW    1000   // + 2 * row: its type drop-down; + 1, its Remove...

static int in_wheel(const char *name) {
    struct group *g = getgrnam("wheel");
    for (int i = 0; g && g->gr_mem && g->gr_mem[i]; i++)
        if (!strcmp(g->gr_mem[i], name)) return 1;
    return 0;
}

// --- other accounts ---------------------------------------------------------

static const char *const TYPE_NAMES[] = { "Standard", "Administrator" };

struct other {
    char *name, *real;
    int admin;
    char desc[160];
    char names[2][80];   // "users_type.<name>", "users_remove.<name>"
    struct uui_setting_row row;
    struct uui_dropdown type;
    struct uui_button remove;
    struct uui_item ctl[2];
    struct uui_layout ctl_row;
};

static struct other *g_oth;           // grown to the accounts there are
static int g_noth;
static struct uui_item *g_oth_items;  // one per row, the column's
static struct uui_layout g_oth_col;
static struct uui_setting_row g_oth_head;
static struct uui_button g_add_b;
static int g_me_admin;

static void others_free(void) {
    for (int i = 0; i < g_noth; i++) { free(g_oth[i].name); free(g_oth[i].real); }
    free(g_oth);
    free(g_oth_items);
    g_oth = 0;
    g_oth_items = 0;
    g_noth = 0;
}

// Every person but the one signed in, afresh from /etc/passwd.
static void others_load(const char *me) {
    others_free();
    struct uuser *u;
    int n = uusers_list(&u);
    if (n < 0) return;
    g_oth = calloc((size_t)(n ? n : 1), sizeof *g_oth);
    g_oth_items = calloc((size_t)(n ? n : 1), sizeof *g_oth_items);
    if (!g_oth || !g_oth_items) { free(g_oth); free(g_oth_items); g_oth = 0; g_oth_items = 0; uusers_free(u, n); return; }
    for (int i = 0; i < n; i++) {
        if (!strcmp(u[i].name, me)) continue;
        struct other *o = &g_oth[g_noth];
        o->name = u[i].name;
        o->real = u[i].real;
        u[i].name = u[i].real = 0;   // the row owns them now
        o->admin = u[i].admin;
        // Not whether it has a password: privd answers that for root and
        // the account itself only (unix_chkpwd's rule).
        snprintf(o->desc, sizeof o->desc, "%s -- %s", o->name, o->admin ? "Administrator" : "Standard");
        int id = ID_US_ROW + 2 * g_noth;
        uui_dropdown_init(&o->type, 0, 0, 0, 0, 0, 0);
        uui_dropdown_set_items(&o->type, TYPE_NAMES, 2);
        uui_dropdown_set_selected(&o->type, o->admin);
        o->type.disabled = !g_me_admin;
        uui_button_init(&o->remove, 0, 0, 0, 0, "Remove...", UUI_ROLE(control_bg), UUI_ROLE(text), id + 1);
        o->remove.disabled = !g_me_admin;
        snprintf(o->names[0], sizeof o->names[0], "users_type.%s", o->name);
        snprintf(o->names[1], sizeof o->names[1], "users_remove.%s", o->name);
        o->ctl[0] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &o->type, .id = id,
                                       .name = o->names[0] };
        o->ctl[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &o->remove, .id = id + 1,
                                       .name = o->names[1] };
        o->ctl_row = (struct uui_layout){ .dir = UUI_ROW, .items = o->ctl, .count = 2 };
        uui_setting_row_init(&o->row, o->real, o->desc,
                             (struct uui_item){ .ops = &uui_layout_ops, .widget = &o->ctl_row,
                                                .name = "users_other_controls" });
        g_oth_items[g_noth] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &o->row,
                                                 .flags = UUI_FILL_W, .name = "users_other" };
        g_noth++;
    }
    uusers_free(u, n);
    g_oth_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_oth_items, .count = g_noth };
    g_add_b.disabled = !g_me_admin;
    g_oth_head.desc = !g_me_admin ? "An administrator adds and removes the people who sign in here"
                    : g_noth ? "The people who also sign in on this machine"
                             : "Nobody else signs in on this machine yet";
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
    uui_dropdown_init(&g_ap_dd, 0, 0, 0, 0, 0, 0);
    uui_setting_row_init(&g_ap_row, "When an app needs administrator rights", "",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_ap_dd, .id = ID_US_ADMIN,
                                            .name = "users_admin_prompt" });
    uui_button_init(&g_add_b, 0, 0, 0, 0, "Add account...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_US_ADD);
    uui_setting_row_init(&g_oth_head, "Other accounts", "",
                         (struct uui_item){ .ops = &uui_button_ops, .widget = &g_add_b, .id = ID_US_ADD,
                                            .name = "users_add" });
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
    g_me_admin = in_wheel(g_acct);
    snprintf(g_acct_desc, sizeof g_acct_desc, "%s -- %s -- home %s", g_acct,
             g_me_admin ? "Administrator (in wheel)" : "Standard account", pw ? pw->pw_dir : "?");
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
    // The choices are the registry's (its slot's), the value read afresh:
    // the slot's own baseline is what the page opened with.
    g_ap_slot = slot_named(SET_ADMIN_PROMPT);
    g_ap_cur = -1;
    if (g_ap_slot >= 0) {
        struct slot *sl = &g_slot[g_ap_slot];
        char v[SETTING_ABI_VALUE_MAX] = "";
        usetting_get(SET_ADMIN_PROMPT, v, sizeof v);
        for (int c = 0; c < sl->choice_count; c++)
            if (!strcmp(sl->choice_raw[c], v)) g_ap_cur = c;
        uui_dropdown_set_items(&g_ap_dd, sl->choice_ptr, sl->choice_count);
        uui_dropdown_set_selected(&g_ap_dd, g_ap_cur < 0 ? 0 : g_ap_cur);
    }
    // Without a password there is nothing to ask but Allow.
    g_ap_dd.disabled = g_ap_row.disabled = g_has_pw != 1;
    g_ap_row.desc = g_has_pw == 1 ? "What the prompt asks of you at this machine; sudo always asks for the password"
                                  : "Needs a password -- without one, the prompt asks only Allow or Cancel";
    others_load(g_acct);
    ulogf("settings: users %s password %d autologin %d admin_prompt %d others %d\n", g_acct, g_has_pw,
          g_auto_sw.on, g_ap_cur, g_noth);
}

// In place of login.autologin's own row; -1 for every other slot.
int users_emit_slot(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (g_slot[i].setting < 0) return -1;
    // Its row is drawn among the cards below, not where the registry puts it.
    if (!strcmp(g_name[g_slot[i].setting], SET_ADMIN_PROMPT)) return n;
    if (strcmp(g_name[g_slot[i].setting], SET_AUTOLOGIN)) return -1;
    if (g_has_pw < 0) users_load();
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_acct_row,
                                  .flags = UUI_FILL_W, .name = "users_account" };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_pw_row,
                                  .flags = UUI_FILL_W, .name = "users_password" };
    for (int k = 0; k < g_pw_btn_row.count; k++)
        focus[(*nfocus)++] = (struct uui_focusable){ g_pw_btns[k].widget, &uui_button_ops };
    if (g_ap_slot >= 0) {
        out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_ap_row,
                                      .flags = UUI_FILL_W, .name = "users_admin" };
        focus[(*nfocus)++] = (struct uui_focusable){ &g_ap_dd, &uui_dropdown_ops };
    }
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_auto_row,
                                  .flags = UUI_FILL_W, .name = "users_auto" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_auto_sw, &uui_switch_ops };
    return n;
}

// OTHER ACCOUNTS, after the page's last row: everything above them is the
// signed-in person's own (the lock after the saver is a registry row of
// its own, emitted after login.autologin's).
int users_emit_after(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *nfocus) {
    if (i != g_slot_count - 1 || slot_named(SET_AUTOLOGIN) < 0) return n;
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_oth_head,
                                  .flags = UUI_FILL_W, .name = "users_others" };
    if (*nfocus < FOCUS_MAX) focus[(*nfocus)++] = (struct uui_focusable){ &g_add_b, &uui_button_ops };
    else ulogf("settings: users: focus ring full at Add account\n");
    if (g_noth) {
        // ONE item for every row (a column), so the page's own fixed list
        // holds any number of people; the focus ring is fixed too, and a
        // row past its end is still clicked, never tabbed to.
        out[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_oth_col, .flags = UUI_FILL_W,
                                      .name = "users_other_list" };
        for (int i = 0; i < g_noth; i++) {
            if (*nfocus + 2 > FOCUS_MAX) { ulogf("settings: users: focus ring full at row %d\n", i); break; }
            focus[(*nfocus)++] = (struct uui_focusable){ &g_oth[i].type, &uui_dropdown_ops };
            focus[(*nfocus)++] = (struct uui_focusable){ &g_oth[i].remove, &uui_button_ops };
        }
    }
    return n;
}

int users_fit(void) {
    int changed = 0;
    if (uui_setting_row_fit(&g_acct_row)) changed = 1;
    if (uui_setting_row_fit(&g_pw_row)) changed = 1;
    if (uui_setting_row_fit(&g_auto_row)) changed = 1;
    if (g_ap_slot >= 0 && uui_setting_row_fit(&g_ap_row)) changed = 1;
    if (uui_setting_row_fit(&g_oth_head)) changed = 1;
    for (int i = 0; i < g_noth; i++)
        if (uui_setting_row_fit(&g_oth[i].row)) changed = 1;
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
        .log_prefix = "settings.users", .default_button = &g_ud_ok,
    });
    ulogf("settings: users dialog %s %s\n", mode == U_REMOVE ? "remove" : mode == U_SET ? "set" : "change",
          g_ud_win ? "open" : "FAILED");
}

// --- Add account... and Remove... -------------------------------------------
//
// Each runs its privd action off the frame -- the prompt may wait on the
// person -- with the strings copied, as the page may reload meanwhile.

enum { AD_ADD = 1, AD_CANCEL, AD_FULL, AD_NAME, AD_TYPE, AD_PW, AD_CONFIRM, AD_KEEP, AD_RM_OK, AD_RM_CANCEL };

static struct uapp_window *g_ad_win;
static struct uui_newaccount g_na;
static struct uui_label g_ad_msg, g_ad_gap;
static struct uui_button g_ad_ok, g_ad_cancel;
static struct uui_item AD_BTNS[3], AD[3];
static struct uui_layout AD_BTN_ROW, AD_LAYOUT;
static struct uui_focusable AD_FOCUS[7];
static struct uui_focus g_ad_focus;
static char g_ad_msg_text[128];

// The worker's copies; the password wiped after.
static char g_w_name[64], g_w_full[256], g_w_val[8];
static char *g_w_pw;
static int g_w_admin, g_w_busy;

static const char *acct_err(int e) {
    return e == EEXIST    ? "That name is taken."
         : e == EBUSY     ? "The account is signed in, or has programs running."
         : e == EMLINK    ? "It is the last administrator."
         : e == ECANCELED ? "Not allowed."
         : e == EPERM     ? "Only an administrator at this machine may do that."
         : e == E2BIG     ? "The full name is too long."
         : "privd could not do it.";
}

static void ad_refresh(void) {
    g_ad_ok.disabled = g_w_busy || uui_newaccount_state(&g_na) != UUI_NEWACCT_READY;
}

static void ad_close(struct uapp_window *w) {
    uui_newaccount_clear(&g_na);
    uapp_window_close(w);
    if (g_app) uapp_redraw(g_app);
}

static int add_work(void *arg) {
    (void)arg;
    return uusers_add(g_w_name, g_w_full, g_w_admin, g_w_pw ? g_w_pw : "", 0) < 0 ? -errno : 0;
}

static void add_done(struct uapp *a, int rc, int err, void *arg) {
    (void)arg; (void)err;
    wipe(&g_w_pw);
    g_w_busy = 0;
    ulogf("settings: users add %s admin %d result %d\n", g_w_name, g_w_admin, rc);
    if (rc && g_ad_win && uapp_window_is_open(g_ad_win)) {
        snprintf(g_ad_msg_text, sizeof g_ad_msg_text, "%s", acct_err(-rc));
        ad_refresh();
        uapp_window_redraw(g_ad_win);
        return;
    }
    if (!rc) snprintf(g_status, sizeof g_status, "%s can sign in now.", g_w_full[0] ? g_w_full : g_w_name);
    if (g_ad_win && uapp_window_is_open(g_ad_win)) ad_close(g_ad_win);
    users_load();
    relayout_page();
    uapp_redraw(a);
}

static void ad_ok(struct uapp_window *w) {
    ad_refresh();
    if (g_ad_ok.disabled) return;
    const char *pw = uui_newaccount_password(&g_na);
    snprintf(g_w_name, sizeof g_w_name, "%s", uui_newaccount_name(&g_na));
    snprintf(g_w_full, sizeof g_w_full, "%s", uui_newaccount_full(&g_na));
    g_w_admin = uui_newaccount_admin(&g_na);
    g_w_pw = strdup(pw ? pw : "");
    if (!g_w_pw) return;
    g_w_busy = 1;
    g_ad_msg_text[0] = '\0';
    if (uapp_run_then(g_app, add_work, 0, add_done) != 0) {
        wipe(&g_w_pw);
        g_w_busy = 0;
        snprintf(g_ad_msg_text, sizeof g_ad_msg_text, "Could not ask privd.");
    }
    ad_refresh();
    uapp_window_redraw(w);
}

static void ad_on_action(struct uapp_window *w, int code) {
    if (code == AD_ADD) ad_ok(w);
    if (code == AD_CANCEL) ad_close(w);
}

// Every change, a key in any field included: the name follows the full
// name, and Add greys until the form is ready.
static void ad_changed(struct uapp_window *w) {
    uui_newaccount_changed(&g_na);
    if (!g_w_busy) g_ad_msg_text[0] = '\0';
    ad_refresh();
    uapp_window_redraw(w);
}
static void ad_on_widget(struct uapp_window *w, int id, int reason) { (void)id; (void)reason; ad_changed(w); }
// A field inside the form is not one of the window's own items, so its
// keys reach on_key, not on_widget (as the password dialog's do).
static void ad_on_key(struct uapp_window *w, int key, unsigned mods) { (void)key; (void)mods; ad_changed(w); }

static void users_open_add(struct uapp *a) {
    if (g_ad_win && uapp_window_is_open(g_ad_win)) return;
    uui_newaccount_init(&g_na);
    uui_newaccount_set_ids(&g_na, AD_FULL, AD_NAME, AD_TYPE, AD_PW, AD_CONFIRM);
    g_ad_msg_text[0] = '\0';
    uui_label_init(&g_ad_msg, g_ad_msg_text);
    g_ad_msg.fg = utheme_severity(UTHEME_SEV_ERROR);
    uui_label_init(&g_ad_gap, "");
    uui_button_init(&g_ad_ok, 0, 0, 0, 0, "Add", UUI_ROLE(accent), UUI_ROLE(accent_text), AD_ADD);
    uui_button_init(&g_ad_cancel, 0, 0, 0, 0, "Cancel", UUI_ROLE(control_bg), UUI_ROLE(text), AD_CANCEL);
    AD_BTNS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_ad_gap, .flags = UUI_FILL_W };
    AD_BTNS[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ad_ok, .id = AD_ADD, .name = "users_add_ok" };
    AD_BTNS[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ad_cancel, .id = AD_CANCEL,
                                    .name = "users_add_cancel" };
    AD_BTN_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = AD_BTNS, .count = 3 };
    AD[0] = (struct uui_item){ .ops = &uui_newaccount_ops, .widget = &g_na, .flags = UUI_FILL_W,
                               .name = "users_newaccount" };
    AD[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_ad_msg, .flags = UUI_FILL_W,
                               .name = "users_add_message" };
    AD[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &AD_BTN_ROW, .flags = UUI_FILL_W };
    AD_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = AD, .count = 3 };
    int nf = uui_newaccount_focusables(&g_na, AD_FOCUS);
    AD_FOCUS[nf++] = (struct uui_focusable){ &g_ad_ok, &uui_button_ops };
    AD_FOCUS[nf++] = (struct uui_focusable){ &g_ad_cancel, &uui_button_ops };
    uui_focus_init(&g_ad_focus, AD_FOCUS, nf);
    uui_focus_set(&g_ad_focus, 0);
    ad_refresh();
    int w = 0, h = 0;
    uui_layout_natural_size(&AD_LAYOUT, &w, &h);
    if (w < ugfx_char_w() * 52) w = ugfx_char_w() * 52;
    g_ad_win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = "Add account", .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = AD, .widget_count = 3, .layout = &AD_LAYOUT, .focus = &g_ad_focus,
        .on_widget = ad_on_widget, .on_action = ad_on_action, .on_key = ad_on_key, .on_close = ad_close,
        .log_prefix = "settings.users", .default_button = &g_ad_ok,
    });
    ulogf("settings: users add dialog %s\n", g_ad_win ? "open" : "FAILED");
}

// Remove...: whose, and whether the home goes too (Windows' "Delete
// account and data", GNOME's Delete Files / Keep Files).
static struct uapp_window *g_rm_win;
static struct uui_label g_rm_text, g_rm_msg, g_rm_gap;
static struct uui_radio_list g_rm_files;
static const char *g_rm_opts[2];
static char g_rm_keep[160], g_rm_line[192], g_rm_msg_text[128];
static struct uui_button g_rm_ok, g_rm_cancel;
static struct uui_item RM_BTNS[3], RM[4];
static struct uui_layout RM_BTN_ROW, RM_LAYOUT;
static struct uui_focusable RM_FOCUS[3];
static struct uui_focus g_rm_focus;

static void rm_close(struct uapp_window *w) {
    uapp_window_close(w);
    if (g_app) uapp_redraw(g_app);
}

static int remove_work(void *arg) {
    (void)arg;
    return uusers_remove(g_w_name, !strcmp(g_w_val, "delete"), 0) < 0 ? -errno : 0;
}

static void remove_done(struct uapp *a, int rc, int err, void *arg) {
    (void)arg; (void)err;
    g_w_busy = 0;
    ulogf("settings: users remove %s %s result %d\n", g_w_name, g_w_val, rc);
    if (rc && g_rm_win && uapp_window_is_open(g_rm_win)) {
        snprintf(g_rm_msg_text, sizeof g_rm_msg_text, "%s", acct_err(-rc));
        g_rm_ok.disabled = 0;
        uapp_window_redraw(g_rm_win);
        return;
    }
    if (!rc) snprintf(g_status, sizeof g_status, "%s was removed.", g_w_name);
    if (g_rm_win && uapp_window_is_open(g_rm_win)) rm_close(g_rm_win);
    users_load();
    relayout_page();
    uapp_redraw(a);
}

static void rm_on_action(struct uapp_window *w, int code) {
    if (code == AD_RM_CANCEL) { rm_close(w); return; }
    if (code != AD_RM_OK || g_w_busy) return;
    snprintf(g_w_val, sizeof g_w_val, "%s", g_rm_files.selected == 1 ? "delete" : "keep");
    g_w_busy = 1;
    g_rm_ok.disabled = 1;
    g_rm_msg_text[0] = '\0';
    if (uapp_run_then(g_app, remove_work, 0, remove_done) != 0) {
        g_w_busy = 0;
        g_rm_ok.disabled = 0;
        snprintf(g_rm_msg_text, sizeof g_rm_msg_text, "Could not ask privd.");
    }
    uapp_window_redraw(w);
}

static void users_open_remove(struct uapp *a, const struct other *o) {
    if ((g_rm_win && uapp_window_is_open(g_rm_win)) || g_w_busy) return;
    snprintf(g_w_name, sizeof g_w_name, "%s", o->name);
    struct passwd *pw = getpwnam(o->name);
    snprintf(g_rm_line, sizeof g_rm_line, "Remove %s? %s can no longer sign in.", o->real, o->name);
    snprintf(g_rm_keep, sizeof g_rm_keep, "Keep the files in %s", pw && pw->pw_dir ? pw->pw_dir : "the home");
    g_rm_opts[0] = g_rm_keep;
    g_rm_opts[1] = "Delete the files";
    uui_label_init(&g_rm_text, g_rm_line);
    g_rm_msg_text[0] = '\0';
    uui_label_init(&g_rm_msg, g_rm_msg_text);
    g_rm_msg.fg = utheme_severity(UTHEME_SEV_ERROR);
    uui_label_init(&g_rm_gap, "");
    memset(&g_rm_files, 0, sizeof g_rm_files);
    g_rm_files.options = g_rm_opts;
    g_rm_files.count = 2;
    g_rm_files.cols = 1;
    g_rm_files.row_h = ugfx_char_h() + ugfx_px(8);
    g_rm_files.col_w = ugfx_text_width(g_rm_keep) + ugfx_char_h() * 2;
    g_rm_files.marker_size = ugfx_char_h() - 4;
    g_rm_files.selected = 0;
    g_rm_files.hovered = g_rm_files.armed_prev = -1;
    g_rm_files.bg = g_rm_files.fg = UUI_COLOR_UNSET;
    uui_button_init(&g_rm_ok, 0, 0, 0, 0, "Remove", UUI_ROLE(accent), UUI_ROLE(accent_text), AD_RM_OK);
    uui_button_init(&g_rm_cancel, 0, 0, 0, 0, "Cancel", UUI_ROLE(control_bg), UUI_ROLE(text), AD_RM_CANCEL);
    RM_BTNS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_rm_gap, .flags = UUI_FILL_W };
    RM_BTNS[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_rm_ok, .id = AD_RM_OK,
                                    .name = "users_remove_ok" };
    RM_BTNS[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_rm_cancel, .id = AD_RM_CANCEL,
                                    .name = "users_remove_cancel" };
    RM_BTN_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = RM_BTNS, .count = 3 };
    RM[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_rm_text, .flags = UUI_FILL_W,
                               .name = "users_remove_text" };
    RM[1] = (struct uui_item){ .ops = &uui_radio_list_ops, .widget = &g_rm_files, .id = AD_KEEP,
                               .name = "users_remove_files" };
    RM[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_rm_msg, .flags = UUI_FILL_W,
                               .name = "users_remove_message" };
    RM[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &RM_BTN_ROW, .flags = UUI_FILL_W };
    RM_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = RM, .count = 4 };
    RM_FOCUS[0] = (struct uui_focusable){ &g_rm_files, &uui_radio_list_ops };
    RM_FOCUS[1] = (struct uui_focusable){ &g_rm_ok, &uui_button_ops };
    RM_FOCUS[2] = (struct uui_focusable){ &g_rm_cancel, &uui_button_ops };
    uui_focus_init(&g_rm_focus, RM_FOCUS, 3);
    uui_focus_set(&g_rm_focus, 0);
    int w = 0, h = 0;
    uui_layout_natural_size(&RM_LAYOUT, &w, &h);
    if (w < ugfx_char_w() * 48) w = ugfx_char_w() * 48;
    g_rm_win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = "Remove account", .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = RM, .widget_count = 4, .layout = &RM_LAYOUT, .focus = &g_rm_focus,
        .on_action = rm_on_action, .on_close = rm_close,
        .log_prefix = "settings.users", .default_button = &g_rm_ok,
    });
    ulogf("settings: users remove dialog %s %s\n", o->name, g_rm_win ? "open" : "FAILED");
}

// A row's type drop-down, chosen: Standard or Administrator, through privd.
static int type_work(void *arg) {
    (void)arg;
    return uusers_set(g_w_name, "admin", g_w_val, 0) < 0 ? -errno : 0;
}

static void type_done(struct uapp *a, int rc, int err, void *arg) {
    (void)arg; (void)err;
    g_w_busy = 0;
    ulogf("settings: users type %s admin %s result %d\n", g_w_name, g_w_val, rc);
    if (rc) snprintf(g_status, sizeof g_status, "%s's type was not changed: %s", g_w_name, acct_err(-rc));
    else snprintf(g_status, sizeof g_status, "%s is %s now.", g_w_name,
                  !strcmp(g_w_val, "1") ? "an administrator" : "a standard account");
    users_load();
    relayout_page();
    uapp_redraw(a);
}

// --- the page's events -------------------------------------------------------

int users_on_action(int code) {
    if (code == ID_US_SET) { users_open_dialog(g_app, U_SET); return 1; }
    if (code == ID_US_CHANGE) { users_open_dialog(g_app, U_CHANGE); return 1; }
    if (code == ID_US_REMOVE) { users_open_dialog(g_app, U_REMOVE); return 1; }
    if (code == ID_US_ADD) { users_open_add(g_app); return 1; }
    int row = (code - ID_US_ROW) / 2;
    if (code >= ID_US_ROW && (code - ID_US_ROW) % 2 == 1 && row < g_noth) {
        users_open_remove(g_app, &g_oth[row]);
        return 1;
    }
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

// login.admin_prompt, also root's: privd asks first -- with the password --
// when the choice asks LESS than the one in force, and not when it asks more.
// Copied when asked: the page may change, and g_slot with it, while
// the prompt waits.
static char g_ap_want[SETTING_ABI_VALUE_MAX], g_ap_want_label[SETTING_ABI_VALUE_MAX];

static int admin_prompt_work(void *arg) {
    (void)arg;
    const char *args[] = { g_ap_want, 0 };
    return upriv_action(UPRIV_ACT_ADMINPROMPT, args, 0) < 0 ? -errno : 0;
}

static void admin_prompt_done(struct uapp *a, int rc, int err, void *arg) {
    (void)arg; (void)err;
    snprintf(g_status, sizeof g_status, rc ? "What administrator prompts ask was not changed."
             : "Administrator prompts: %s.", g_ap_want_label);
    ulogf("settings: users admin_prompt %s result %d\n", g_ap_want, rc);
    users_load();
    relayout_page();
    uapp_redraw(a);
}

int users_on_widget(int id) {
    if (id == ID_US_ADMIN) {
        // The choice is made when the list CLOSES: each arrow while it is
        // open is a change too, and each would ask privd -- and the person.
        if (g_ap_dd.open) return 1;
        int sel = uui_dropdown_selected(&g_ap_dd);
        if (g_ap_slot < 0 || sel < 0 || sel >= g_slot[g_ap_slot].choice_count || sel == g_ap_cur) return 1;
        snprintf(g_ap_want, sizeof g_ap_want, "%s", g_slot[g_ap_slot].choice_raw[sel]);
        snprintf(g_ap_want_label, sizeof g_ap_want_label, "%s", g_slot[g_ap_slot].choice[sel]);
        if (uapp_run_then(g_app, admin_prompt_work, 0, admin_prompt_done) != 0) users_load();
        return 1;
    }
    int row = (id - ID_US_ROW) / 2;
    if (id >= ID_US_ROW && (id - ID_US_ROW) % 2 == 0 && row < g_noth) {
        struct other *o = &g_oth[row];
        if (o->type.open || g_w_busy) return 1;   // chosen when the list closes, as above
        int want = uui_dropdown_selected(&o->type) == 1;
        if (want == o->admin) return 1;
        snprintf(g_w_name, sizeof g_w_name, "%s", o->name);
        snprintf(g_w_val, sizeof g_w_val, "%s", want ? "1" : "0");
        g_w_busy = 1;
        if (uapp_run_then(g_app, type_work, 0, type_done) != 0) { g_w_busy = 0; users_load(); }
        return 1;
    }
    if (id != ID_US_AUTO) return 0;
    g_auto_want = g_auto_sw.on;
    if (uapp_run_then(g_app, autologin_work, 0, autologin_done) != 0) users_load();
    return 1;
}
