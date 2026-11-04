// Compress: the marked items (or the selected one) into a new archive
// beside them, over lib/uarchive.h -- the job itself is fm_jobs.c's.
//
// TWO WAYS IN, chosen from mockups (2026-10-14). "Compress to ZIP" is
// Windows 11's: no question, a .zip named after the item the pointer is
// on, made at once and dropped into an in-place rename, with Undo on the
// toast. "Compress..." is Ark's: a dialog for the name, the format
// (.zip, .tar.gz, .tar) and how hard to try.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "lib/uarchive.h"
#include "lib/uinflate.h"
#include "lib/ufileop.h"
#include "lib/icon_cache.h"
#include "ui/uui_dialog.h"
#include "ui/uui_form.h"
#include "ui/ulog.h"
#include "kpath.h"
#include <stdio.h>
#include <string.h>

static const char *const FORMATS[] = { ".zip", ".tar.gz", ".tar" };
static const int FORMAT_KIND[] = { UARC_ZIP, UARC_TGZ, UARC_TAR };
static const char *const LEVELS[] = { "Store", "Fast", "Normal", "Best" };
static const int LEVEL_VALUE[] = { 0, UDEFLATE_FAST, UDEFLATE_NORMAL, UDEFLATE_BEST };

static struct uui_form g_form;
static struct uui_item g_form_item = { .ops = &uui_form_ops, .widget = &g_form, .name = "compress" };
static int g_row_name, g_row_format, g_row_level, g_row_note;
static int g_seen_edits = -1;
static char g_note[UUI_FILEVIEW_PATH_MAX + 64];
static char g_title[48];

// The items a compress takes: the marked ones, or the selected one.
static int item_count(void) {
    int marks = uui_fileview_mark_count(active());
    return marks > 0 ? marks : uui_fileview_selected_name(active()) ? 1 : 0;
}

// "aurora.jpg" -> "aurora", "notes" -> "notes": a file's last extension
// goes, a folder's name stays whole.
static void stem_of(const char *name, int is_dir, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s", name);
    char *dot = strrchr(out, '.');
    if (!is_dir && dot && dot != out) *dot = '\0';
}

// `dir`/`stem``suffix`, or "stem (2)" and on when that is taken.
static int free_path(const char *dir, const char *stem, const char *suffix, char *out, int cap) {
    char want[UUI_FILEVIEW_PATH_MAX], got[UUI_FILEVIEW_PATH_MAX];
    snprintf(want, sizeof want, "%s%s", stem, suffix);
    struct sys_stat st;
    if (!k_path_join(dir, want, out, (size_t)cap)) return 0;
    if (sys_stat(out, &st) != 0) return 1;
    if (!ufileop_unique_name(dir, want, got, sizeof got)) return 0;
    return k_path_join(dir, got, out, (size_t)cap);
}

// Somewhere a new file can go: not inside an archive, the bin or a
// results view, all of which are read-only.
static int can_compress_here(void) {
    struct uui_fileview *fv = active();
    if (in_archive(fv) || in_bin(fv) || in_search(fv) || in_recent(fv)) {
        set_note("compress from a folder -- this view is read-only");
        return 0;
    }
    if (!item_count()) { set_note("nothing selected"); return 0; }
    return 1;
}

static void compress_quick(void) {
    if (!can_compress_here()) return;
    const char *name = uui_fileview_selected_name(active());
    if (!name) {   // marked but nothing under the cursor: the first marked
        char p[PATH_MAX_LEN];
        if (!uui_fileview_marked_path(active(), 0, p, sizeof p)) return;
        name = k_path_basename(p);
    }
    char stem[UUI_FILEVIEW_PATH_MAX], out[PATH_MAX_LEN];
    stem_of(name, uui_fileview_selected_is_dir(active()), stem, sizeof stem);
    if (!free_path(uui_fileview_dir(active()), stem, ".zip", out, sizeof out)) {
        set_note("no free name for the archive");
        return;
    }
    ulogf("files: compress to zip -> %s\n", out);
    do_compress(UARC_ZIP, UDEFLATE_NORMAL, out, 1);
}

