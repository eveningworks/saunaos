// Undo and Redo for the File Manager's operations: lib/ufileundo's
// journal, what each finished operation records into it, and the toast
// that offers to take the latest back (Ctrl+Z / Ctrl+Y do the same).
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include "kpath.h"
#include "base/ufileundo.h"
#include "ui/uui_toast.h"
#include "ui/uui_anim.h"
#include "base/ulog.h"

static struct ufileundo g_undo;   // static: hundreds of KB (ufileundo.h)
struct uui_toast g_toast;
static unsigned long long g_toast_at;   // when it was shown, for its timeout
static int g_toast_redo;                // its button redoes rather than undoes

// A toast that has said its piece goes away by itself after this long --
// checked on the tick the app already runs, so it costs no timer.
#define TOAST_NS (8ull * 1000000000ull)

// The CARD look (uui_toast.h, chosen from mockups 2026-10-06): a light
// surface at the pane's corner, the operation's icon, an accent Undo and
// a dismiss slot -- a dark pill did not belong on this chrome.
static void toast(const char *icon, const char *text, const char *action, int redo) {
    uui_toast_show(&g_toast, text, uui_anim_ms(180), uui_anim_now_ns());
    uui_toast_set_action(&g_toast, action);
    g_toast.style = UUI_TOAST_CARD;
    g_toast.icon = icon;
    g_toast.closable = 1;
    g_toast_at = uui_anim_now_ns();
    g_toast_redo = redo;
}

void undo_toast_hide(void) { uui_toast_hide(&g_toast); }

int undo_toast_tick(void) {
    if (g_toast.shown && uui_anim_now_ns() - g_toast_at > TOAST_NS) {
        uui_toast_hide(&g_toast);
        return 1;
    }
    return 0;
}

static const char *folder_name(const char *dir) {
    if (!strcmp(dir, FM_BIN)) return "the Recycle Bin";
    if (dir[0] == '/' && !dir[1]) return "System";
    return k_path_basename(dir);
}

// A finished job, as the journal's entry and the toast's sentence.
void undo_record_job(int op, const char *what, char (*paths)[PATH_MAX_LEN],
                     char (*results)[PATH_MAX_LEN], int n, const char *dest) {
    (void)what;
    // A COMPRESS is undone like a copy: what it made goes to the bin.
    int kind = op == CMD_MOVE ? UFU_MOVE : op == CMD_COPY || op == CMD_COMPRESS ? UFU_COPY
             : op == CMD_TRASH ? UFU_TRASH : 0;
    if (!kind) return;
    int done = 0;
    for (int i = 0; i < n; i++) if (results[i][0]) done++;
    if (!done) return;

    char label[UFU_LABEL], one[PATH_MAX_LEN];
    if (done == 1) {
        for (int i = 0; i < n; i++)
            if (results[i][0]) { snprintf(one, sizeof one, "%s", k_path_basename(paths[i])); break; }
    } else {
        snprintf(one, sizeof one, "%d items", done);
    }
    if (op == CMD_COMPRESS) {
        for (int i = 0; i < n; i++)
            if (results[i][0]) { snprintf(one, sizeof one, "%s", k_path_basename(results[i])); break; }
        snprintf(label, sizeof label, "Compressed %d item%s into %s", n, n == 1 ? "" : "s", one);
    } else if (kind == UFU_TRASH) snprintf(label, sizeof label, "Moved %s to the Recycle Bin", one);
    else snprintf(label, sizeof label, "%s %s to %s", kind == UFU_MOVE ? "Moved" : "Copied",
                  one, folder_name(dest));

    struct ufu_op *rec = ufileundo_begin(&g_undo, kind, label);
    for (int i = 0; i < n; i++) {
        if (!results[i][0]) continue;
        int ok = kind == UFU_TRASH ? ufileundo_add(rec, paths[i], 0, results[i])
                                   : ufileundo_add(rec, paths[i], results[i], 0);
        if (!ok) { ulogf("files: out of memory recording undo at item %d of %d\n", i + 1, n); break; }
    }
    ufileundo_commit(&g_undo, rec);
    ulogf("files: undo recorded \"%s\" (%d step%s)\n", label, rec->count, rec->count == 1 ? "" : "s");
    toast(op == CMD_COMPRESS ? "tb-archive" : kind == UFU_TRASH ? "place-trash"
          : kind == UFU_MOVE ? "tb-move" : "tb-copy",
          label, "Undo", 0);
}

