// Configuration Editor -- every setting and every kernel fact as one raw
// dotted tree, the values editable in place. System Settings is the easy
// view; this is the raw one: Windows' regedit in shape (branches on the
// left, the chosen branch's values in a table), GNOME's dconf-editor and
// Firefox's about:config in what it shows (where each value came from).
//
// WHAT IT SHOWS is what `config get` walks (base/usetting_tree.h): the
// merged registry -- the kernel's settings and the ones /etc/settings.d
// declares -- and the kernel's scalar facts. The data is
// confedit_model.c; this file is the window.
//
// WHAT EDITS (docs/roadmap-details.md, the maintainer's picks): a value
// of the person's own (`Scope=user`, written to ~/.config) and a setting
// the kernel registered, both through the registry's SET -- validated
// exactly as `config set` is, its three-way answer shown as it is. A
// machine-wide declared setting is READ-ONLY here until users stage D3
// puts machine writes behind privd; a fact never edits. Reset removes
// the person's value, so the machine's shows again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "keyboard.h"
#include "setting_abi.h"
#include "query_abi.h"     // QUERY_TYPE_BYTES
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "base/ulog.h"
#include "base/usetting.h"
#include "base/usetting_schema.h"
#include "media/uclip.h"
#include "ui/uui_menubar.h"
#include "ui/uui_builder.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_textbox.h"
#include "ui/uui_tree.h"
#include "ui/uui_table.h"
#include "ui/uui_splitter.h"
#include "ui/uui_props.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "confedit_internal.h"

#define ID_MENU    1
#define ID_TOOLBAR 2
#define ID_FIND    3
#define ID_TREE    4
#define ID_SPLIT   5
#define ID_TABLE   6
#define ID_DETAILS 7
#define ID_STATUS  8
#define ID_DIALOG  9

enum {
    CMD_REFRESH = 1, CMD_EXIT, CMD_EDIT, CMD_RESET, CMD_COPY_NAME, CMD_COPY_VALUE, CMD_FIND,
    CMD_EXPAND, CMD_COLLAPSE,
    DLG_SAVE, DLG_CANCEL,
};

static const struct uui_builder_name MENU_NAMES[] = {
    { "refresh", CMD_REFRESH }, { "exit", CMD_EXIT }, { "edit", CMD_EDIT }, { "reset", CMD_RESET },
    { "copy_name", CMD_COPY_NAME }, { "copy_value", CMD_COPY_VALUE }, { "find", CMD_FIND },
    { "expand", CMD_EXPAND }, { "collapse", CMD_COLLAPSE },
};

// COLOURED BY WHAT EACH DOES: changing a value, undoing one, copying.
static const struct uui_toolbar_item TB_ITEMS[] = {
    { "tb-pencil", "Edit the value (Enter)",          CMD_EDIT,       "Edit value...",      0, "Enter",  UTHEME_ACT_EDIT },
    { "tb-undo",   "Remove your value, so the machine's applies", CMD_RESET, "Reset to machine's", 0, 0, UTHEME_ACT_DANGER },
    UUI_TOOLBAR_SEP,
    { "tb-copy",   "Copy the name (Ctrl+C)",          CMD_COPY_NAME,  "Copy name",          0, "Ctrl+C", UTHEME_ACT_EDIT },
};

static const struct uui_table_column COLS[] = {
    { .title = "Name", .width_chars = 0 },
    { .title = "Value", .width_chars = 18 },
    { .title = "From", .width_chars = 9 },
    { .title = "Type", .width_chars = 6 },
};

static struct uui_menubar g_menu;
static struct uui_toolbar g_tb;
// THE FIND FIELD sits at the command bar's right end, laid over the
// strip: what it finds is a list of rows, so it replaces the branch's
// rows while it holds text (about:config's filter, regedit's Find as a
// list rather than a step).
static struct uui_textbox g_find;
static struct uui_tree g_tree;
static struct uui_splitter g_split;
static struct uui_table g_table;
static struct uui_props g_details;
static struct uui_statusbar g_status;
static struct uui_dialog g_dlg;
static struct uui_textbox g_field;   // the edit dialog's body

static struct uui_item g_field_item = { .ops = &uui_textbox_ops, .widget = &g_field, .name = "value" };

