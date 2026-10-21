// See ui/uui_filedialog.h for the design writeup (what this is, why it
// is a window, and why Places rather than a tree).
#include "lib/unetmount.h"
#include "ui/uui_filedialog.h"

#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "kpath.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_route.h"   // UUI_REASON_*
#include "lib/urecent.h"   // a file chosen is Recent's
#include "lib/ufileop.h"
#include "ui/ulog.h"
#include "ui/uui_verbs.h"
#include <errno.h>
#include <stdlib.h>

// Widget ids, which are also the toolbar/button codes. One space: the
// window has one on_widget and one switch.
enum {
    ID_BAR = 1, ID_PLACES, ID_VIEW, ID_NAME, ID_TYPE, ID_OK, ID_CANCEL, ID_PATH,
    ID_CTX, ID_ASK, ID_UNDO,
    CMD_UP = 20, CMD_REFRESH, CMD_DETAILS, CMD_ICONS, CMD_BACK, CMD_FORWARD,
    CMD_NEW_FOLDER, CMD_RENAME, CMD_DELETE, CMD_OPEN,
    ASK_DELETE, ASK_CANCEL,
};

// Where each widget sits in the focus ring. NAMED, because a focus ring
// is addressed by index and a bare 2 in a press handler is the "named by
// its position in the array" bug this tree keeps finding.
enum { FOCUS_BAR, FOCUS_PATH, FOCUS_PLACES, FOCUS_VIEW, FOCUS_NAME, FOCUS_TYPE, FOCUS_COUNT };

static const struct uui_toolbar_item g_tools[] = {
    { "tb-back",    "Back (Alt+Left)",     CMD_BACK, 0, 0, 0, 0 },
    { "tb-forward", "Forward (Alt+Right)", CMD_FORWARD, 0, 0, 0, 0 },
    { "tb-up",      "Up",      CMD_UP, 0, 0, 0, 0 },
    { "tb-refresh", "Refresh", CMD_REFRESH, 0, 0, 0, 0 },
    UUI_TOOLBAR_SEP,
    { UUI_VI_NEW_FOLDER, "New folder (Ctrl+Shift+N)", CMD_NEW_FOLDER, "New folder", 0, 0,
      UUI_VT_NEW_FOLDER },
    UUI_TOOLBAR_SEP,
    { "tb-details", "Details", CMD_DETAILS, 0, 0, 0, 0 },
    { "tb-icons",   "Icons",   CMD_ICONS, 0, 0, 0, 0 },
};

// THE ONE INSTANCE THE TOOLBAR'S FLAGS CALLBACK CAN SEE. `item_flags`
// takes a code and nothing else (ui/uui_toolbar.h), so the latched view
// button has to be reachable from a file-scope pointer. One chooser at a
// time per process is the rule anyway -- it is modal.
static struct uui_filedialog *g_open;

static unsigned tool_flags(int code) {
    if (!g_open) return 0;
    if (code == CMD_DETAILS) return g_open->view.mode == UUI_FILEVIEW_DETAILS ? UUI_MI_CHECKED : 0;
    if (code == CMD_ICONS)   return g_open->view.mode == UUI_FILEVIEW_ICONS   ? UUI_MI_CHECKED : 0;
    if (code == CMD_BACK || code == CMD_FORWARD)
        return unavhist_can(&g_open->hist, code == CMD_BACK ? -1 : 1) ? 0 : UUI_MI_DISABLED;
    return 0;
}

// --- places ------------------------------------------------------------
//
// Only what is REALLY THERE. A destination that does not resolve is left
// out rather than listed and refused: a chooser whose sidebar offers a
// folder that cannot be opened teaches the user to distrust the rest.
static void add_place(struct uui_filedialog *fd, const char *label, const char *icon,
                      const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0 || !st.is_dir) return;
    uui_places_add(&fd->places, label, icon, path);
}

static void build_places(struct uui_filedialog *fd) {
    uui_places_init(&fd->places);
    // The File Manager's list, in its order -- one idea of where
    // Documents is -- then Scratch, which a chooser saves into and a
    // file manager does not need, then every mounted device.
    uui_places_add_pins(&fd->places, 0);   // the File Manager's Places, in its order
    add_place(fd, "Scratch", "folder", "/var/tmp");
    uui_places_refresh(&fd->places);
}

// The sidebar and the breadcrumb follow the LISTING, so browsing away
// from a place clears its highlight rather than leaving it claiming to
// be where you are.
static void sync_places(struct uui_filedialog *fd) {
    const char *dir = uui_fileview_dir(&fd->view);
    uui_places_select_path(&fd->places, dir);
    if (!uui_pathbar_is_editing(&fd->path)) uui_pathbar_set_path(&fd->path, dir);
}

// **A DIRECTORY ALWAYS PASSES, WHATEVER THE ROW SAYS.** The contract is
// in uui_filedialog.h and it has to be ENFORCED here rather than asked
// of every caller: each of the three apps' filters answers "is this a
// file I can open", and every one of them says no to a directory --
// which in a SIDEBAR pinned to one folder is right, and in a chooser
// leaves a root listing with nothing in it and no way to go anywhere.
// Found by looking at the screen; 22 green checks had not noticed,
// because each of them reached its directory by a Places row or a typed
// path rather than by walking one.
static const struct uui_filedialog_filter *chosen_filter(const struct uui_filedialog *fd);

static int filter_gate(void *ctx, const char *dir, const struct sys_dirent *e) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    if (e->is_dir) return 1;
    const struct uui_filedialog_filter *f = chosen_filter(fd);
    return (f && f->fn) ? f->fn(f->ctx, dir, e) : 1;
}

// The row the combo has selected, or NULL when there are none.
static const struct uui_filedialog_filter *chosen_filter(const struct uui_filedialog *fd) {
    int i = fd->filter_count ? uui_dropdown_selected(&fd->type) : -1;
    if (i < 0 || i >= fd->filter_count) return 0;
    return &fd->filters[i];
}

// The combo changed: the view's filter is always the gate, so only the
// listing has to be redone.
static void apply_filter(struct uui_filedialog *fd) {
    uui_fileview_reload(&fd->view);
}