// A rename or a new item, which the app does itself rather than as a job.
void undo_record(int kind, const char *a, const char *b) {
    char label[UFU_LABEL];
    if (kind == UFU_RENAME) snprintf(label, sizeof label, "Renamed %s", k_path_basename(a));
    else snprintf(label, sizeof label, "Created %s", k_path_basename(b));
    struct ufu_op *rec = ufileundo_begin(&g_undo, kind, label);
    ufileundo_add(rec, a, b, 0);
    ufileundo_commit(&g_undo, rec);
    ulogf("files: undo recorded \"%s\" (1 step)\n", label);
}

// Many renames as ONE operation: one Ctrl+Z puts every name back.
// RECORDED THE WAY IT WAS DONE, through the temporary names (fm_modal.c's
// two passes): old -> temp for each, then temp -> new. An Undo walks the
// steps last to first, so a set whose new names are each other's old
// ones (two files swapped) comes back too; renamed straight, each step
// would land on a name that is taken. A pair whose name did not change
// is no step: undoing it would rename a file onto itself.
void undo_record_renames(const char *dir, char (*olds)[64], char (*news)[64], int n) {
    char label[UFU_LABEL], a[PATH_MAX_LEN], b[PATH_MAX_LEN], t[PATH_MAX_LEN], tmp[64];
    int changed = 0;
    for (int i = 0; i < n; i++) changed += strcmp(olds[i], news[i]) != 0;
    if (!changed) return;
    snprintf(label, sizeof label, "Renamed %d item%s", changed, changed == 1 ? "" : "s");
    struct ufu_op *rec = ufileundo_begin(&g_undo, UFU_RENAME, label);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            if (!strcmp(olds[i], news[i])) continue;
            snprintf(tmp, sizeof tmp, ".rename-%d-%s", i, olds[i]);
            if (!k_path_join(dir, olds[i], a, sizeof a) || !k_path_join(dir, news[i], b, sizeof b) ||
                !k_path_join(dir, tmp, t, sizeof t))
                continue;
            if (pass == 0) ufileundo_add(rec, a, t, 0);
            else ufileundo_add(rec, t, b, 0);
        }
    ufileundo_commit(&g_undo, rec);
    ulogf("files: undo recorded \"%s\" (%d steps)\n", label, rec->count);
    toast("tb-rename", label, "Undo", 0);
}

// Not while an operation runs: fm_job_undo() would refuse it as busy.
int undo_can(int redo) {
    if (g_job_count > 0) return 0;
    return redo ? ufileundo_next_redo(&g_undo) != 0 : ufileundo_next_undo(&g_undo) != 0;
}

void do_undo(int redo) {
    struct ufu_op *op = redo ? ufileundo_next_redo(&g_undo) : ufileundo_next_undo(&g_undo);
    if (!op) { set_note(redo ? "nothing to redo" : "nothing to undo"); return; }
    if (g_job_count > 0) { set_note("wait for the operation to finish"); return; }   // toast stays
    uui_toast_hide(&g_toast);
    ulogf("files: %s \"%s\"\n", redo ? "redo" : "undo", op->label);
    fm_job_undo(op, redo);
}

// The job is over: the cursor moves even when a step failed, since the
// others did run -- the failures are said, not retried. A job stopped
// after `ran` of its steps moves past those alone: the journal splits.
void undo_applied(int redo, int failures, int ran) {
    struct ufu_op *op = redo ? ufileundo_next_redo(&g_undo) : ufileundo_next_undo(&g_undo);
    if (!op) return;
    int total = op->count;
    char text[UFU_LABEL + 32];
    snprintf(text, sizeof text, "%s", op->label);   // op moves when it splits
    if (ufileundo_done_part(&g_undo, redo, ran))
        ulogf("files: out of memory splitting a stopped %s\n", redo ? "redo" : "undo");
    if (ran < total) {
        snprintf(text, sizeof text, "%s stopped: %d of %d %s", redo ? "Redo" : "Undo",
                 ran, total, redo ? "redone" : "undone");
        ulogf("files: %s stopped after %d of %d steps\n", redo ? "redo" : "undo", ran, total);
        if (ran) toast(redo ? "tb-redo" : "tb-undo", text, redo ? "Undo" : "Redo", !redo);
        return;
    }
    if (failures)
        snprintf(text, sizeof text, "%s: %d could not be %s", redo ? "Redo" : "Undo",
                 failures, redo ? "redone" : "undone");
    else if (!redo) {
        char label[UFU_LABEL];
        strlcpy(label, text, sizeof label);
        snprintf(text, sizeof text, "Undone: %s", label);
    }
    ulogf("files: %s done, %d failed\n", redo ? "redo" : "undo", failures);
    toast(redo ? "tb-redo" : "tb-undo", text, redo ? "Undo" : "Redo", !redo);
}

// The toast's button: whichever way the toast offers.
void undo_toast_action(void) { do_undo(g_toast_redo); }