// The dialog LAST: an open one is on top, and the router asks the last
// item first. The find field after the toolbar it is drawn over.
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,    .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,      .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_textbox_ops,   .widget = &g_find,    .id = ID_FIND,    .name = "find" },
    { .ops = &uui_tree_ops,      .widget = &g_tree,    .id = ID_TREE,    .name = "tree" },
    { .ops = &uui_splitter_ops,  .widget = &g_split,   .id = ID_SPLIT,   .name = "split" },
    { .ops = &uui_table_ops,     .widget = &g_table,   .id = ID_TABLE,   .name = "values" },
    { .ops = &uui_props_ops,     .widget = &g_details, .id = ID_DETAILS, .name = "details" },
    { .ops = &uui_statusbar_ops, .widget = &g_status,  .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_dlg,     .id = ID_DIALOG,  .name = "dialog" },
};

static struct uui_focusable g_focusables[] = {
    { &g_tree, &uui_tree_ops }, { &g_table, &uui_table_ops }, { &g_find, &uui_textbox_ops },
};
static struct uui_focus g_focus;

// The tree's visible nodes, rebuilt whenever a branch opens or closes.
static struct uui_tree_node *g_nodes;
static int g_nnodes;

// THE SELECTED ROW'S INFO, read when the selection moves -- item_flags
// asks on every draw, and an INFO is a declaration read. STATIC: a
// setting_msg is over a kilobyte against ring 3's frame budget.
static struct setting_msg g_sel;
static int g_sel_entry = -1;
// Its merged index, from usetting_find() BY NAME: an index kept from the
// load shifts when a declaration comes or goes before the next reload.
static int g_sel_index = -1;

static char g_st_where[96], g_st_count[64], g_st_note[160];
static char g_dlg_row[2][160];
static const char *g_dlg_rows[2] = { g_dlg_row[0], g_dlg_row[1] };
static char g_dlg_note[200];
static char g_edit_name[SETTING_ABI_QUALIFIED_MAX];   // by NAME: a reload may run under the dialog

// --- what the selection can do ---------------------------------------------

static int sel_entry(void) {
    int r = g_table.selected;
    return r >= 0 && r < g_m.nrows ? g_m.rows[r] : -1;
}

static const struct usetting_entry *sel(void) {
    int i = sel_entry();
    return i >= 0 ? &g_m.e[i] : 0;
}

// Why the selected row cannot be edited, or NULL when it can.
static const char *why_not_edit(void) {
    int i = sel_entry();
    if (i < 0) return "Select a value first";
    const struct ce_info *in = ce_model_info(i);
    if (g_m.e[i].fact) return "A kernel fact is read-only";
    if (g_sel_entry == i && g_sel.unavailable[0]) return g_sel.unavailable;
    if (in->declared && !(in->scope == USCHEMA_SCOPE_USER && in->person))
        return "A machine-wide setting is read-only here";
    return 0;
}

static const char *why_not_reset(void) {
    int i = sel_entry();
    if (i < 0) return "Select a value first";
    const struct ce_info *in = ce_model_info(i);
    if (!in->declared) return "Only a value of your own can be reset";
    if (in->from != CE_FROM_USER) return "There is no value of your own to remove";
    return 0;
}

static unsigned item_flags(int code) {
    switch (code) {
    case CMD_EDIT:  return why_not_edit() ? UUI_MI_DISABLED : 0;
    case CMD_RESET: return why_not_reset() ? UUI_MI_DISABLED : 0;
    case CMD_COPY_NAME: case CMD_COPY_VALUE: return sel() ? 0 : UUI_MI_DISABLED;
    default: return 0;
    }
}

static const char *why(int code) {
    switch (code) {
    case CMD_EDIT: return why_not_edit();
    case CMD_RESET: return why_not_reset();
    case CMD_COPY_NAME: return sel() ? 0 : "Select a value first";
    default: return 0;
    }
}

// --- the tree ------------------------------------------------------------

static int sel_branch(void) { return uui_tree_selected_id(&g_tree); }