// The note under the choices: where it will go, and of how much.
static void note_update(void) {
    char out[PATH_MAX_LEN];
    int f = uui_form_choice(&g_form, g_row_format);
    const char *stem = uui_form_text(&g_form, g_row_name);
    int n = item_count();
    if (!stem[0] || !free_path(uui_fileview_dir(active()), stem, FORMATS[f < 0 ? 0 : f], out, sizeof out))
        snprintf(g_note, sizeof g_note, "Give the archive a name.");
    else
        snprintf(g_note, sizeof g_note, "Saved as %s -- %d item%s", out, n, n == 1 ? "" : "s");
    uui_form_set_note(&g_form, g_row_note, g_note);
}

static void compress_ask(void) {
    if (!can_compress_here()) return;
    int n = item_count();
    // One item names it; several take their folder's name, as Ark does.
    char stem[UUI_FILEVIEW_PATH_MAX];
    const char *one = uui_fileview_selected_name(active());
    if (n == 1 && one) stem_of(one, uui_fileview_selected_is_dir(active()), stem, sizeof stem);
    else {
        const char *dir = uui_fileview_dir(active());
        snprintf(stem, sizeof stem, "%s", strcmp(dir, "/") ? k_path_basename(dir) : "Archive");
    }

    uui_form_init(&g_form);
    g_row_name = uui_form_add_text(&g_form, "Name", stem, 0);
    g_row_format = uui_form_add_choice(&g_form, "Format", FORMATS, 3, 0, 0);
    g_row_level = uui_form_add_choice(&g_form, "Compression", LEVELS, 4, 2, 0);
    g_row_note = uui_form_add_note(&g_form, "");
    note_update();
    g_seen_edits = g_form.edits;

    static const struct uui_dialog_button btns[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "Compress", DLG_OK, UUI_DLG_PRIMARY },
    };
    snprintf(g_title, sizeof g_title, "Compress %d item%s", n, n == 1 ? "" : "s");
    g_dialog_kind = DIALOG_COMPRESS;
    g_modal = MODAL_PROMPT;
    uui_dialog_open(&g_dialog, g_title, 0, 0, btns, 2, 1, DLG_CANCEL);
    int fw, fh;
    uui_form_ops.natural_size(&g_form, &fw, &fh);
    int want = ugfx_char_w() * 44;
    uui_dialog_set_body(&g_dialog, &g_form_item, fw > want ? fw : want, fh);
    uui_dialog_focus(&g_dialog, &g_form_item);
    uui_form_focus(&g_form, g_row_name);
    uui_dialog_set_picture(&g_dialog, icon_get("file-archive", ugfx_char_h() * 4));
}

// The form changed since last frame: a .tar stores, so its level does
// not apply; a .tar.gz cannot store, so Store becomes Fast; the note
// names the file the choice now makes.
void compress_sync(void) {
    if (g_dialog_kind != DIALOG_COMPRESS || g_form.edits == g_seen_edits) return;
    g_seen_edits = g_form.edits;
    int f = uui_form_choice(&g_form, g_row_format);
    uui_form_set_disabled(&g_form, g_row_level, FORMAT_KIND[f < 0 ? 0 : f] == UARC_TAR);
    if (FORMAT_KIND[f < 0 ? 0 : f] == UARC_TGZ && uui_form_choice(&g_form, g_row_level) == 0)
        g_form.rows[g_row_level].seg.selected = 1;
    note_update();
}

void compress_answer(int code) {
    g_modal = MODAL_NONE;
    uui_dialog_set_body(&g_dialog, NULL, 0, 0);
    if (code != DLG_OK) { set_note("cancelled"); return; }
    int f = uui_form_choice(&g_form, g_row_format);
    int l = uui_form_choice(&g_form, g_row_level);
    const char *stem = uui_form_text(&g_form, g_row_name);
    char out[PATH_MAX_LEN];
    if (!stem[0] || strchr(stem, '/') ||
        !free_path(uui_fileview_dir(active()), stem, FORMATS[f < 0 ? 0 : f], out, sizeof out)) {
        set_note("not a name an archive can have");
        return;
    }
    int kind = FORMAT_KIND[f < 0 ? 0 : f];
    int level = LEVEL_VALUE[l < 0 ? 2 : l];
    if (kind == UARC_TGZ && level == 0) level = UDEFLATE_FAST;
    ulogf("files: compress %s level %d -> %s\n", FORMATS[f < 0 ? 0 : f], level, out);
    do_compress(kind, level, out, 0);
}

void compress_command(int code) {
    if (code == CMD_COMPRESS_ZIP) compress_quick();
    else if (code == CMD_COMPRESS_ASK) compress_ask();
}