// SAVE AS TYPE: the name's extension becomes the chosen row's -- the
// last one swapped ("holiday.png" -> "holiday.jpg"), or added to a name
// with none. A row with no `ext` leaves the name alone.
static void retype_name(struct uui_filedialog *fd) {
    const struct uui_filedialog_filter *f = chosen_filter(fd);
    if (!f || !f->ext) return;
    char name[UUI_FILEDIALOG_PATH_MAX];
    snprintf(name, sizeof name, "%s", uui_textbox_text(&fd->name));
    if (!name[0]) return;
    char *slash = strrchr(name, '/');
    char *dot = strrchr(slash ? slash + 1 : name, '.');
    if (dot && dot != (slash ? slash + 1 : name)) *dot = '\0';
    strlcat(name, f->ext, sizeof name);
    uui_textbox_set_text(&fd->name, name);
}

// --- committing --------------------------------------------------------

static void finish(struct uui_filedialog *fd, const char *path) {
    void (*done)(void *, const char *) = fd->on_done;
    void *ctx = fd->ctx;
    char copy[UUI_FILEDIALOG_PATH_MAX];
    copy[0] = '\0';
    if (path) strlcpy(copy, path, sizeof copy);
    // CLOSED FIRST, so the callback may open another chooser -- and so
    // the owner is unblocked before it is asked to do anything.
    uui_filedialog_close_window(fd);
    // A file opened or saved is Recent's, as Windows' common dialog does.
    if (path && fd->mode != UUI_FILEDIALOG_FOLDER) urecent_add(copy, fd->app);
    if (done) done(ctx, path ? copy : 0);
}

// What the OK button means, which is different in all three modes.
// Returns 1 if the dialog is finished; 0 keeps it up (a folder was
// entered instead, or the answer is not usable yet).
static int commit(struct uui_filedialog *fd) {
    const char *dir = uui_fileview_dir(&fd->view);

    if (fd->mode == UUI_FILEDIALOG_FOLDER) {
        // A directory ROW is the answer if one is selected; otherwise
        // the directory being shown is, which is GTK's SELECT_FOLDER
        // behaviour and what makes "choose this folder" reachable
        // without selecting anything.
        char sel[UUI_FILEDIALOG_PATH_MAX];
        if (uui_fileview_selected_is_dir(&fd->view) &&
            uui_fileview_selected_path(&fd->view, sel, sizeof sel)) {
            finish(fd, sel);
            return 1;
        }
        finish(fd, dir);
        return 1;
    }

    const char *typed = uui_textbox_text(&fd->name);
    char full[UUI_FILEDIALOG_PATH_MAX];

    if (typed && typed[0]) {
        // AN ABSOLUTE PATH TYPED INTO THE FIELD IS TAKEN AS ONE, which
        // every real chooser allows and is the only way to reach a
        // directory the Places list does not name.
        // A share address (smb://nas/media) opens the share, mounted for
        // the session if nothing has it yet (lib/unetmount.h).
        if (unetmount_is_address(typed)) {
            if (unetmount_open_address(typed, full, sizeof full)) return 0;
            build_places(fd);
        } else if (typed[0] == '/') strlcpy(full, typed, sizeof full);
        else if (!k_path_join(dir, typed, full, sizeof full)) return 0;
    } else if (!uui_fileview_selected_path(&fd->view, full, sizeof full)) {
        return 0;   // nothing named and nothing selected
    }

    struct sys_stat st;
    int exists = sys_stat(full, &st) == 0;
    if (exists && st.is_dir) {
        // OK ON A FOLDER ENTERS IT and keeps the dialog up, as in
        // Explorer and every GTK chooser.
        uui_fileview_set_dir(&fd->view, full);
        uui_textbox_set_text(&fd->name, "");
        sync_places(fd);
        return 0;
    }
    if (fd->mode == UUI_FILEDIALOG_OPEN && !exists) return 0;
    finish(fd, full);
    return 1;
}

// --- managing files ------------------------------------------------------
//
// Done here and at once, with no job or progress: a chooser's listing
// is one folder, and only a permanent delete of a tree can take long.

// What Undo would put back, and the paths a question is about: both
// sized by the selection, so they are allocated for it rather than kept.
static void drop_undo(struct uui_filedialog *fd) {
    free(fd->undone);
    fd->undone = 0;
    fd->undone_n = 0;
}

static void set_note(struct uui_filedialog *fd, const char *text, int can_undo) {
    if (text != fd->note_text) snprintf(fd->note_text, sizeof fd->note_text, "%s", text ? text : "");
    if (!can_undo) drop_undo(fd);
    fd->can_undo = can_undo;
    fd->root_items[2].hidden = !fd->note_text[0];
    fd->note_items[1].hidden = !can_undo;
}

// The listing again, with `name` selected (and, for a file, in the field
// -- view_select's rule).
static void relist(struct uui_filedialog *fd, const char *name) {
    uui_fileview_reload(&fd->view);
    if (name && name[0]) uui_fileview_select_name(&fd->view, name);
}

// After a delete: the row that took the deleted one's place, Explorer's
// rule, rather than wherever the reload left the selection.
static void relist_at(struct uui_filedialog *fd, int view_row) {
    uui_fileview_reload(&fd->view);
    struct uui_table *t = &fd->view.table;
    int n = uui_fileview_row_count(&fd->view);
    if (view_row >= n) view_row = n - 1;
    if (view_row < 0) return;
    t->selected = uui_table_source_row(t, view_row);
    char name[UUI_FILEDIALOG_PATH_MAX];
    const char *sel = uui_fileview_selected_name(&fd->view);
    if (sel) {
        snprintf(name, sizeof name, "%s", sel);
        uui_fileview_select_name(&fd->view, name);   // reports it, so a file lands in the field
    }
}

static int selected_view_row(const struct uui_filedialog *fd) {
    const struct uui_table *t = &fd->view.table;
    return t->selected >= 0 ? uui_table_view_row(t, t->selected) : -1;
}

static void new_folder(struct uui_filedialog *fd) {
    const char *dir = uui_fileview_dir(&fd->view);
    char name[UUI_FILEDIALOG_PATH_MAX], path[UUI_FILEDIALOG_PATH_MAX];
    struct sys_stat st;
    strlcpy(name, "New folder", sizeof name);
    if (!k_path_join(dir, name, path, sizeof path) ||
        (sys_stat(path, &st) == 0 && (!ufileop_unique_name(dir, "New folder", name, sizeof name) ||
                                      !k_path_join(dir, name, path, sizeof path)))) {
        set_note(fd, "There is no free name for a new folder here.", 0);
        return;
    }
    if (sys_mkdir(path) < 0) {
        set_note(fd, "A folder can't be made here.", 0);
        return;
    }
    set_note(fd, "", 0);
    relist(fd, name);
    // NAMED WHERE IT STANDS, Explorer's New folder: the edit is the
    // listing's, so the keys go there.
    if (uui_fileview_begin_rename(&fd->view)) {
        fd->was_renaming = 1;
        uui_focus_set(&fd->focus, FOCUS_VIEW);
        uui_textbox_set_active(&fd->name, 0);
    }
}