// EXPANSION IS THIS APP'S (UUI_TREE_CLOSED/OPEN), a flag per branch, so
// the tree is not bounded by the widget's collapsed bitmap.
static void rebuild_tree(int keep_branch) {
    free(g_nodes);
    g_nnodes = 0;
    g_nodes = g_m.nb ? calloc((size_t)g_m.nb, sizeof *g_nodes) : 0;
    if (!g_nodes) { uui_tree_set_nodes(&g_tree, 0, 0); return; }
    int hide_below = -1;
    for (int k = 0; k < g_m.nb; k++) {
        const struct ce_branch *b = &g_m.b[k];
        if (hide_below >= 0 && b->depth > hide_below) continue;
        hide_below = -1;
        struct uui_tree_node *t = &g_nodes[g_nnodes++];
        t->label = b->label;
        t->depth = b->depth;
        t->id = k;
        t->note = b->note[0] ? b->note : 0;
        if (b->kids) {
            t->kind = b->open ? UUI_TREE_OPEN : UUI_TREE_CLOSED;
            if (!b->open) hide_below = b->depth;
        }
    }
    uui_tree_set_nodes_keep(&g_tree, g_nodes, g_nnodes);
    if (keep_branch >= 0) uui_tree_select_id(&g_tree, keep_branch);
    // What a test clicks a branch by: its row and its name.
    if (g_nnodes <= 200)
        for (int r = 0; r < g_nnodes; r++)
            ulogf("confedit: tree row %d \"%s\"\n", r, g_m.b[g_nodes[r].id].name);
}

// Opens every branch above `k`, so it can be shown.
static void reveal(int k) {
    if (k < 0) return;
    char name[SETTING_ABI_QUALIFIED_MAX];
    strlcpy(name, g_m.b[k].name, sizeof name);
    for (char *dot = strrchr(name, '.'); dot; dot = strrchr(name, '.')) {
        *dot = '\0';
        int p = ce_model_branch_named(name);
        if (p >= 0) g_m.b[p].open = 1;
    }
}

// --- the details strip ---------------------------------------------------

// The values a setting accepts, as a sentence: what the edit dialog
// says, and what a refusal repeats.
static void accepted(char *out, size_t cap) {
    out[0] = '\0';
    const struct usetting_entry *e = sel();
    if (!e || e->fact || g_sel_entry != sel_entry()) return;
    if (g_sel.type == SETTING_ABI_TYPE_INT) {
        snprintf(out, cap, "A whole number from %d to %d%s%s", (int)g_sel.imin, (int)g_sel.imax,
                 g_sel.unit[0] ? " " : "", g_sel.unit);
        return;
    }
    if (g_sel.type == SETTING_ABI_TYPE_KEYCOMBO) { snprintf(out, cap, "A key combination, such as Ctrl+Alt+T"); return; }
    if (g_sel.type != SETTING_ABI_TYPE_ENUM || g_sel.count <= 0) { snprintf(out, cap, "Any text on one line"); return; }
    strlcpy(out, "One of: ", cap);
    static struct setting_msg ch;
    for (int c = 0; c < g_sel.count; c++) {
        memset(&ch, 0, sizeof ch);
        ch.op = SETTING_OP_CHOICE;
        ch.index = g_sel_index;
        ch.choice = c;
        if (usetting_dispatch(&ch) != 0) break;
        // What fits whole, then how many did not: a list of hundreds (the
        // time zones) is a count, not a wall of text.
        char more[32];
        int last = c + 1 == g_sel.count;
        snprintf(more, sizeof more, ", and %d more", g_sel.count - c);
        size_t n = strlen(out), need = strlen(ch.value) + (c ? 2 : 0);
        if (n + need + (last ? 0 : strlen(more)) >= cap) {
            if (n + strlen(more) < cap) strlcat(out, more, cap);
            return;
        }
        if (c) strlcat(out, ", ", cap);
        strlcat(out, ch.value, cap);
    }
}

static void update_details(void) {
    int i = sel_entry();
    uui_props_begin(&g_details, i == g_sel_entry);
    if (i != g_sel_entry) {
        memset(&g_sel, 0, sizeof g_sel);
        g_sel_entry = -1;
        if (i >= 0 && !g_m.e[i].fact) {
            g_sel_index = usetting_find(g_m.e[i].name, &g_sel);
            if (g_sel_index >= 0) g_sel_entry = i;
        } else if (i >= 0) {
            g_sel_entry = i;
        }
    }
    if (i < 0) return;
    const struct usetting_entry *e = &g_m.e[i];
    const struct ce_info *in = ce_model_info(i);
    if (e->fact) {
        uui_props_section(&g_details, "Kernel fact", UUI_PROPS_PLAIN);
        uui_props_row(&g_details, "Name", "%s", e->name);
        uui_props_row(&g_details, "Value", "%s", e->value);
        uui_props_row(&g_details, "", "Read-only -- %s the kernel reports, not stored in any file",
                      e->type == QUERY_TYPE_BYTES ? "a byte count" : "a count");
        return;
    }
    uui_props_section(&g_details, in->declared ? "Setting" : "Kernel setting", UUI_PROPS_PLAIN);
    uui_props_row(&g_details, "Name", "%s", e->name);
    if (g_sel.label[0]) uui_props_row(&g_details, "Label", "%s", g_sel.label);
    if (g_sel.description[0]) uui_props_row(&g_details, "", "%s", g_sel.description);
    uui_props_row(&g_details, "Value", "%s", e->value[0] ? e->value : "(empty)");
    if (in->declared) {
        static struct uschema s;
        struct uschema_origin o;
        memset(&o, 0, sizeof o);
        int have = uschema_find(e->name, &s);
        if (have) uschema_origin(&s, &o);
        if (in->scope == USCHEMA_SCOPE_USER) uui_props_row(&g_details, "You", "%s", o.has_user ? o.user : "--");
        uui_props_row(&g_details, "Machine", "%s  (%s)", o.has_machine ? o.machine : "--", have ? s.file : "?");
        uui_props_row(&g_details, "Default", "%s", have && s.def[0] ? s.def : "(empty)");
    }
    char acc[UUI_PROPS_VALUE];
    accepted(acc, sizeof acc);
    if (acc[0]) uui_props_row(&g_details, "Accepts", "%s", acc);
    if (in->declared && in->scope == USCHEMA_SCOPE_USER && in->person)
        uui_props_row(&g_details, "Scope", "Yours -- saved to %s", g_sel.file);
    else if (in->declared && in->scope == USCHEMA_SCOPE_USER)
        uui_props_row(&g_details, "Scope", "A person's setting -- this process has no person's layer");
    else if (in->declared)
        uui_props_row(&g_details, "Scope", "Machine-wide -- read-only here");
    else
        uui_props_row(&g_details, "Scope", "Machine-wide, the kernel's -- saved to %s", g_sel.file);
    if (g_sel.sflags & SETTING_ABI_SF_REBOOT) uui_props_row(&g_details, "Takes effect", "At the next restart");
    if (g_sel.unavailable[0]) uui_props_row(&g_details, "Not in effect", "%s", g_sel.unavailable);
}

// --- the table -----------------------------------------------------------

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (row < 0 || row >= g_m.nrows) return;
    int i = g_m.rows[row];
    const struct usetting_entry *e = &g_m.e[i];
    switch (col) {
    case 0:
        // Under a branch, a name is shown from the branch down: the
        // status bar says which branch.
        if (g_m.branch >= 0 && strcmp(e->name, g_m.b[g_m.branch].name)) {
            size_t len = strlen(g_m.b[g_m.branch].name);
            snprintf(out, (size_t)cap, "\xE2\x80\xA6%s", e->name + len + 1);
        } else {
            strlcpy(out, e->name, (size_t)cap);
        }
        break;
    case 1: strlcpy(out, e->value, (size_t)cap); break;
    case 2: strlcpy(out, ce_from_word(ce_model_info(i)->from), (size_t)cap); break;
    case 3: strlcpy(out, ce_type_word(e), (size_t)cap); break;
    }
}

// A value at its default is faded, as dconf-editor and about:config set
// a changed one apart: the eye finds what somebody chose.
static int fade(void *ctx, int row) {
    (void)ctx;
    return row >= 0 && row < g_m.nrows && ce_model_info(g_m.rows[row])->from == CE_FROM_DEFAULT;
}

// A dot before "You": the person's own values at a glance. The word
// says the same, so the dot is never the only cue.
static uint32_t dot(void *ctx, int row, int col) {
    (void)ctx;
    if (col != 2 || row < 0 || row >= g_m.nrows) return 0;
    return ce_model_info(g_m.rows[row])->from == CE_FROM_USER ? utheme_current()->accent : 0;
}