// A rename the listing finished (Enter, or a click elsewhere): do it.
static void rename_poll(struct uui_filedialog *fd) {
    char from[UUI_FILEDIALOG_PATH_MAX], to[UUI_FILEDIALOG_PATH_MAX];
    if (!uui_fileview_take_rename(&fd->view, from, to, sizeof from)) return;
    const char *dir = uui_fileview_dir(&fd->view);
    char src[UUI_FILEDIALOG_PATH_MAX], dst[UUI_FILEDIALOG_PATH_MAX];
    struct sys_stat st;
    if (strchr(to, '/') || !strcmp(to, ".") || !strcmp(to, "..")) {
        set_note(fd, "A name can't contain / or be . or ..", 0);
        return;
    }
    if (!k_path_join(dir, from, src, sizeof src) || !k_path_join(dir, to, dst, sizeof dst)) {
        set_note(fd, "That name is too long.", 0);
        return;
    }
    // A RENAME NEVER REPLACES: rename(2) would, silently, and Explorer
    // refuses instead.
    if (sys_stat(dst, &st) == 0) {
        snprintf(fd->note_text, sizeof fd->note_text, "There is already something called %s here.", to);
        set_note(fd, fd->note_text, 0);
        return;
    }
    if (sys_rename(src, dst) < 0) {
        set_note(fd, "That can't be renamed here.", 0);
        return;
    }
    set_note(fd, "", 0);
    relist(fd, to);
}

// The permanent delete, of a file or a whole tree. The walk's state is
// ~100 KB, so it is borrowed for the call rather than kept in every app
// that can open a chooser.
static int delete_for_good(const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) return 0;
    if (!st.is_dir) return sys_unlink(path) == 0;
    struct ufileop *op = malloc(sizeof *op);
    if (!op) return 0;
    memset(op, 0, sizeof *op);
    struct ufileop_policy pol = { 0 };
    int ok = ufileop_remove(path, 1, op, &pol) == UFILEOP_OK;
    free(op);
    return ok;
}

// The operands: the marked set, else the selected row -- the File
// Manager's rule. Into fd->ops; returns how many.
static int gather(struct uui_filedialog *fd) {
    free(fd->ops);
    fd->ops = 0;
    fd->op_n = 0;
    int marks = uui_fileview_mark_count(&fd->view);
    int n = marks > 0 ? marks : uui_fileview_selected_name(&fd->view) ? 1 : 0;
    if (!n || !(fd->ops = malloc((size_t)n * sizeof fd->ops[0]))) return 0;
    for (int i = 0; i < n; i++) {
        int ok = marks > 0 ? uui_fileview_marked_path(&fd->view, i, fd->ops[fd->op_n], UUI_FILEDIALOG_PATH_MAX)
                           : uui_fileview_selected_path(&fd->view, fd->ops[fd->op_n], UUI_FILEDIALOG_PATH_MAX);
        if (ok) fd->op_n++;
    }
    return fd->op_n;
}

// "notes.txt" for one operand, "3 items" for several.
static const char *what(const struct uui_filedialog *fd, char *buf, int cap) {
    if (fd->op_n == 1) return k_path_basename(fd->ops[0]);
    snprintf(buf, (size_t)cap, "%d items", fd->op_n);
    return buf;
}

static void ask_for_good(struct uui_filedialog *fd, int no_bin) {
    static const struct uui_dialog_button btns[] = {
        { "Cancel", ASK_CANCEL, 0 }, { "Delete", ASK_DELETE, UUI_DLG_DANGER },
    };
    char n[32];
    snprintf(fd->ask_line, sizeof fd->ask_line, "%s will be deleted permanently.",
             what(fd, n, sizeof n));
    fd->ask_rows[0] = fd->ask_line;
    fd->ask_rows[1] = 0;
    uui_dialog_set_bounds(&fd->ask, 0, 0, fd->root.w, fd->root.h);
    uui_dialog_open(&fd->ask, "Delete for good?", fd->ask_rows, 1, btns, 2, 0, ASK_CANCEL);
    uui_dialog_set_note(&fd->ask, no_bin ? "This place has no Recycle Bin." : 0);
}

// The name field held one of the deleted: it names nothing now.
static void clear_name_if_gone(struct uui_filedialog *fd) {
    if (fd->mode == UUI_FILEDIALOG_FOLDER) return;
    for (int i = 0; i < fd->op_n; i++)
        if (!strcmp(uui_textbox_text(&fd->name), k_path_basename(fd->ops[i])))
            uui_textbox_set_text(&fd->name, "");
}

static void delete_selected(struct uui_filedialog *fd, int for_good) {
    if (!gather(fd)) return;
    char bin[UUI_FILEDIALOG_PATH_MAX];
    // ONE BIN PER VOLUME, and a listing is one folder: the first item's
    // answer is every item's.
    if (for_good || !utrash_bin_for(fd->ops[0], bin, sizeof bin)) {
        ask_for_good(fd, !for_good);
        return;
    }
    int at = selected_view_row(fd);
    set_note(fd, "", 0);
    fd->undone = malloc((size_t)fd->op_n * sizeof fd->undone[0]);
    int moved = 0;
    for (int i = 0; i < fd->op_n; i++) {
        struct utrash_item it;
        if (utrash_put(fd->ops[i], &it) != 0) continue;
        if (fd->undone) fd->undone[fd->undone_n++] = it;
        moved++;
    }
    char n[32];
    if (moved == fd->op_n)
        snprintf(fd->note_text, sizeof fd->note_text, "Moved %s to the Recycle Bin.", what(fd, n, sizeof n));
    else
        snprintf(fd->note_text, sizeof fd->note_text, "Moved %d of %d to the Recycle Bin; the rest can't be deleted.",
                 moved, fd->op_n);
    set_note(fd, fd->note_text, fd->undone_n > 0);
    clear_name_if_gone(fd);
    relist_at(fd, at);
}

static void ask_answer(struct uui_filedialog *fd, int code) {
    if (code != ASK_DELETE) return;
    int at = selected_view_row(fd);
    int gone = 0;
    for (int i = 0; i < fd->op_n; i++) gone += delete_for_good(fd->ops[i]);
    char n[32];
    if (gone == fd->op_n)
        snprintf(fd->note_text, sizeof fd->note_text, "Deleted %s.", what(fd, n, sizeof n));
    else
        snprintf(fd->note_text, sizeof fd->note_text, "Deleted %d of %d; the rest can't be deleted.",
                 gone, fd->op_n);
    set_note(fd, fd->note_text, 0);
    clear_name_if_gone(fd);
    relist_at(fd, at);
}

static void undo_delete(struct uui_filedialog *fd) {
    if (!fd->can_undo || !fd->undone_n) return;
    int back = 0;
    char first[UUI_FILEDIALOG_PATH_MAX] = "";
    for (int i = 0; i < fd->undone_n; i++)
        if (utrash_restore(&fd->undone[i]) == 0 && !back++)
            snprintf(first, sizeof first, "%s", k_path_basename(fd->undone[i].orig));
    set_note(fd, back == fd->undone_n ? "" : "Some can't be put back -- see the Recycle Bin.", 0);
    // Back into the folder shown: leaving it clears the strip, Undo too.
    relist(fd, first);
    uui_focus_set(&fd->focus, FOCUS_VIEW);
}

// What a row may do right now: Rename and Delete need a real row.
static unsigned ctx_flags(int code) {
    if (!g_open) return UUI_MI_DISABLED;
    if (code == CMD_RENAME)
        return uui_fileview_selected_name(&g_open->view) ? 0 : UUI_MI_DISABLED;
    if (code == CMD_DELETE)
        return uui_fileview_selected_name(&g_open->view) || uui_fileview_mark_count(&g_open->view)
               ? 0 : UUI_MI_DISABLED;
    return 0;
}

static void ctx_add(struct uui_filedialog *fd, struct uui_menu_item it) {
    if (fd->menu_count < (int)(sizeof fd->menu_rows / sizeof fd->menu_rows[0]))
        fd->menu_rows[fd->menu_count++] = it;
}

// ON A ROW: what OK would do with it, then the row's own verbs. On the
// empty space: the folder's. Windows' common dialog has both menus.
static void ctx_build(struct uui_filedialog *fd, int on_row) {
    fd->menu_count = 0;
    if (on_row) {
        int dir = uui_fileview_selected_is_dir(&fd->view);
        if (dir) ctx_add(fd, (struct uui_menu_item)UUI_MENU_VERB("Open", CMD_OPEN, "Enter", OPEN));
        else if (fd->mode != UUI_FILEDIALOG_FOLDER)
            ctx_add(fd, (struct uui_menu_item)UUI_MENU_VERB(fd->ok.label, CMD_OPEN, "Enter", OPEN));
        if (fd->menu_count) ctx_add(fd, (struct uui_menu_item)UUI_MENU_SEP);
        ctx_add(fd, (struct uui_menu_item)UUI_MENU_VERB("Rename", CMD_RENAME, "F2", RENAME));
        ctx_add(fd, (struct uui_menu_item)UUI_MENU_VERB("Delete", CMD_DELETE, "Del", DELETE));
        ctx_add(fd, (struct uui_menu_item)UUI_MENU_SEP);
    }
    ctx_add(fd, (struct uui_menu_item)UUI_MENU_VERB("New folder", CMD_NEW_FOLDER, "Ctrl+Shift+N",
                                                    NEW_FOLDER));
    if (!on_row) ctx_add(fd, (struct uui_menu_item)UUI_MENU_VERB("Refresh", CMD_REFRESH, 0, REFRESH));
}

static int still_a_dir(void *ctx, const char *dir) {
    (void)ctx;
    struct sys_stat st;
    return sys_stat(dir, &st) == 0 && st.is_dir;
}