static void update_status(void) {
    if (g_m.find[0]) snprintf(g_st_where, sizeof g_st_where, "Find: %s", g_m.find);
    else if (g_m.branch >= 0) strlcpy(g_st_where, g_m.b[g_m.branch].name, sizeof g_st_where);
    else g_st_where[0] = '\0';
    int settings = 0, facts = 0, yours = 0;
    for (int r = 0; r < g_m.nrows; r++) {
        int i = g_m.rows[r];
        if (g_m.e[i].fact) facts++; else settings++;
        yours += ce_model_info(i)->from == CE_FROM_USER;
    }
    char f[24] = "";
    if (facts) snprintf(f, sizeof f, "%s%d fact%s", settings ? ", " : "", facts, facts == 1 ? "" : "s");
    char s[24] = "";
    if (settings || !facts) snprintf(s, sizeof s, "%d setting%s", settings, settings == 1 ? "" : "s");
    snprintf(g_st_count, sizeof g_st_count, "%s%s, %d changed by you", s, f, yours);
}

// The rows for the tree's branch (or the find), the selection kept on
// the entry named `keep` when it is still listed.
static void show_rows(const char *keep) {
    ce_model_rows(sel_branch(), uui_textbox_text(&g_find));
    uui_table_set_rows(&g_table, g_m.nrows);
    int pick = 0;
    if (keep && keep[0])
        for (int r = 0; r < g_m.nrows; r++)
            if (!strcmp(g_m.e[g_m.rows[r]].name, keep)) { pick = r; break; }
    g_table.selected = g_m.nrows ? pick : -1;
    if (pick == 0) g_table.top = 0;
    update_details();
    update_status();
}

// --- loading -------------------------------------------------------------

static void load(const char *keep_branch, const char *keep_entry) {
    unsigned long long t0 = sys_monotonic_ns();
    if (ce_model_load() != 0) {
        snprintf(g_st_note, sizeof g_st_note, "The settings could not be read");
        return;
    }
    g_sel_entry = -1;
    int k = keep_branch && keep_branch[0] ? ce_model_branch_named(keep_branch) : -1;
    if (k < 0 && g_m.nb) k = 0;
    reveal(k);
    rebuild_tree(k);
    show_rows(keep_entry);
    ulogf("confedit: loaded %d entries, %d branches in %llu ms\n", g_m.n, g_m.nb,
          (sys_monotonic_ns() - t0) / 1000000ull);
}

static void reload(void) {
    char b[SETTING_ABI_QUALIFIED_MAX] = "", e[SETTING_ABI_QUALIFIED_MAX] = "";
    if (sel_branch() >= 0) strlcpy(b, g_m.b[sel_branch()].name, sizeof b);
    if (sel()) strlcpy(e, sel()->name, sizeof e);
    load(b, e);
}

// --- editing -------------------------------------------------------------

static void open_edit(const char *note, const char *text) {
    const struct usetting_entry *e = sel();
    if (!e) return;
    strlcpy(g_edit_name, e->name, sizeof g_edit_name);
    snprintf(g_dlg_row[0], sizeof g_dlg_row[0], "%s", e->name);
    accepted(g_dlg_row[1], sizeof g_dlg_row[1]);
    static const struct uui_dialog_button BTNS[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "Save", DLG_SAVE, UUI_DLG_PRIMARY },
    };
    uui_dialog_open(&g_dlg, "Edit value", g_dlg_rows, g_dlg_row[1][0] ? 2 : 1, BTNS, 2, 1, DLG_CANCEL);
    if (note) {
        strlcpy(g_dlg_note, note, sizeof g_dlg_note);
        uui_dialog_set_note(&g_dlg, g_dlg_note);
    }
    uui_textbox_init(&g_field, text);
    uui_textbox_set_active(&g_field, 1);
    uui_textbox_select(&g_field, 0, (int)strlen(g_field.buf));
    int per = ugfx_char_advance('n') > 0 ? ugfx_char_advance('n') : 8, fh;
    uui_textbox_natural_size(&g_field, 0, &fh);
    uui_dialog_set_body(&g_dlg, &g_field_item, per * 40, fh);
    uui_dialog_focus(&g_dlg, &g_field_item);
}

static void edit(void) {
    if (why_not_edit()) return;
    open_edit(0, sel()->value);
}