// Back (-1) or Forward (+1), stepping over a folder that has gone away.
static void go(struct uui_filedialog *fd, int delta) {
    const char *skipped;
    const char *to = unavhist_step(&fd->hist, delta, still_a_dir, 0, &skipped);
    if (!to) return;
    char dir[UUI_FILEDIALOG_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", to);
    fd->replaying = 1;
    uui_fileview_set_dir(&fd->view, dir);
    fd->replaying = 0;
    if (fd->mode != UUI_FILEDIALOG_SAVE) uui_textbox_set_text(&fd->name, "");
    if (skipped) {
        snprintf(fd->note_text, sizeof fd->note_text, "Skipped %s -- it no longer exists.",
                 k_path_basename(skipped));
        set_note(fd, fd->note_text, 0);
    }
}

static void run_command(struct uui_filedialog *fd, int code) {
    switch (code) {
    case CMD_BACK:    go(fd, -1); break;
    case CMD_FORWARD: go(fd, 1); break;
    case CMD_UP:      uui_fileview_up(&fd->view); sync_places(fd); break;
    case CMD_REFRESH: uui_fileview_reload(&fd->view); break;
    case CMD_DETAILS: uui_fileview_set_mode(&fd->view, UUI_FILEVIEW_DETAILS); break;
    case CMD_ICONS:   uui_fileview_set_mode(&fd->view, UUI_FILEVIEW_ICONS); break;
    case CMD_NEW_FOLDER: new_folder(fd); break;
    case CMD_RENAME:
        if (uui_fileview_begin_rename(&fd->view)) {
            fd->was_renaming = 1;
            uui_focus_set(&fd->focus, FOCUS_VIEW);
            uui_textbox_set_active(&fd->name, 0);
        }
        break;
    case CMD_DELETE:  delete_selected(fd, 0); break;
    case CMD_OPEN:    uui_fileview_activate(&fd->view); break;
    default: break;
    }
}

// --- the window's callbacks --------------------------------------------

static void on_widget(struct uapp_window *w, int id, int reason) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;

    switch (id) {
    case ID_BAR:
        run_command(fd, uui_toolbar_take_code(&fd->bar));
        break;
    case ID_CTX: {
        int code = uui_menubar_take_code(&fd->menu);
        if (!uui_menubar_is_open(&fd->menu) && fd->focus.current < 0)
            uui_focus_set(&fd->focus, FOCUS_VIEW);
        if (code > 0) run_command(fd, code);
        break;
    }
    case ID_ASK: {
        int code = uui_dialog_take_code(&fd->ask);
        if (code > 0) {
            uui_dialog_close(&fd->ask);
            ask_answer(fd, code);
        }
        break;
    }
    case ID_PLACES: {
        // TAKEN, never read off the selection: a hover reaches here too
        // (motion goes to every widget), and only a click or Enter parks
        // a path -- so a pointer resting over the list cannot re-list
        // the directory and wipe the typed name.
        char to[UUI_FILEDIALOG_PATH_MAX];
        if (uui_places_take(&fd->places, to, sizeof to)) {
            // A pinned folder deleted since the list was built: the
            // listing stays where it was and the list loses the row.
            char was[UUI_FILEDIALOG_PATH_MAX];
            snprintf(was, sizeof was, "%s", uui_fileview_dir(&fd->view));
            if (!uui_fileview_set_dir(&fd->view, to)) {
                uui_fileview_set_dir(&fd->view, was);
                build_places(fd);
            }
            uui_textbox_set_text(&fd->name, "");
            sync_places(fd);
            uui_focus_set(&fd->focus, FOCUS_PLACES);
        }
        break;
    }
    case ID_PATH: {
        // A segment, or a typed path (relative to the listing). One that
        // is not a folder leaves the field up, holding what was typed.
        char to[UUI_FILEDIALOG_PATH_MAX], full[UUI_FILEDIALOG_PATH_MAX];
        if (reason == UUI_REASON_KEY) fd->swallow_key = 1;
        if (reason == UUI_REASON_PRESS && uui_pathbar_is_editing(&fd->path))
            uui_focus_set(&fd->focus, FOCUS_PATH);
        if (!uui_pathbar_take(&fd->path, to, sizeof to)) break;
        static char scratch[KPATH_SCRATCH_FOR(UUI_FILEDIALOG_PATH_MAX)];
        struct kpath_scratch sc = { scratch, sizeof scratch };
        struct sys_stat st;
        if (unetmount_is_address(to)) {
            if (unetmount_open_address(to, full, sizeof full)) break;
            build_places(fd);
            snprintf(to, sizeof to, "%s", full);
        }
        if (k_path_resolve(uui_fileview_dir(&fd->view), to, full, sizeof full, &sc) &&
            sys_stat(full, &st) == 0 && st.is_dir) {
            uui_fileview_set_dir(&fd->view, full);
            uui_pathbar_set_path(&fd->path, uui_fileview_dir(&fd->view));
            sync_places(fd);
            uui_focus_set(&fd->focus, FOCUS_VIEW);
        }
        break;
    }
    case ID_VIEW:
        // A PRESS MOVES THE FOCUS TO WHAT WAS PRESSED. The router names
        // the child, not the container, so nothing else does this --
        // and typing after clicking the list would otherwise go to the
        // name field (ui/uui_dialog.h's note, reached from the other
        // side).
        if (reason == UUI_REASON_PRESS) {
            uui_focus_set(&fd->focus, FOCUS_VIEW);
            uui_textbox_set_active(&fd->name, 0);
        }
        break;
    case ID_NAME:
        if (reason == UUI_REASON_PRESS) uui_focus_set(&fd->focus, FOCUS_NAME);
        break;
    case ID_TYPE:
        // THE LISTING FOLLOWS THE COMBO. Re-listed on every report the
        // widget makes rather than only on a release: a dropdown commits
        // from the keyboard too, and a chooser showing the wrong set
        // until the next click is worse than one that re-lists twice.
        if (reason == UUI_REASON_PRESS) uui_focus_set(&fd->focus, FOCUS_TYPE);
        apply_filter(fd);
        if (fd->mode == UUI_FILEDIALOG_SAVE) retype_name(fd);
        break;
    default:
        break;
    }
    rename_poll(fd);
    // NOT FOR A KEY: the toolkit reports a key the listing took BEFORE
    // on_key hears it, and on_key must still see that the field had it.
    if (reason != UUI_REASON_KEY) fd->was_renaming = uui_fileview_renaming(&fd->view);
    uapp_window_redraw(w);
}

// OK and Cancel.
static void on_action(struct uapp_window *w, int code) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;
    if (code == ID_OK && commit(fd)) return;
    if (code == ID_CANCEL) { finish(fd, 0); return; }
    if (code == ID_UNDO) undo_delete(fd);
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;
    // THE OPEN MENU HAS THE KEYS -- arrows, Enter, Esc -- as in every
    // app here (uui_menubar_key()). The focus was parked when it opened,
    // so the listing behind it did not move or open anything first.
    if (uui_menubar_is_open(&fd->menu)) {
        int code = 0;
        if (key == '\t') uui_menubar_close(&fd->menu);   // Tab moved the focus: let it go
        else uui_menubar_key(&fd->menu, key, &code);
        if (!uui_menubar_is_open(&fd->menu)) {
            uui_focus_set(&fd->focus, FOCUS_VIEW);
            if (code > 0) run_command(fd, code);
        }
        uapp_window_redraw(w);
        return;
    }
    // THE NAME BEING EDITED IN THE LISTING had this key: its Enter kept
    // the name and its Esc dropped it, and neither answers the dialog.
    // So did an open menu or question, whose keys the router gave them.
    int was = fd->was_renaming;
    rename_poll(fd);
    fd->was_renaming = uui_fileview_renaming(&fd->view);
    if (was || uui_dialog_is_open(&fd->ask)) {
        uapp_window_redraw(w);
        return;
    }
    if ((mods & KEY_MOD_ALT) && (key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT)) {
        go(fd, key == KEY_ARROW_LEFT ? -1 : 1);
        uapp_window_redraw(w);
        return;
    }
    if (key == 0x0E && (mods & KEY_MOD_CTRL) && (mods & KEY_MOD_SHIFT)) {   // Ctrl+Shift+N
        new_folder(fd);
        uapp_window_redraw(w);
        return;
    }
    // F2 and Del are the LISTING's: in the name field, Del deletes text.
    if (fd->focus.current == FOCUS_VIEW && (key == KEY_F2 || key == KEY_DELETE)) {
        if (key == KEY_F2) run_command(fd, CMD_RENAME);
        else delete_selected(fd, (mods & KEY_MOD_SHIFT) != 0);
        uapp_window_redraw(w);
        return;
    }
    // ESCAPE CANCELS AND RETURN COMMITS, in a dialog, on every system
    // there is. Both are read here rather than by a widget because
    // neither belongs to one: they are the dialog's own two answers.
    // ...unless the breadcrumb was being typed in: its Enter went there
    // and its Esc closed the field, and neither is the dialog's answer.
    if (fd->swallow_key) { fd->swallow_key = 0; uapp_window_redraw(w); return; }
    if (key == 0x1B) { finish(fd, 0); return; }
    if (key == '\n' || key == '\r') { if (commit(fd)) return; }
    uapp_window_redraw(w);
}

// THE PRESS MOVES THE SELECTION FIRST, Explorer's rule: a menu acting
// on a row other than the one pointed at deletes the wrong file. The
// release opens the menu.
static void on_button(struct uapp_window *w, int x, int y, unsigned buttons, int up) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;
    // THE THUMB BUTTONS ARE BACK AND FORWARD, the one nearer the thumb's
    // rest going back -- Explorer's, Dolphin's, every browser's. On the
    // press; the release that follows is nobody's.
    if (!up && (buttons & (WIN_MOUSE_BTN_SIDE | WIN_MOUSE_BTN_EXTRA))) {
        fd->menu_armed = 0;
        if (!uui_dialog_is_open(&fd->ask) && !uui_menubar_is_open(&fd->menu) &&
            !uui_fileview_renaming(&fd->view))
            go(fd, (buttons & WIN_MOUSE_BTN_SIDE) ? -1 : 1);
        uapp_window_redraw(w);
        return;
    }
    if (!up && !(buttons & 0x2)) return;
    if (!up) {
        fd->menu_armed = !uui_dialog_is_open(&fd->ask) && uui_fileview_ops.hit(&fd->view, x, y);
        if (fd->menu_armed) {
            uui_fileview_finish_rename(&fd->view);
            rename_poll(fd);
            fd->was_renaming = 0;
            // Below the rows is the FOLDER's menu, so nothing stays selected.
            if (uui_fileview_row_at(&fd->view, x, y) < 0) uui_fileview_select_none(&fd->view);
            else uui_fileview_select_at(&fd->view, x, y);
            uui_focus_set(&fd->focus, FOCUS_VIEW);
            uui_textbox_set_active(&fd->name, 0);
        }
        uapp_window_redraw(w);
        return;
    }
    if (!fd->menu_armed) return;
    fd->menu_armed = 0;
    // On a row is "the press selected one under the pointer": the
    // selection after select_at() is the row there, or none.
    int on_row = uui_fileview_selected_name(&fd->view) != 0;
    ctx_build(fd, on_row);
    char rows[200] = "";
    for (int i = 0; i < fd->menu_count; i++) {
        size_t l = strlen(rows);
        snprintf(rows + l, sizeof rows - l, "%s%s", i ? "|" : "",
                 fd->menu_rows[i].label ? fd->menu_rows[i].label : "-");
    }
    ulogf("filedialog: ctxmenu rows %s\n", rows);
    uui_menubar_set_bounds(&fd->menu, 0, 0, fd->root.w, fd->root.h);
    uui_menubar_open_at(&fd->menu, fd->menu_rows, fd->menu_count, x, y);
    uui_focus_set(&fd->focus, -1);   // parked: on_key gives the menu its keys
    uapp_window_redraw(w);
}

// WHAT THE WIDGET WALK CANNOT SAY: which directory is listed and which
// type row is selected. Reported HERE rather than from uui_fileview's
// describe op, where it would cost every fileview in the tree two log
// lines a frame -- the File Manager has two panes, and its own test
// reads the same serial console the log goes out on.
static void log_layout(struct uapp_window *w) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;
    uapp_logf_layout("filedialog: layout view.dir %s\n", uui_fileview_dir(&fd->view));
    // Quoted, so an EMPTY field is a visible fact rather than a line
    // that looks truncated -- which is the state a test asserting the
    // name survived a hover has to tell apart from no line at all.
    uapp_logf_layout("filedialog: layout name.text \"%s\"\n",
                     uui_textbox_text(&fd->name));
    uapp_logf_layout("filedialog: layout view.rows %d\n", uui_fileview_row_count(&fd->view));
    // The selected row and where it is (details view), so a test can
    // right-click the row it means rather than guess the row height.
    const struct uui_table *t = &fd->view.table;
    const char *sel = uui_fileview_selected_name(&fd->view);
    int vr = t->selected >= 0 ? uui_table_view_row(t, t->selected) - t->top : 0;
    uapp_logf_layout("filedialog: layout view.selected \"%s\" %d %d %d %d\n", sel ? sel : "",
                     t->x, t->y + uui_table_header_h(t) + vr * uui_table_row_h(t), t->w,
                     uui_table_row_h(t));
    uapp_logf_layout("filedialog: layout note \"%s\" undo %d\n", fd->note_text, fd->can_undo);
    uapp_logf_layout("filedialog: layout renaming %d ctx %d ask %d\n",
                     uui_fileview_renaming(&fd->view), uui_menubar_is_open(&fd->menu),
                     uui_dialog_is_open(&fd->ask));
    if (fd->filter_count)
        uapp_logf_layout("filedialog: layout type.selected %d\n",
                         uui_dropdown_selected(&fd->type));
}

static void on_close(struct uapp_window *w) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (fd) finish(fd, 0);
    else uapp_window_close(w);
}

// --- the fileview's callbacks ------------------------------------------

static void view_select(void *ctx, const char *path, int is_dir) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    if (is_dir || fd->mode == UUI_FILEDIALOG_FOLDER) return;
    // THE SELECTED NAME LANDS IN THE FIELD, which is what makes saving
    // over an existing file one click. The BASENAME, because the field
    // is relative to the listing.
    if (path) uui_textbox_set_text(&fd->name, k_path_basename(path));
}

static void view_open(void *ctx, const char *path) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    // A double click (or Enter) on a FILE is the commit. A directory
    // never reaches here -- the widget descends into those itself.
    if (fd->mode == UUI_FILEDIALOG_FOLDER) return;
    finish(fd, path);
}

static void view_dir_changed(void *ctx, const char *dir) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    (void)dir;
    set_note(fd, "", 0);   // the strip was about the folder left
    // EVERY CHANGE IS RECORDED HERE, by result: Places, the breadcrumb,
    // Up and a double click all end in set_dir(), which reports here.
    if (!fd->replaying) unavhist_record(&fd->hist, uui_fileview_dir(&fd->view));
    sync_places(fd);
}