// THE REGISTRY DECIDES, as for `config set`: a refusal keeps the dialog
// open with what would be accepted; "applied but not saved" is said as
// such, never as success.
static void answer_edit(int code) {
    char text[UUI_TEXTBOX_MAX];
    strlcpy(text, uui_textbox_text(&g_field), sizeof text);
    uui_dialog_set_body(&g_dlg, 0, 0, 0);
    if (code != DLG_SAVE) return;
    if (!sel() || strcmp(sel()->name, g_edit_name)) {
        snprintf(g_st_note, sizeof g_st_note, "The list changed -- nothing was saved");
        return;
    }
    static struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    strlcpy(m.name, g_edit_name, sizeof m.name);
    strlcpy(m.value, text, sizeof m.value);
    int rc = usetting_dispatch(&m);
    ulogf("confedit: set %s = \"%s\" -> %d result %u\n", g_edit_name, text, rc, (unsigned)m.result);
    if (rc != 0) {
        snprintf(g_st_note, sizeof g_st_note, "%s could not be changed", g_edit_name);
        return;
    }
    if (m.result == SETTING_INVALID) {
        char acc[160], note[200];
        accepted(acc, sizeof acc);
        if (g_sel.unavailable[0]) snprintf(note, sizeof note, "Not accepted: %s", g_sel.unavailable);
        else snprintf(note, sizeof note, "Not accepted. %s", acc[0] ? acc : "");
        open_edit(note, text);
        return;
    }
    if (m.result == SETTING_UNSAVED)
        snprintf(g_st_note, sizeof g_st_note, "Applied, but not saved -- gone at the next restart");
    else
        snprintf(g_st_note, sizeof g_st_note, "Saved %s", g_edit_name);
    ce_model_refresh(sel_entry());
    update_details();
    update_status();
}

static void reset(void) {
    if (why_not_reset()) return;
    static struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    strlcpy(m.name, sel()->name, sizeof m.name);
    int rc = usetting_dispatch(&m);
    ulogf("confedit: reset %s -> %d result %u\n", sel()->name, rc, (unsigned)m.result);
    if (rc == 0 && m.result == SETTING_SAVED) snprintf(g_st_note, sizeof g_st_note, "Reset %s to the machine's value", sel()->name);
    else snprintf(g_st_note, sizeof g_st_note, "%s could not be reset", sel()->name);
    ce_model_refresh(sel_entry());
    update_details();
    update_status();
}

static void copy(int value) {
    const struct usetting_entry *e = sel();
    if (!e) return;
    const char *s = value ? e->value : e->name;
    int ok = uclip_set_text(s, (int)strlen(s));
    snprintf(g_st_note, sizeof g_st_note, ok ? "Copied %s" : "The clipboard refused %s", s);
    update_details();
}

// --- layout and drawing --------------------------------------------------

static uint32_t pane_bg(void) { return ugfx_blend(UTHEME_CHROME, UTHEME_WHITE, 110); }

static void layout_all(int cw, int ch) {
    int mb = uui_menubar_height(&g_menu), tb = uui_toolbar_height(&g_tb), sb = uui_statusbar_height(&g_status);
    int pad = utheme_pad(), chh = ugfx_char_h();
    int per = ugfx_char_advance('n') > 0 ? ugfx_char_advance('n') : 8;
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_toolbar_ops.set_geometry(&g_tb, 0, mb, cw, tb);
    uui_toolbar_set_bounds(&g_tb, 0, 0, cw, ch);
    int fw = per * 30, fh;
    if (fw > cw / 3) fw = cw / 3;
    uui_textbox_natural_size(&g_find, 0, &fh);
    if (fh > tb - 4) fh = tb - 4;
    uui_textbox_set_geometry(&g_find, cw - pad - fw, mb + (tb - fh) / 2, fw, fh);

    int top = mb + tb, bottom = ch - sb;
    uui_statusbar_set_geometry(&g_status, 0, bottom, cw, sb);
    uui_splitter_set_track(&g_split, 0, cw, per * 16, per * 40);
    int left = uui_splitter_before(&g_split), bar = uui_splitter_thickness();
    uui_splitter_set_geometry(&g_split, uui_splitter_pos(&g_split), top, bar, bottom - top);
    uui_tree_ops.set_geometry(&g_tree, 0, top, left, bottom - top);

    int rx = left + bar, rw = cw - rx;
    int dh = chh * 9 + pad * 2;
    if (dh > (bottom - top) / 2) dh = (bottom - top) / 2;
    uui_table_ops.set_geometry(&g_table, rx, top, rw, bottom - top - dh);
    uui_props_set_geometry(&g_details, rx, bottom - dh, rw, dh);
    uui_dialog_set_bounds(&g_dlg, 0, 0, cw, ch);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    layout_all(d->surface->w, d->surface->h);
    uapp_log_layout(a, "confedit");
    const struct usetting_entry *e = sel();
    uapp_logf_layout("confedit: branch %s find \"%s\" rows %d selected %s from %s edit %d reset %d\n",
                     g_m.branch >= 0 ? g_m.b[g_m.branch].name : "-", g_m.find, g_m.nrows,
                     e ? e->name : "-", e ? ce_from_word(ce_model_info(sel_entry())->from) : "-",
                     !why_not_edit(), !why_not_reset());
    uapp_logf_layout("confedit: tree rows %d visible %d top %d\n", uui_tree_visible_count(&g_tree),
                     uui_tree_visible_rows(&g_tree), g_tree.top);
    uapp_logf_layout("confedit: table rows %d visible %d top %d\n", g_m.nrows,
                     uui_table_visible_rows(&g_table), g_table.top);
}

// --- commands and input --------------------------------------------------

static void focus_find(void) {
    for (int i = 0; i < g_focus.count; i++)
        if (g_focus.items[i].widget == &g_find) { uui_focus_set(&g_focus, i); return; }
}

static void set_all_open(int open) {
    for (int k = 0; k < g_m.nb; k++) g_m.b[k].open = open && g_m.b[k].kids;
    int keep = sel_branch();
    if (!open && keep >= 0) {   // up to its top-level branch, which stays visible
        while (g_m.b[keep].depth > 0) {
            char p[SETTING_ABI_QUALIFIED_MAX];
            strlcpy(p, g_m.b[keep].name, sizeof p);
            *strrchr(p, '.') = '\0';
            keep = ce_model_branch_named(p);
        }
    }
    rebuild_tree(keep);
    show_rows(0);
}

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_REFRESH:    reload(); snprintf(g_st_note, sizeof g_st_note, "Refreshed"); break;
    case CMD_EXIT:       uapp_quit(a, 0); return;
    case CMD_EDIT:       edit(); break;
    case CMD_RESET:      reset(); break;
    case CMD_COPY_NAME:  copy(0); break;
    case CMD_COPY_VALUE: copy(1); break;
    case CMD_FIND:       focus_find(); break;
    case CMD_EXPAND:     set_all_open(1); break;
    case CMD_COLLAPSE:   set_all_open(0); break;
    default: return;
    }
    uapp_redraw(a);
}

static void on_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    if (id < 0 || id >= g_m.nb) return;
    int keep = sel_branch();
    g_m.b[id].open = expand;
    // A collapse that hides the selection selects the branch collapsed,
    // as every desktop tree does, and the table follows it.
    if (!expand && keep >= 0 && keep != id) {
        size_t len = strlen(g_m.b[id].name);
        if (!strncmp(g_m.b[keep].name, g_m.b[id].name, len) && g_m.b[keep].name[len] == '.') keep = id;
    }
    rebuild_tree(keep);
    if (sel_branch() != g_m.branch && !g_m.find[0]) show_rows(0);
}