// --- opening -----------------------------------------------------------

void uui_filedialog_close_window(struct uui_filedialog *fd) {
    if (!fd || !fd->win) return;
    struct uapp_window *w = fd->win;
    fd->win = 0;
    drop_undo(fd);
    uui_fileview_free(&fd->view);       // its listing is the view's own, grown to the folder
    free(fd->ops);
    fd->ops = 0;
    fd->op_n = 0;
    if (g_open == fd) g_open = 0;
    uapp_window_close(w);
}

int uui_filedialog_is_open(const struct uui_filedialog *fd) {
    return fd && fd->win && uapp_window_is_open(fd->win);
}

struct uapp_window *uui_filedialog_open(struct uapp *a, struct uui_filedialog *fd,
                                         const struct uui_filedialog_opts *opts,
                                         void (*on_done)(void *ctx, const char *path),
                                         void *ctx) {
    if (!a || !fd || !opts) return 0;
    if (fd->win) return 0;   // modal: one at a time

    fd->on_done = on_done;
    fd->ctx = ctx;
    fd->mode = opts->mode;
    uapp_display_name(a, fd->app, sizeof fd->app);

    const char *deflt = opts->mode == UUI_FILEDIALOG_SAVE   ? "Save File"
                      : opts->mode == UUI_FILEDIALOG_FOLDER ? "Choose Folder"
                                                            : "Open File";
    strlcpy(fd->title, opts->title && opts->title[0] ? opts->title : deflt, sizeof fd->title);

    // --- the widgets ---------------------------------------------------
    uui_toolbar_init(&fd->bar, g_tools, (int)(sizeof g_tools / sizeof g_tools[0]));
    fd->bar.item_flags = tool_flags;

    build_places(fd);
    uui_pathbar_init(&fd->path, "System", "drive");

    uui_fileview_init(&fd->view, 0, 0, 10, 10);
    uui_fileview_set_mode(&fd->view, UUI_FILEVIEW_DETAILS);
    fd->filters = opts->filters;
    fd->filter_count = opts->filter_count > UUI_FILEDIALOG_FILTERS
                           ? UUI_FILEDIALOG_FILTERS : opts->filter_count;
    if (fd->filter_count < 0) fd->filter_count = 0;
    for (int i = 0; i < fd->filter_count; i++)
        fd->type_labels[i] = fd->filters[i].label;
    uui_dropdown_init(&fd->type, 0, 0, 10, 10, fd->type_labels, fd->filter_count);
    if (opts->filter_index > 0 && opts->filter_index < fd->filter_count)
        fd->type.list.selected = opts->filter_index;
    uui_fileview_set_filter(&fd->view, filter_gate, fd);
    fd->view.on_select = view_select;
    fd->view.on_open = view_open;
    fd->view.on_dir_changed = view_dir_changed;
    fd->view.ctx = fd;
    unavhist_init(&fd->hist);
    fd->replaying = 0;
    fd->undone = 0;
    fd->undone_n = 0;
    fd->ops = 0;
    fd->op_n = 0;
    fd->can_undo = 0;
    fd->note_text[0] = '\0';
    const char *start = (opts->start_dir && opts->start_dir[0]) ? opts->start_dir : "/";
    if (!uui_fileview_set_dir(&fd->view, start)) uui_fileview_set_dir(&fd->view, "/");

    uui_label_init(&fd->name_label, opts->mode == UUI_FILEDIALOG_FOLDER ? "Folder:" : "File name:");
    uui_label_init(&fd->type_label, "Files of type:");
    uui_textbox_init(&fd->name, opts->initial_name ? opts->initial_name : "");
    uui_label_init(&fd->spacer, "");
    uui_label_init(&fd->note, fd->note_text);
    uui_button_init(&fd->undo, 0, 0, 0, 0, "Undo", UUI_ROLE(control_bg), UUI_ROLE(text), ID_UNDO);
    uui_menubar_init(&fd->menu, 0, 0);   // no bar: a context menu (ui/uui_menubar.h)
    fd->menu.item_flags = ctx_flags;
    fd->menu_armed = 0;
    uui_dialog_init(&fd->ask);
    fd->was_renaming = 0;

    const char *ok_label = opts->mode == UUI_FILEDIALOG_SAVE   ? "Save"
                         : opts->mode == UUI_FILEDIALOG_FOLDER ? "Choose"
                                                               : "Open";
    uui_button_init(&fd->ok, 0, 0, 0, 0, ok_label, UUI_ROLE(accent), UUI_ROLE(accent_text), ID_OK);
    uui_button_init(&fd->cancel, 0, 0, 0, 0, "Cancel", UUI_ROLE(control_bg), UUI_ROLE(text), ID_CANCEL);

    // --- the layout ----------------------------------------------------
    //
    // A COLUMN of four rows. The body is the only thing that grows, so
    // it carries both FILL flags and everything else keeps its natural
    // height -- which is what stops the button row being pushed off the
    // bottom of a short window (docs/conventions/gui.md).
    fd->body_items[0] = (struct uui_item){ .ops = &uui_places_ops, .widget = &fd->places,
                                           .flags = UUI_FILL_H, .id = ID_PLACES,
                                           .main_size = ugfx_char_w() * 15, .name = "places" };
    fd->body_items[1] = (struct uui_item){ .ops = &uui_fileview_ops, .widget = &fd->view,
                                           .flags = UUI_FILL_W | UUI_FILL_H, .id = ID_VIEW,
                                           .name = "view" };
    // **MARGIN 1, NOT 0.** uui_layout_margin() reads anything <= 0 as
    // "the font-derived default" (ui/uui_layout.c), so a nested row
    // asking for no padding gets a character cell of it on every side --
    // three of those stacked took a third of the window's height.
    fd->body = (struct uui_layout){ .dir = UUI_ROW, .items = fd->body_items, .count = 2,
                                    .margin = 1 };

    // **UUI_FILL_H ON THE LABELS, AND IT IS WHAT CENTRES THEM.** A
    // label's natural height is one text row, so without it the box is
    // short and sits at the TOP of a row whose field is half as tall
    // again -- the caption then reads as belonging to whatever is
    // above. uui_label already centres its text inside a taller box;
    // the flag is what gives it one. Both labels are pinned to the same
    // width so the two fields line up, which is what a form layout does
    // (Win32 dialog units, QFormLayout, GtkGrid).
    int label_w = ugfx_text_width("Files of type:") + ugfx_char_w();
    fd->name_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->name_label,
                                           .flags = UUI_FILL_H,
                                           .main_size = label_w, .name = "namelabel" };
    fd->name_items[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &fd->name,
                                           .flags = UUI_FILL_W, .id = ID_NAME, .name = "name" };
    fd->name_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->name_items, .count = 2,
                                        .margin = 1 };

    fd->type_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->type_label,
                                           .flags = UUI_FILL_H,
                                           .main_size = label_w, .name = "typelabel" };
    fd->type_items[1] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &fd->type,
                                           .flags = UUI_FILL_W, .id = ID_TYPE, .name = "type" };
    fd->type_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->type_items, .count = 2,
                                        .margin = 1 };

    // An EMPTY LABEL that takes the leftover width, which is what pushes
    // the buttons to the right edge -- UUI_FILL_W in a row absorbs the
    // slack (ui/uui_layout.h), and a row with nothing stretchable would
    // leave them hard against the left.
    fd->button_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->spacer,
                                             .flags = UUI_FILL_W };
    fd->button_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &fd->ok,
                                             .id = ID_OK, .name = "ok" };
    fd->button_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &fd->cancel,
                                             .id = ID_CANCEL, .name = "cancel" };
    fd->button_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->button_items, .count = 3,
                                          .margin = 1 };

    // The strip under the listing: what the last change did, and Undo.
    fd->note_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->note,
                                           .flags = UUI_FILL_W | UUI_FILL_H, .name = "note" };
    fd->note_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &fd->undo,
                                           .id = ID_UNDO, .name = "undo" };
    fd->note_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->note_items, .count = 2,
                                        .margin = 1 };

    // The toolbar and the breadcrumb share the top row, the breadcrumb
    // taking what the buttons leave.
    fd->top_items[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &fd->bar,
                                          .flags = UUI_FILL_H, .id = ID_BAR, .name = "bar" };
    fd->top_items[1] = (struct uui_item){ .ops = &uui_pathbar_ops, .widget = &fd->path,
                                          .flags = UUI_FILL_W | UUI_FILL_H, .id = ID_PATH,
                                          .name = "path" };
    fd->top_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->top_items, .count = 2,
                                       .margin = 1 };
    fd->root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->top_row,
                                           .flags = UUI_FILL_W, .name = "top" };
    fd->root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->body,
                                           .flags = UUI_FILL_W | UUI_FILL_H, .name = "body" };
    fd->root_items[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->note_row,
                                           .flags = UUI_FILL_W, .name = "noterow" };
    fd->root_items[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->name_row,
                                           .flags = UUI_FILL_W, .name = "namerow" };
    // THE TYPE ROW IS HIDDEN WHEN THERE ARE NO FILTERS rather than
    // absent: a combo with nothing in it is a control that reads as
    // broken, and an app that opens anything has no types to offer.
    fd->root_items[4] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->type_row,
                                           .flags = UUI_FILL_W, .name = "typerow",
                                           .hidden = fd->filter_count == 0 };
    fd->root_items[5] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->button_row,
                                           .flags = UUI_FILL_W, .name = "buttons" };
    // THE OVERLAYS COME LAST, routed and drawn but not laid out -- the
    // layout's count stops before them. The question is a modal over the
    // window, so it is the last of all (ui/uui_dialog.h).
    fd->root_items[6] = (struct uui_item){ .ops = &uui_menubar_ops, .widget = &fd->menu,
                                           .id = ID_CTX, .name = "ctxmenu" };
    fd->root_items[7] = (struct uui_item){ .ops = &uui_dialog_ops, .widget = &fd->ask,
                                           .id = ID_ASK, .name = "ask" };
    set_note(fd, "", 0);
    // THE FOLDER MODE HAS NO NAME FIELD to speak of -- the answer is the
    // listing's own directory -- but the row stays, showing which folder
    // OK would choose. Hiding it would leave the dialog with no text at
    // all saying what it is about to return.
    fd->root = (struct uui_layout){ .dir = UUI_COLUMN, .items = fd->root_items, .count = 6 };

    // Tab order: the chrome first, then the list, then the field. The
    // list is what opens focused -- it is what the user came to use.
    fd->focusables[FOCUS_BAR]    = (struct uui_focusable){ &fd->bar, &uui_toolbar_ops };
    fd->focusables[FOCUS_PATH]   = (struct uui_focusable){ &fd->path, &uui_pathbar_ops };
    fd->focusables[FOCUS_PLACES] = (struct uui_focusable){ &fd->places, &uui_places_ops };
    fd->focusables[FOCUS_VIEW]   = (struct uui_focusable){ &fd->view, &uui_fileview_ops };
    fd->focusables[FOCUS_NAME]   = (struct uui_focusable){ &fd->name, &uui_textbox_focus_ops };
    fd->focusables[FOCUS_TYPE]   = (struct uui_focusable){ &fd->type, &uui_dropdown_ops };
    uui_focus_init(&fd->focus, fd->focusables, FOCUS_COUNT);
    uui_focus_set(&fd->focus,
                  opts->mode == UUI_FILEDIALOG_SAVE ? FOCUS_NAME : FOCUS_VIEW);

    // --- the window ----------------------------------------------------
    //
    // FONT-DERIVED, like every size here (docs/gui-guidelines.md). Wide
    // enough for the Places strip plus a details view with room for the
    // Modified column; tall enough for about twenty rows.
    int w = ugfx_char_w() * 60;
    int h = ugfx_char_h() * 30;

    struct uapp_window_desc d = {
        .title = fd->title,
        .w = w, .h = h,
        .flags = UAPP_WIN_MODAL,
        .widgets = fd->root_items,
        .widget_count = 8,
        .layout = &fd->root,
        .focus = &fd->focus,
        .state = fd,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_close = on_close,
        .on_button = on_button,
        // ONE PREFIX FOR EVERY APP'S CHOOSER, because it is one widget:
        // a test that can drive Notepad's Open can drive the Image
        // Viewer's without learning a second vocabulary.
        .log_prefix = "filedialog",
        .on_log_layout = log_layout,
    };
    fd->win = uapp_window_open(a, &d);
    if (!fd->win) return 0;
    g_open = fd;
    sync_places(fd);
    return fd->win;
}