static void on_widget(struct uapp *a, int id, int reason) {
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    case ID_DIALOG: {
        int code = uui_dialog_take_code(&g_dlg);
        if (code < 0) return;
        answer_edit(code);
        uapp_redraw(a);
        return;
    }
    case ID_FIND:
        if (strcmp(uui_textbox_text(&g_find), g_m.find)) show_rows(sel() ? sel()->name : 0);
        uapp_redraw(a);
        return;
    case ID_TREE:
        if (sel_branch() != g_m.branch && !g_m.find[0]) show_rows(0);
        uapp_redraw(a);
        return;
    case ID_TABLE:
        if (sel_entry() != g_sel_entry) { update_details(); }
        uapp_redraw(a);
        return;
    }
    (void)reason;
    uapp_redraw(a);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (uui_dialog_is_open(&g_dlg)) return;
    if (uui_table_double_click(&g_table, x, y) >= 0) do_command(a, CMD_EDIT);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }
    if (uui_dialog_is_open(&g_dlg)) {
        uui_dialog_key(&g_dlg, key);
        int c = uui_dialog_take_code(&g_dlg);
        if (c >= 0) answer_edit(c);
        uapp_redraw(a);
        return;
    }
    (void)mods;
    switch (key) {
    case 0x06:   do_command(a, CMD_FIND); return;         // Ctrl+F
    case 0x03:                                             // Ctrl+C: a field copies its own text
        if (g_focus.current >= 0 && g_focus.items[g_focus.current].widget != &g_find)
            do_command(a, CMD_COPY_NAME);
        return;
    case KEY_F5: do_command(a, CMD_REFRESH); return;
    case '\n': case '\r':
        // Enter in the find field goes to what it found, as about:config's does.
        if (g_focus.current >= 0 && g_focus.items[g_focus.current].widget == &g_find) {
            for (int i = 0; i < g_focus.count; i++)
                if (g_focus.items[i].widget == &g_table) uui_focus_set(&g_focus, i);
            uapp_redraw(a);
            return;
        }
        do_command(a, CMD_EDIT);
        return;
    case 0x1B:                                             // Esc empties the find
        if (g_m.find[0]) {
            uui_textbox_set_text(&g_find, "");
            show_rows(sel() ? sel()->name : 0);
            uapp_redraw(a);
        }
        return;
    }
    // The ring moved a selection; the table and details follow it.
    if (sel_branch() != g_m.branch && !g_m.find[0]) show_rows(0);
    else if (sel_entry() != g_sel_entry) update_details();
    uapp_redraw(a);
}

// SOMEBODY CHANGED A SETTING -- this window, System Settings, `config`:
// the whole tree is read again, the selection kept by name.
static void on_setting(struct uapp *a) {
    reload();
    uapp_redraw(a);
}

static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    load(0, 0);
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

static void on_size(int *w, int *h) {
    int per = ugfx_char_advance('n');
    if (per <= 0) per = 8;
    *w = per * 104;
    *h = ugfx_char_h() * 36;
}

int main(void) {
    // Never freed: the menu rows live in the builder.
    struct uui_builder *menus = uui_builder_load("confedit", MENU_NAMES,
                                                (int)(sizeof MENU_NAMES / sizeof MENU_NAMES[0]));
    int bar_count;
    const struct uui_menu_item *bar_rows = uui_builder_menu(menus, "bar", &bar_count);
    if (uui_builder_error(menus)) return uui_builder_run_error(menus);
    uui_menubar_init(&g_menu, bar_rows, bar_count);
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_tb, TB_ITEMS, (int)(sizeof TB_ITEMS / sizeof TB_ITEMS[0]));
    g_tb.item_flags = item_flags;
    g_tb.why = why;
    g_tb.overflow = 1;
    uui_textbox_init(&g_find, "");
    g_find.placeholder = "Find a name or value";
    uui_tree_init(&g_tree, 0, 0, 0, 0, 0, 0);
    uui_tree_set_on_toggle(&g_tree, on_toggle, 0);
    g_tree.sel_style = UUI_SEL_ROUNDED;
    g_tree.bg = pane_bg();
    uui_splitter_init(&g_split, 1, 280);
    uui_table_init(&g_table, 0, 0, 0, 0, COLS, (int)(sizeof COLS / sizeof COLS[0]), cell, 0);
    uui_table_set_fade(&g_table, fade);
    uui_table_set_dot(&g_table, dot);
    uui_props_init(&g_details);
    uui_dialog_init(&g_dlg);
    uui_statusbar_init(&g_status);
    g_status.panes[0] = (struct uui_status_pane){ .text = g_st_where, .chars = 30 };
    g_status.panes[1] = (struct uui_status_pane){ .text = g_st_count, .chars = 34 };
    g_status.panes[2] = (struct uui_status_pane){ .text = g_st_note, .chars = 0 };
    g_status.count = 3;
    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

    struct uapp_desc desc = {
        .title        = "Configuration Editor",
        .app_id       = "confedit",
        .flags        = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .on_size      = on_size,
        .min_w        = 640,
        .min_h        = 400,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .focus        = &g_focus,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_press     = on_press,
        .on_key       = on_key,
        .on_resize    = on_resize,
        .on_setting   = on_setting,
    };
    return uapp_run(&desc);
}
