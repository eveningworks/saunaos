// File operations: one at a time on a worker thread, the rest waiting
// their turn in a queue.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "lib/icon_cache.h"
#include "lib/ufiletype.h"
#include "lib/udate.h"
#include "lib/human.h"
#include "lib/ufileop.h"
#include "lib/utrash.h"
#include "lib/ufileundo.h"
#include "lib/uarchive.h"
#include "ui/uui_dialog.h"
#include "ui/uui_transfers.h"
#include <pthread.h>
#include "lib/uclip.h"
#include "ui/ulog.h"
#include "kpath.h"
#include "lib/uwalk.h"
#include "lib/ugrow.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// --- the job queue ----------------------------------------------------
//
// ONE CHILD AT A TIME, over a QUEUE. Marking a set of files means an
// operation is N operations, and running them concurrently would need N
// pids, N exit codes and a way to say which one failed -- with one
// status line to say it in. Sequential is also what a person expects
// from a progress that counts "3 of 7".
//
// THE PATHS ARE SNAPSHOT WHEN THE QUEUE IS BUILT, not read from the
// marks as it runs. Marks name ROWS, every reload clears them
// (ui/uui_fileview.h says why), and this app reloads whenever
// SYS_FS_GENERATION moves -- which a copy in progress makes it do. A
// queue reading marks as it went would lose them halfway through its
// own work.
#define JOB_MAX 64

static char g_job_what[32];
static char g_job_path[JOB_MAX][PATH_MAX_LEN];
static int g_job_isdir[JOB_MAX];
int g_job_count, g_job_at;
static int g_job_op;
static char g_job_dest[PATH_MAX_LEN];
static int g_job_failures;
// WHERE EACH ITEM ENDED UP, written by the worker: the final path of a
// move or copy, an item's place in the bin, "" when nothing was done or
// the step could not be safely undone. What the undo journal records.
static char g_job_result[JOB_MAX][PATH_MAX_LEN];
// What to do once an extraction is done: 1 go to the folder, 2 open the
// one file (opening a member of an archive extracts it to /tmp first,
// Explorer's way).
static int g_extract_then;
static char g_extract_open[PATH_MAX_LEN];
// An Undo or Redo running as a job: the journal entry and the direction.
static struct ufu_op *g_job_undo;
// A compress's own facts: what to make, and whether to rename it after.
static int g_cmp_kind, g_cmp_level, g_cmp_rename;
static char g_cmp_out[PATH_MAX_LEN];
static int g_job_redo;

// --- jobs -------------------------------------------------------------
//
// A path handed to a CHILD goes as an argv vector (sys_spawn_argv(),
// uapp_spawn()), never through sys_spawn()'s string, which splits on
// whitespace -- names with spaces exist (F2 can make one).
// --- operations waiting their turn ---------------------------------
//
// A SECOND COPY WAITS rather than being refused "busy". Windows runs
// them side by side in one dialog and Dolphin as separate jobs; one
// worker here runs them in turn, which is what "3 of 7" can say
// honestly and what a single disk does fastest anyway. Only copy, move
// and delete wait: an archive, an Undo or Empty the bin carries state
// beyond a list of paths, and stays "busy".
//
// EVERY BUILDER COLLECTS INTO g_stage FIRST, never into the running
// job's g_job_* -- which a builder writing them while the worker reads
// them would corrupt. stage_take() then makes it the running job, queues
// it or drops it.
struct pending {
    int op;
    char what[32], dest[PATH_MAX_LEN];
    int count, cap;
    char (*path)[PATH_MAX_LEN];   // grown to fit
    int *isdir;
};
static struct pending g_stage;
static struct pending *g_queue;   // waiting, oldest first; main thread only
static int g_nqueue, g_queue_cap;

static void stage_free(struct pending *p) {
    free(p->path);
    free(p->isdir);
    memset(p, 0, sizeof *p);
}

static void stage_begin(int op, const char *what, const char *dest) {
    stage_free(&g_stage);
    g_stage.op = op;
    strlcpy(g_stage.what, what, sizeof g_stage.what);
    strlcpy(g_stage.dest, dest, sizeof g_stage.dest);
}

static int stage_add(const char *path, int isdir) {
    struct pending *p = &g_stage;
    if (p->count == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        void *np = realloc(p->path, sizeof p->path[0] * (size_t)cap);
        if (!np) { ulogf("files: out of memory at %d items\n", p->count); return 0; }
        p->path = np;
        void *ni = realloc(p->isdir, sizeof p->isdir[0] * (size_t)cap);
        if (!ni) { ulogf("files: out of memory at %d items\n", p->count); return 0; }
        p->isdir = ni;
        p->cap = cap;
    }
    strlcpy(p->path[p->count], path, PATH_MAX_LEN);
    p->isdir[p->count++] = isdir;
    return 1;
}

static int queueable(int op) {
    return op == CMD_COPY || op == CMD_MOVE || op == CMD_TRASH || op == CMD_DELETE;
}

// A FOLDER INTO ITSELF, refused before a byte moves: the engine would
// copy the tree into its own child until the disk filled. Checked where
// every copy and move is accepted, so a queued one cannot slip past it.
static int into_itself(const struct pending *p) {
    if (p->op != CMD_COPY && p->op != CMD_MOVE) return 0;
    for (int i = 0; i < p->count; i++) {
        size_t n = strlen(p->path[i]);
        if (strncmp(p->dest, p->path[i], n) == 0 && (p->dest[n] == '\0' || p->dest[n] == '/'))
            return 1;
    }
    return 0;
}

// The running job, from a staged or a queued one. JOB_MAX is the undo
// journal's bound per operation (fm_undo.c), so past it the rest is
// logged rather than silently lost.
static void install(const struct pending *p) {
    g_job_count = g_job_at = g_job_failures = 0;
    g_job_op = p->op;
    strlcpy(g_job_what, p->what, sizeof g_job_what);
    strlcpy(g_job_dest, p->dest, sizeof g_job_dest);
    for (int i = 0; i < p->count && g_job_count < JOB_MAX; i++) {
        strlcpy(g_job_path[g_job_count], p->path[i], PATH_MAX_LEN);
        g_job_isdir[g_job_count++] = p->isdir[i];
    }
    if (p->count > JOB_MAX) ulogf("files: %d items, one operation holds %d\n", p->count, JOB_MAX);
}

enum { STAGE_DROPPED, STAGE_RUN, STAGE_QUEUED };

// g_stage made the running job (STAGE_RUN: the caller starts it), queued
// behind it, or dropped with a note.
static int stage_take(void) {
    if (g_stage.count == 0) { stage_free(&g_stage); return STAGE_DROPPED; }
    if (into_itself(&g_stage)) {
        set_note("cannot move a folder into itself");
        stage_free(&g_stage);
        return STAGE_DROPPED;
    }
    if (g_job_count > 0) {
        if (!queueable(g_stage.op) || !UGROW(g_queue, g_queue_cap, g_nqueue + 1)) {
            set_note("busy");
            stage_free(&g_stage);
            return STAGE_DROPPED;
        }
        ulogf("files: queued %s, %d item%s, behind %s\n", g_stage.what, g_stage.count,
              g_stage.count == 1 ? "" : "s", g_job_what);
        g_queue[g_nqueue++] = g_stage;
        memset(&g_stage, 0, sizeof g_stage);   // the queue owns its arrays now
        set_note("queued");
        return STAGE_QUEUED;
    }
    install(&g_stage);
    stage_free(&g_stage);
    return STAGE_RUN;
}

// 1 when `fv`'s operands are the running job and the caller should start
// it; 0 when they were queued (nothing more to do) or there were none.
static int queue_from(int op, const char *what, struct uui_fileview *fv,
                      const char *dest) {
    stage_begin(op, what, dest);
    char path[PATH_MAX_LEN];
    int marks = uui_fileview_mark_count(fv);
    if (marks > 0) {
        for (int i = 0; i < marks; i++)
            if (uui_fileview_marked_path(fv, i, path, sizeof path) &&
                !stage_add(path, uui_fileview_marked_is_dir(fv, i))) break;
    } else if (uui_fileview_selected_path(fv, path, sizeof path)) {
        stage_add(path, uui_fileview_selected_is_dir(fv));
    }
    if (g_stage.count == 0) { set_note("nothing selected"); stage_free(&g_stage); return 0; }
    return stage_take() == STAGE_RUN;
}

// The commander's verbs: the ACTIVE pane's operands into the OTHER pane.
static int queue_from_selection(int op, const char *what) {
    return queue_from(op, what, active(), uui_fileview_dir(other()));
}

static int start_job(void);   // defined with the worker, below

// The queue from a list of PATHS -- a drop whose source is another
// window, whose files are in the drag slot rather than in a pane.
static int queue_from_paths(int op, const char *what, const struct uclip *c,
                            const char *dest) {
    stage_begin(op, what, dest);
    for (int i = 0; i < uclip_count(c); i++) {
        const char *p = uclip_path(c, i);
        if (!p) break;
        struct sys_stat st;
        if (!stage_add(p, sys_stat(p, &st) == 0 && st.is_dir)) break;
    }
    if (g_stage.count == 0) { set_note("nothing to drop"); stage_free(&g_stage); return 0; }
    return stage_take() == STAGE_RUN;
}

void do_drop_extern(const char *dest, int copy) {
    static struct uclip c;
    uclip_drag_load(&c);
    // Dropped on the Recycle Bin: deleted, the way a drop on Explorer's
    // bin is -- copy or not.
    if (strcmp(dest, FM_BIN) == 0) {
        if (queue_from_paths(CMD_TRASH, "Delete", &c, "")) start_job();
        return;
    }
    if (!queue_from_paths(copy ? CMD_COPY : CMD_MOVE, copy ? "Copy" : "Move", &c, dest))
        return;
    for (int i = 0; i < g_job_count; i++) {
        size_t n = strlen(g_job_path[i]);
        if (strncmp(g_job_dest, g_job_path[i], n) == 0 &&
            (g_job_dest[n] == '\0' || g_job_dest[n] == '/')) {
            g_job_count = 0;
            set_note("cannot move a folder into itself");
            return;
        }
    }
    start_job();
}

void do_drop(struct uui_fileview *src, const char *dest, int copy) {
    if (strcmp(dest, FM_BIN) == 0) {
        if (queue_from(CMD_TRASH, "Delete", src, "")) start_job();
        return;
    }
    // A folder dropped into itself (a tree node under the folder being
    // dragged, the case the target cannot see) is stage_take()'s refusal.
    if (!queue_from(copy ? CMD_COPY : CMD_MOVE, copy ? "Copy" : "Move", src, dest))
        return;
    // Where they already are is a no-op said out loud, for the
    // one target (a tree node) that does not know the items' directory.
    if (strcmp(g_job_dest, uui_fileview_dir(src)) == 0) {
        g_job_count = 0;
        set_note("already there");
        return;
    }
    start_job();
}

// --- the clipboard ----------------------------------------------------
//
// Ctrl+C and Ctrl+X put the marked set (or the selection) on the system
// clipboard; Ctrl+V acts on it. A CUT MOVES NOTHING until the paste --
// Explorer's and Dolphin's rule, and the reason the files are still
// where they were until then.
//
// This does NOT replace F5/F6. The commander's two-pane copy needs no
// carrier at all and stays the faster gesture; the clipboard is what
// lets a copy cross a navigation, which is the thing two panes cannot
// do.
static int clip_put(int op, const char *what) {
    struct uui_fileview *fv = active();
    // STATIC: struct uclip embeds the whole 64 KiB payload, which is
    // thirty times the ring-3 frame budget (lib/uclip.h).
    static struct uclip c;
    uclip_begin(&c, op);

    int marks = uui_fileview_mark_count(fv);
    char path[PATH_MAX_LEN];
    int n = 0, refused = 0;
    if (marks > 0) {
        for (int i = 0; i < marks; i++) {
            if (!uui_fileview_marked_path(fv, i, path, sizeof path)) continue;
            if (!uclip_add(&c, path)) { refused = 1; break; }
            n++;
        }
    } else if (uui_fileview_selected_path(fv, path, sizeof path)) {
        if (uclip_add(&c, path)) n = 1; else refused = 1;
    }

    if (n == 0) { set_note("nothing selected"); return 0; }
    // A REFUSAL IS SAID OUT LOUD and puts nothing on the clipboard: a
    // partial cut set pasted is files silently left behind.
    if (refused || !uclip_commit(&c)) {
        set_note("too many to copy at once");
        ulogf("files: clipboard refused %d entries\n", n);
        return 0;
    }
    snprintf(g_stat_note, sizeof g_stat_note, "%s %d item%s", what, n,
              n == 1 ? "" : "s");
    return 1;
}

void clip_copy(void) { clip_put(UCLIP_COPY, "Copied"); }
void clip_cut(void)  { clip_put(UCLIP_CUT, "Cut"); }

// The paste. The destination is the ACTIVE pane's directory -- where you
// are -- which is both Explorer's rule and the commander's.
void clip_paste(void) {
    static struct uclip c;   // see clip_put() on why it is static
    uclip_load(&c);
    int op = uclip_op(&c), n = uclip_count(&c);
    ulogf("files: paste op=%d n=%d jobs=%d into %s\n", op, n, g_job_count,
          uui_fileview_dir(active()));
    if (op == UCLIP_NONE || n == 0) { set_note("clipboard is empty"); return; }

    stage_begin(op == UCLIP_CUT ? CMD_MOVE : CMD_COPY, op == UCLIP_CUT ? "Move" : "Copy",
                uui_fileview_dir(active()));
    for (int i = 0; i < n; i++) {
        const char *src = uclip_path(&c, i);
        if (!src) break;
        // PASTING INTO THE DIRECTORY A FILE IS ALREADY IN would ask
        // /bin/cp to copy a file onto itself. Skipped rather than
        // refused: pasting a mixed set where one happens to be here
        // should still move the rest.
        char dir[PATH_MAX_LEN];
        k_path_dirname(src, dir, sizeof dir);
        if (strcmp(dir, g_stage.dest) == 0) continue;
        if (!stage_add(src, 0)) break;   // the engine asks a path what it is
    }

    if (g_stage.count == 0) { set_note("already here"); stage_free(&g_stage); return; }
    int took = stage_take();
    if (took == STAGE_DROPPED) return;

    // A CUT IS SPENT BY ITS PASTE. Explorer clears the clipboard after a
    // cut-paste for the reason that matters: the files are no longer
    // where the clipboard says they are, so a second paste would fail
    // on every one of them.
    if (op == UCLIP_CUT) { (void)uclip_clear(); refresh_dim(); }
    if (took == STAGE_RUN) start_job();
}

// --- the worker -------------------------------------------------------
//
// THE OPERATION RUNS ON A THREAD, not on the event loop and not in a
// spawned child. A loop here would freeze the window (CLAUDE.md's
// long-work rule); a child cannot report progress, be cancelled, or be
// asked anything. A thread can do all three, and lib/ufileop.h is the
// engine it drives -- the same one /bin/cp runs, so the copying is
// still tested as text at a prompt.
//
// **THE WORKER TOUCHES NOTHING IN TOYKIT.** It writes into the block
// below under `g_lock` and posts; every widget, every draw and every
// decision happens on the main thread (ui/uapp.h's rule). The one thing
// it reads without the lock is `g_cancel`, which is a single int that
// only ever goes 0 -> 1.
static pthread_t g_worker;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ufileop g_engine;     // ~30 KB; the worker's alone
// g_app is fm_internal.h's now -- one app pointer, set in on_open(),
// used here for uapp_post and by files.c for uapp_spawn.

static volatile int g_running;      // a job is in flight
static volatile int g_cancel;       // the main thread asked it to stop

// Progress. Written by the worker under the lock and read by the main
// thread ON ITS TICK -- NOT posted per chunk, which at 4 KB a post
// would be thousands of events for one file. A status line does not
// need finer than the tick it is already drawn on.
static char g_cur_name[PATH_MAX_LEN];
static unsigned long long g_cur_done, g_cur_size;
static int g_cur_index;
// THE WHOLE OPERATION IN BYTES, for a copy or a move: counted before the
// first byte moves (g_counting while it is), then advanced per chunk.
// Explorer's "Calculating..." pass, and the only way a bar can say how
// far through ALL of it rather than through one file.
static int g_counting, g_by_bytes;
static unsigned long long g_bytes_total, g_bytes_done;
static unsigned long long g_item_bytes[JOB_MAX];
static unsigned long long g_item_base;    // bytes of the items before this one
static unsigned long long g_file_base;    // ...and of this item's files before this file
static unsigned long long g_file_size;
static char g_file_path[PATH_MAX_LEN];
// PAUSED by the person: the worker waits between chunks, holding no lock.
// Written by the main thread, a single int, read without the lock.
static volatile int g_paused;

// The conflict handshake. The worker parks here until the main thread
// answers -- see fm_conflict_answer().
static volatile int g_conflict_pending;
static volatile int g_conflict_answer = -1;
static int g_item_first;   // the FIRST conflict answer this item drew, -1 for none
static char g_conflict_src[PATH_MAX_LEN], g_conflict_dst[PATH_MAX_LEN];
static char g_conflict_rename[PATH_MAX_LEN];

// --- the policy the worker supplies to the engine ---------------------

// Waits out a pause; 0 once cancelled.
static int pause_point(void) {
    while (g_paused && !g_cancel) sys_sleep_ms(50);
    return !g_cancel;
}

static int worker_progress(void *ctx, const char *path,
                            uint64_t done, uint64_t total) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    strlcpy(g_cur_name, k_path_basename(path), sizeof g_cur_name);
    g_cur_done = done;
    g_cur_size = total;
    // A NEW FILE: the one before it is whole. The engine reports per
    // chunk and by path, and a file of no bytes is never reported at all.
    if (strcmp(path, g_file_path) != 0) {
        g_file_base += g_file_size;
        g_file_size = total;
        strlcpy(g_file_path, path, sizeof g_file_path);
    }
    if (g_by_bytes) {
        unsigned long long at = g_item_base + g_file_base + done;
        unsigned long long end = g_item_base + g_item_bytes[g_cur_index];
        g_bytes_done = at < end ? at : end;   // a file that grew mid-copy
    }
    pthread_mutex_unlock(&g_lock);
    return pause_point();  // 0 cancels the whole operation
}

// One item's bytes: a file's size, a folder's files summed (lib/uwalk.h).
static int count_visit(void *ctx, const char *path, const struct sys_dirent *e) {
    (void)path;
    if (g_cancel) return -1;
    if (!e->is_dir) *(unsigned long long *)ctx += e->size;
    return 1;
}

static unsigned long long item_bytes(const char *path, int isdir) {
    unsigned long long n = 0;
    struct sys_stat st;
    if (!isdir && sys_stat(path, &st) == 0 && !st.is_dir) return st.size;
    static struct uwalk w;   // several KB; the worker's alone
    if (uwalk_begin(&w, path))
        while (!g_cancel && uwalk_step(&w, count_visit, &n)) {
            pthread_mutex_lock(&g_lock);
            g_bytes_total = g_item_base + n;   // the count, as it grows
            pthread_mutex_unlock(&g_lock);
        }
    return n;
}

static void count_job(void) {
    pthread_mutex_lock(&g_lock);
    g_by_bytes = g_job_op == CMD_COPY || g_job_op == CMD_MOVE;
    g_bytes_total = g_bytes_done = g_item_base = 0;
    g_counting = g_by_bytes;
    pthread_mutex_unlock(&g_lock);
    if (!g_by_bytes) return;
    unsigned long long sum = 0;
    for (int i = 0; i < g_job_count && !g_cancel; i++) {
        g_item_base = sum;   // item_bytes() reports the running total from it
        g_item_bytes[i] = item_bytes(g_job_path[i], g_job_isdir[i]);
        sum += g_item_bytes[i];
    }
    pthread_mutex_lock(&g_lock);
    g_bytes_total = sum;
    g_item_base = 0;
    g_counting = 0;
    pthread_mutex_unlock(&g_lock);
}

static void worker_error(void *ctx, const char *path, int err) {
    (void)ctx;
    // COUNTED, not shown one by one: a tree with fifty unreadable files
    // would otherwise be fifty dialogs. The count reaches the status
    // line and the detail reaches the log.
    pthread_mutex_lock(&g_lock);
    g_job_failures++;
    pthread_mutex_unlock(&g_lock);
    ulogf("files: %s: errno %d\n", path, err);
}

static int worker_conflict(void *ctx, const char *src, const char *dst,
                            char *rename_out, int cap) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    strlcpy(g_conflict_src, src, sizeof g_conflict_src);
    strlcpy(g_conflict_dst, dst, sizeof g_conflict_dst);
    g_conflict_rename[0] = '\0';
    g_conflict_answer = -1;
    g_conflict_pending = 1;
    pthread_mutex_unlock(&g_lock);

    uapp_post(g_app, POST_CONFLICT, 0);

    // PARKED BY SLEEPING, not by pthread_cond_wait, and not by yielding.
    // This wait lasts as long as a person takes to read a dialog --
    // seconds, sometimes minutes -- and cond_wait spins on sys_yield()
    // (userland/libc/pthread.c), which would burn a scheduler slot the
    // whole time. 30 ms is invisible to a human and costs nothing.
    // `< 0` IS THE WAIT, not `!answer`. -1 is the no-answer sentinel and
    // it is TRUTHY, so `!g_conflict_answer` fell through instantly: the
    // worker never waited, read -1 as a decision, and overwrote the file
    // while the dialog was still on screen asking about it.
    while (g_conflict_answer < 0 && !g_cancel) sys_sleep_ms(30);
    if (g_cancel) return UFILEOP_CANCEL;

    pthread_mutex_lock(&g_lock);
    int d = g_conflict_answer;
    if (g_item_first < 0) g_item_first = d;
    if (d == UFILEOP_RENAME) strlcpy(rename_out, g_conflict_rename, (size_t)cap);
    g_conflict_pending = 0;
    pthread_mutex_unlock(&g_lock);
    ulogf("files: conflict answered d=%d name=%s\n", d,
          d == UFILEOP_RENAME ? rename_out : "-");
    return d;
}

// --- extraction (the inside of a .zip, fm_zip.c) --------------------------

// Every folder of `path` that is not there yet. 0, or -errno.
static int mkdirs(const char *path) {
    char p[PATH_MAX_LEN];
    strlcpy(p, path, sizeof p);
    for (char *s = p + 1; ; s++) {
        if (*s == '/' || !*s) {
            char c = *s;
            *s = '\0';
            struct sys_stat st;
            if (sys_stat(p, &st) != 0 && sys_mkdir(p) != 0) return -sys_errno();
            *s = c;
            if (!c) break;
        }
    }
    return 0;
}

struct xjob { const char *archive, *prefix, *dest; size_t plen, cut; };

static int x_progress(void *ctx, unsigned long long done, unsigned long long total) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    g_cur_done = (unsigned long)done;
    if (total) g_cur_size = (unsigned long)total;
    pthread_mutex_unlock(&g_lock);
    return g_cancel;   // non-zero stops
}

// One member, if it is the item or under it: routed under `dest` by its
// name from the item's PARENT on, so the item keeps its own name. A
// folder's record is made here and passed by.
static int x_route(void *ctx, const struct uarchive_entry *e, char *out, int cap) {
    struct xjob *x = ctx;
    const char *name = e->name;
    if (g_cancel) return 0;
    if (x->plen && strcmp(name, x->prefix) &&
        (strncmp(name, x->prefix, x->plen) || name[x->plen] != '/')) return 0;
    if (snprintf(out, (size_t)cap, "%s/%s", x->dest, name + x->cut) >= cap) {
        worker_error(0, name, ENAMETOOLONG);
        return 0;
    }
    size_t l = strlen(out);
    if (e->is_dir) {
        if (l && out[l - 1] == '/') out[l - 1] = '\0';
        if (mkdirs(out)) worker_error(0, out, EIO);
        return 0;
    }
    char parent[PATH_MAX_LEN];
    if (k_path_dirname(out, parent, sizeof parent)) mkdirs(parent);
    pthread_mutex_lock(&g_lock);
    strlcpy(g_cur_name, k_path_basename(out), sizeof g_cur_name);
    g_cur_size = (unsigned long)e->size;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static void extract_one(const char *item, const char *dest) {
    char archive[PATH_MAX_LEN], err[96];
    const char *inner;
    if (!arc_split(item, archive, sizeof archive, &inner)) { worker_error(0, item, EINVAL); return; }
    struct xjob x = { archive, inner, dest, strlen(inner), 0 };
    const char *slash = strrchr(inner, '/');
    x.cut = slash ? (size_t)(slash - inner) + 1 : 0;   // the item's parent, cut off
    // ONE PASS over the archive for all the members: a tar has no index.
    int rc = uarchive_extract_many(archive, x_route, &x, x_progress, 0, err, sizeof err);
    if (rc < 0 && !g_cancel) {
        worker_error(0, archive, -rc);
        ulogf("files: extract %s: %s\n", archive, err);
    }
}

static int c_progress(void *ctx, unsigned long long done, unsigned long long total) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    g_cur_done = (unsigned long)done;
    g_cur_size = (unsigned long)total;
    pthread_mutex_unlock(&g_lock);
    return g_cancel;
}

// ALL the items into one archive, run once rather than per item: the
// archive is the single result, and every member is named from the
// folder they share.
static void compress_all(void) {
    static const char *names[JOB_MAX];
    for (int i = 0; i < g_job_count; i++) names[i] = k_path_basename(g_job_path[i]);
    pthread_mutex_lock(&g_lock);
    strlcpy(g_cur_name, k_path_basename(g_cmp_out), sizeof g_cur_name);
    pthread_mutex_unlock(&g_lock);
    char err[128];
    int rc = uarchive_create(g_cmp_out, g_cmp_kind, g_cmp_level, g_job_dest, names, g_job_count,
                             c_progress, 0, err, sizeof err);
    if (rc < 0) {
        if (!g_cancel) worker_error(0, g_cmp_out, -rc);
        ulogf("files: compress %s: %s\n", g_cmp_out, err);
        return;
    }
    strlcpy(g_job_result[0], g_cmp_out, PATH_MAX_LEN);
}

static void *worker_main(void *arg) {
    (void)arg;
    struct ufileop_policy policy = {
        .on_conflict = worker_conflict,
        .on_progress = worker_progress,
        .on_error = worker_error,
    };

    count_job();
    unsigned long long base = 0;
    for (int i = 0; i < g_job_count && pause_point(); i++) {
        pthread_mutex_lock(&g_lock);
        g_cur_index = i;
        g_job_at = i;
        strlcpy(g_cur_name, k_path_basename(g_job_path[i]), sizeof g_cur_name);
        g_cur_done = g_cur_size = 0;
        g_item_base = base;
        g_file_base = g_file_size = 0;
        g_file_path[0] = '\0';
        if (g_by_bytes) g_bytes_done = base;
        pthread_mutex_unlock(&g_lock);
        base += g_by_bytes ? g_item_bytes[i] : 0;

        g_job_result[i][0] = '\0';
        g_item_first = -1;
        switch (g_job_op) {
        case CMD_COPY:
        case CMD_MOVE: {
            // A DESTINATION THAT ALREADY EXISTED is a merge or an
            // overwrite, and undoing one would recycle what was there
            // before -- so it is not recorded unless the first answer
            // renamed this item to a fresh name.
            char dest[PATH_MAX_LEN];
            struct sys_stat st;
            int ok = ufileop_resolve_dest(g_job_path[i], g_job_dest, dest, sizeof dest);
            int existed = ok && sys_stat(dest, &st) == 0;
            int r = g_job_op == CMD_COPY
                  ? ufileop_copy(g_job_path[i], g_job_dest, &g_engine, &policy)
                  : ufileop_move(g_job_path[i], g_job_dest, &g_engine, &policy);
            if (r != UFILEOP_OK || !ok) break;
            if (!existed) {
                strlcpy(g_job_result[i], dest, PATH_MAX_LEN);
            } else if (g_item_first == UFILEOP_RENAME) {
                char dir[PATH_MAX_LEN];
                if (k_path_dirname(dest, dir, sizeof dir))
                    k_path_join(dir, g_conflict_rename, g_job_result[i], PATH_MAX_LEN);
            }
            break;
        }
        case CMD_DELETE:
            ufileop_remove(g_job_path[i], 1, &g_engine, &policy);
            break;
        case CMD_TRASH: {
            struct utrash_item it;
            int e = utrash_put(g_job_path[i], &it);
            if (e) worker_error(0, g_job_path[i], -e);
            else utrash_item_path(&it, g_job_result[i], PATH_MAX_LEN);
            break;
        }
        case CMD_EXTRACT:
            extract_one(g_job_path[i], g_job_dest);
            break;
        case CMD_COMPRESS:
            compress_all();
            i = g_job_count;   // one archive for them all
            break;
        case CMD_UNDO: {
            int e = ufileundo_apply(g_job_undo, i, g_job_redo, &g_engine, &policy);
            if (e) worker_error(0, g_job_path[i], -e);
            break;
        }
        case CMD_PURGE:
            // The info file goes only once its item is gone: a half-
            // deleted folder stays listed, and Empty again finishes it.
            if (ufileop_remove(g_job_path[i], 1, &g_engine, &policy) == UFILEOP_OK &&
                utrash_forget_path(g_job_path[i]) != 0)
                worker_error(0, g_job_path[i], EIO);
            break;
        default: break;
        }
    }

    pthread_mutex_lock(&g_lock);
    if (g_by_bytes && !g_cancel) g_bytes_done = g_bytes_total;
    pthread_mutex_unlock(&g_lock);
    g_running = 0;
    uapp_post(g_app, POST_DONE, 0);
    return 0;
}

// --- what the app calls -----------------------------------------------

int fm_job_running(void) { return g_running; }

int fm_conflict_pending(void) { return g_conflict_pending; }
const char *fm_conflict_src(void) { return g_conflict_src; }
const char *fm_conflict_dst(void) { return g_conflict_dst; }

// The main thread's answer. `rename` is a basename and only read for
// UFILEOP_RENAME.
void fm_conflict_answer(int decision, const char *rename) {
    pthread_mutex_lock(&g_lock);
    if (rename) strlcpy(g_conflict_rename, rename, sizeof g_conflict_rename);
    // LAST, and after the payload: the worker wakes on this one word,
    // so anything it will read has to be in place before it is set.
    g_conflict_answer = decision;
    pthread_mutex_unlock(&g_lock);
}

void fm_job_cancel(void) { g_cancel = 1; }

void fm_job_pause(int paused) {
    if (!g_running) return;
    g_paused = paused ? 1 : 0;
    ulogf("files: job %s\n", paused ? "paused" : "resumed");
}

int fm_job_paused(void) { return g_running && g_paused; }

// "Copying", "Moving to the Recycle Bin" -- an operation as a row says it.
static const char *doing(int op, const char *what) {
    switch (op) {
    case CMD_COPY:     return "Copying";
    case CMD_MOVE:     return "Moving";
    case CMD_TRASH:    return "Moving to the Recycle Bin";
    case CMD_DELETE:   return "Deleting";
    case CMD_PURGE:    return !strcmp(what, "Empty") ? "Emptying the Recycle Bin" : "Deleting";
    case CMD_EXTRACT:  return "Extracting";
    case CMD_COMPRESS: return "Compressing";
    case CMD_UNDO:     return !strcmp(what, "Redo") ? "Redoing" : "Undoing";
    }
    return what;
}

static const char *icon_for(int op) {
    switch (op) {
    case CMD_MOVE:     return "tb-move";
    case CMD_TRASH: case CMD_DELETE: case CMD_PURGE: return "tb-delete";
    case CMD_EXTRACT: case CMD_COMPRESS: return "tb-archive";
    case CMD_UNDO:     return "tb-undo";
    }
    return "tb-copy";
}

// "Copying 12 items to Photos", "Copying dusk.bmp to Photos".
static void title(char *out, int cap, int op, const char *what, int count,
                  const char *first, const char *dest) {
    char items[PATH_MAX_LEN];
    if (count == 1) strlcpy(items, k_path_basename(first), sizeof items);
    else snprintf(items, sizeof items, "%d items", count);
    const char *where = dest[0] && (op == CMD_COPY || op == CMD_MOVE || op == CMD_EXTRACT)
                      ? k_path_basename(dest) : 0;
    if (where && !where[0]) where = "System";
    snprintf(out, (size_t)cap, "%s %s%s%s", doing(op, what), items, where ? " to " : "",
             where ? where : "");
}

// THE RATE, MEASURED HERE on the main thread from what the worker last
// recorded: sampled at most twice a second and smoothed, so one slow
// chunk does not make "about 4 s left" jump to minutes. A pause is not a
// slow second: the clock restarts on resume.
static unsigned long long g_rate, g_rate_bytes;
static uint64_t g_rate_t0;

static void rate_sample(unsigned long long done) {
    uint64_t now = sys_monotonic_ns();
    if (g_paused || g_counting || !g_rate_t0 || done < g_rate_bytes) {
        g_rate_t0 = now;
        g_rate_bytes = done;
        return;
    }
    if (now - g_rate_t0 < 500000000ull) return;
    unsigned long long inst = (done - g_rate_bytes) * 1000000000ull / (now - g_rate_t0);
    g_rate = g_rate ? (g_rate * 7 + inst * 3) / 10 : inst;
    g_rate_t0 = now;
    g_rate_bytes = done;
}

// The panel's rows: the running operation, then each one waiting.
int fm_job_rows(struct uui_xfer *out, int cap) {
    static char note[96];
    int n = 0;
    if (g_running && n < cap) {
        struct uui_xfer *x = &out[n++];
        memset(x, 0, sizeof *x);
        pthread_mutex_lock(&g_lock);
        int counting = g_counting, by_bytes = g_by_bytes, idx = g_cur_index;
        unsigned long long total = g_bytes_total, done = g_bytes_done;
        unsigned long long cdone = g_cur_done, csize = g_cur_size;
        strlcpy(note, g_cur_name, sizeof note);
        pthread_mutex_unlock(&g_lock);
        title(x->name, sizeof x->name, g_job_op, g_job_what, g_job_count, g_job_path[0], g_job_dest);
        x->state = g_paused ? UUI_XFER_PAUSED : UUI_XFER_RUNNING;
        x->can_pause = 1;
        x->icon = icon_for(g_job_op);
        if (counting) {
            // Nothing to measure yet: the figure is the count so far.
            char h[24];
            human_size_iec(h, sizeof h, total);
            snprintf(x->note, sizeof x->note, "counting first, %s so far", h);
        } else if (by_bytes) {
            x->got = done;
            x->total = total;
            rate_sample(done);
            x->rate = g_rate;
            strlcpy(x->note, note, sizeof x->note);
        } else if (g_job_op == CMD_EXTRACT || g_job_op == CMD_COMPRESS) {
            x->got = cdone;          // the archive's own count, in bytes
            x->total = csize;
            strlcpy(x->note, note, sizeof x->note);
        } else {
            x->items = 1;
            x->got = (unsigned long long)idx;
            x->total = (unsigned long long)g_job_count;
            strlcpy(x->note, note, sizeof x->note);
        }
    }
    for (int q = 0; q < g_nqueue && n < cap; q++) {
        const struct pending *p = &g_queue[q];
        struct uui_xfer *x = &out[n++];
        memset(x, 0, sizeof *x);
        title(x->name, sizeof x->name, p->op, p->what, p->count, p->path[0], p->dest);
        x->state = UUI_XFER_QUEUED;
        x->icon = icon_for(p->op);
    }
    return n;
}

int fm_job_waiting(void) { return g_nqueue; }

// A row's action: row 0 is the running one while there is one.
void fm_job_action(int ev, int row) {
    int q = row - (g_running ? 1 : 0);
    if (g_running && row == 0) {
        if (ev == UUI_XFER_EV_PAUSE)  fm_job_pause(1);
        if (ev == UUI_XFER_EV_RESUME) fm_job_pause(0);
        if (ev == UUI_XFER_EV_CANCEL) fm_job_cancel();
        return;
    }
    if (ev != UUI_XFER_EV_CANCEL || q < 0 || q >= g_nqueue) return;
    ulogf("files: dequeued %s, %d items\n", g_queue[q].what, g_queue[q].count);
    stage_free(&g_queue[q]);
    memmove(&g_queue[q], &g_queue[q + 1], sizeof g_queue[0] * (size_t)(g_nqueue - q - 1));
    g_nqueue--;
}

// The status line's text, built on the MAIN thread from the same row the
// panel draws: "Copying 5 of 12 \xb7 64%".
void fm_job_status(char *out, int cap) {
    struct uui_xfer x;
    if (!fm_job_rows(&x, 1)) { out[0] = '\0'; return; }
    int pct = x.total ? (int)(x.got * 100 / x.total) : 0;
    char tail[48] = "";
    if (g_nqueue) snprintf(tail, sizeof tail, " \xb7 %d waiting", g_nqueue);
    // One item is named; several are counted, "5 of 12".
    char which[PATH_MAX_LEN];
    if (g_job_count == 1) strlcpy(which, k_path_basename(g_job_path[0]), sizeof which);
    else snprintf(which, sizeof which, "%d of %d", g_cur_index + 1, g_job_count);
    if (x.state == UUI_XFER_PAUSED)
        snprintf(out, (size_t)cap, "%s, paused at %d%%%s", doing(g_job_op, g_job_what), pct, tail);
    else if (!x.total)
        snprintf(out, (size_t)cap, "%s %s%s", doing(g_job_op, g_job_what), which, tail);
    else
        snprintf(out, (size_t)cap, "%s %s \xb7 %d%%%s", doing(g_job_op, g_job_what), which, pct, tail);
}

static int start_job(void) {
    if (g_job_count == 0) return 0;
    g_cancel = 0;
    g_paused = 0;
    g_rate = 0;
    g_rate_t0 = 0;
    g_running = 1;
    g_job_failures = 0;
    if (pthread_create(&g_worker, 0, worker_main, 0) != 0) {
        g_running = 0;
        set_note("could not start the operation");
        ulog("files: pthread_create for the file worker FAILED\n");
        return 0;
    }
    // DETACHED IN EFFECT: nothing joins it. The app learns it finished
    // from the post, and a join on the main thread would be the freeze
    // this whole arrangement exists to avoid.
    return 1;
}

// Called from the tick. Returns 1 if the screen should be repainted.
int poll_job(void) {
    if (!g_running) return 0;
    return 1;      // the status line moves while it runs
}

// The post arrived: the operation is over.
void fm_job_finished(void) {
    char select_after[PATH_MAX_LEN] = "";
    snprintf(g_stat_note, sizeof g_stat_note, "%s %s", g_job_what,
              g_cancel ? "cancelled" : (g_job_failures ? "FAILED" : "done"));
    if (g_job_op == CMD_EXTRACT && !g_cancel && !g_job_failures) {
        int then = g_extract_then;
        g_extract_then = 0;
        if (then == 1) {
            char dest[PATH_MAX_LEN];
            strlcpy(dest, g_job_dest, sizeof dest);
            g_job_count = g_job_at = 0;
            fm_goto(g_active, dest);
        } else if (then == 2) {
            g_job_count = g_job_at = 0;
            open_path(g_extract_open);
        } else if (then == 3) {
            // Extract here: stay, and show the folder it made selected --
            // after the reload below, which would otherwise drop it.
            strlcpy(select_after, k_path_basename(g_job_dest), sizeof select_after);
        }
    }
    if (!g_cancel) {
        if (g_job_op == CMD_UNDO) undo_applied(g_job_redo, g_job_failures);
        else undo_record_job(g_job_op, g_job_what, g_job_path, g_job_result,
                             g_job_count, g_job_dest);
    }
    int compressed = g_job_op == CMD_COMPRESS && !g_cancel && !g_job_failures && g_job_result[0][0];
    g_job_count = g_job_at = 0;
    reload_panes();
    // THE NEW ARCHIVE, selected -- and for Compress to ZIP named in place,
    // Explorer's: the name it was given is a guess the user may not want.
    if (compressed && uui_fileview_select_name(active(), k_path_basename(g_job_result[0])) &&
        g_cmp_rename)
        uui_fileview_begin_rename(active());
    if (select_after[0]) uui_fileview_select_name(active(), select_after);
    // THE NEXT ONE WAITING starts now. Its paths were snapshot when it was
    // asked for, so a file it names may be gone: the engine reports that
    // per file, as for any other.
    if (g_nqueue > 0) {
        struct pending p = g_queue[0];
        memmove(&g_queue[0], &g_queue[1], sizeof g_queue[0] * (size_t)(g_nqueue - 1));
        g_nqueue--;
        install(&p);
        stage_free(&p);
        ulogf("files: starting the next, %s %d items\n", g_job_what, g_job_count);
        start_job();
    }
    refresh_status();
}

void do_compress(int kind, int level, const char *out, int rename_after) {
    struct uui_fileview *fv = active();
    if (!queue_from(CMD_COMPRESS, "Compress", fv, uui_fileview_dir(fv))) return;
    g_cmp_kind = kind;
    g_cmp_level = level;
    g_cmp_rename = rename_after;
    strlcpy(g_cmp_out, out, sizeof g_cmp_out);
    g_job_result[0][0] = '\0';
    start_job();
}

// A WHOLE ARCHIVE named by its path, from outside it -- Extract here and
// Extract to... on an archive in an ordinary folder. `then` as do_extract().
void do_extract_archive(const char *archive, const char *dest, int then) {
    if (g_job_count > 0) { set_note("busy"); return; }
    if (mkdirs(dest)) { set_note("could not make the folder to extract into"); return; }
    g_job_at = g_job_failures = 0;
    g_job_op = CMD_EXTRACT;
    strlcpy(g_job_what, "Extract", sizeof g_job_what);
    strlcpy(g_job_dest, dest, sizeof g_job_dest);
    snprintf(g_job_path[0], PATH_MAX_LEN, "%s%s", FM_ARC, archive);
    g_job_isdir[0] = 1;
    g_job_count = 1;
    g_extract_then = then;
    start_job();
}

void do_extract(int all, const char *dest, int then) {
    struct uui_fileview *fv = active();
    if (!in_archive(fv)) return;
    if (mkdirs(dest)) { set_note("could not make the folder to extract into"); return; }
    if (all) {
        char archive[PATH_MAX_LEN];
        const char *inner;
        if (g_job_count > 0) { set_note("busy"); return; }
        if (!arc_split(uui_fileview_dir(fv), archive, sizeof archive, &inner)) return;
        g_job_at = g_job_failures = 0;
        g_job_op = CMD_EXTRACT;
        strlcpy(g_job_what, "Extract", sizeof g_job_what);
        strlcpy(g_job_dest, dest, sizeof g_job_dest);
        snprintf(g_job_path[0], PATH_MAX_LEN, "%s%s", FM_ARC, archive);
        g_job_isdir[0] = 1;
        g_job_count = 1;
    } else if (!queue_from(CMD_EXTRACT, "Extract", fv, dest)) {
        return;
    }
    g_extract_then = then;
    if (then == 2) k_path_join(dest, k_path_basename(g_job_path[0]), g_extract_open, sizeof g_extract_open);
    start_job();
}

// Undo or Redo as a job: the steps run on the worker, a long move with
// progress like any other.
void fm_job_undo(struct ufu_op *op, int redo) {
    if (g_job_count > 0) { set_note("busy"); return; }
    g_job_at = g_job_failures = 0;
    g_job_op = CMD_UNDO;
    g_job_undo = op;
    g_job_redo = redo;
    strlcpy(g_job_what, redo ? "Redo" : "Undo", sizeof g_job_what);
    g_job_dest[0] = '\0';
    g_job_count = op->count < JOB_MAX ? op->count : JOB_MAX;
    for (int i = 0; i < g_job_count; i++) {
        const struct ufu_step *s = &op->step[i];
        strlcpy(g_job_path[i], s->b[0] ? s->b : s->a, PATH_MAX_LEN);
        g_job_isdir[i] = 0;
    }
    start_job();
}

// How many files an operation would act on, and what to call them.
int operand_count(void) {
    int marks = uui_fileview_mark_count(active());
    if (marks > 0) return marks;
    return uui_fileview_selected_name(active()) ? 1 : 0;
}

void do_copy(void) {
    if (queue_from_selection(CMD_COPY, "Copy")) start_job();
}

void do_move(void) {
    if (queue_from_selection(CMD_MOVE, "Move")) start_job();
}

// The dialog's rows are caller-owned for as long as it is up
// (ui/uui_dialog.h), so they live here rather than on the stack.
// The delete card's words, and its picture. File-scope because the dialog
// keeps pointers (ui/uui_dialog.h).
static char g_del_title[PATH_MAX_LEN + 24];
static char g_del_facts[96];
static char g_del_where[PATH_MAX_LEN + 8];
static const char *const g_del_rows[] = { g_del_facts, g_del_where };
static char g_del_path[PATH_MAX_LEN];   // the one file shown, for its thumbnail

// The picture on the card: the file's own thumbnail when one is ready,
// else its type's icon. Asked again when a thumbnail lands (files.c's
// on_user), since the first ask usually only queued the decode.
void delete_picture(void) {
    if (g_dialog_kind != DIALOG_DELETE || !uui_dialog_is_open(&g_dialog)) return;
    int px = ugfx_char_h() * 4;
    const struct uimg *pic = 0;
    if (g_del_path[0]) {
        struct sys_stat st;
        if (sys_stat(g_del_path, &st) == 0) {
            if (!st.is_dir && g_opt.folder.thumbs) {
                struct sys_dirent e;
                memset(&e, 0, sizeof e);
                strlcpy(e.name, k_path_basename(g_del_path), sizeof e.name);
                e.size = (uint32_t)st.size;
                e.modified = st.modified;
                char dir[PATH_MAX_LEN];
                k_path_dirname(g_del_path, dir, sizeof dir);
                pic = pane_thumb(0, dir, &e, px);
            }
            if (!pic) pic = icon_get(ufiletype_icon(k_path_basename(g_del_path), st.is_dir), px);
        }
    } else {
        pic = icon_get("file", px);
    }
    if (pic != g_dialog.picture) uui_dialog_set_picture(&g_dialog, pic);
}

// WHAT A DELETE DOES, decided when it is asked for: into the Recycle Bin
// (the default), for good (Shift+Delete, or a volume with no bin), or
// out of the bin itself (Delete inside it, or Empty).
enum { DEL_TRASH, DEL_FOREVER, DEL_PURGE, DEL_EMPTY };
static int g_del_mode;
static char g_del_note[96];

// Every operand has a bin to go to. One that does not -- /tmp is RAM,
// /boot is read-only -- makes the whole delete a permanent one, said
// on the card, rather than splitting one request into two behaviours.
static int all_binnable(struct uui_fileview *fv, char *volume, int cap) {
    char path[PATH_MAX_LEN], bin[PATH_MAX_LEN];
    int marks = uui_fileview_mark_count(fv);
    for (int i = 0; i < (marks ? marks : 1); i++) {
        int ok = marks ? uui_fileview_marked_path(fv, i, path, sizeof path)
                       : uui_fileview_selected_path(fv, path, sizeof path);
        if (ok && !utrash_bin_for(path, bin, sizeof bin)) {
            k_path_dirname(path, volume, (size_t)cap);
            return 0;
        }
    }
    return 1;
}

void do_delete(int forever) {
    int n = operand_count();
    if (n == 0) { set_note("nothing selected"); return; }
    struct uui_fileview *fv = active();
    char where[PATH_MAX_LEN] = "";
    g_del_mode = in_bin(fv) ? DEL_PURGE
               : forever ? DEL_FOREVER
               : all_binnable(fv, where, sizeof where) ? DEL_TRASH : DEL_FOREVER;
    // Options can say not to ask before the BIN -- never before a delete
    // that cannot be taken back.
    if (g_del_mode == DEL_TRASH && !g_opt.folder.confirm_delete) { commit_delete(); return; }

    int marks = uui_fileview_mark_count(fv);
    const char *verb = g_del_mode == DEL_TRASH ? "Move" : "Permanently delete";
    const char *tail = g_del_mode == DEL_TRASH ? " to the Recycle Bin?" : "?";
    g_del_path[0] = '\0';
    if (n == 1) {
        if (marks == 1) uui_fileview_marked_path(fv, 0, g_del_path, sizeof g_del_path);
        else uui_fileview_selected_path(fv, g_del_path, sizeof g_del_path);
        const char *name = g_del_mode == DEL_PURGE ? uui_fileview_selected_name(fv)
                                                    : k_path_basename(g_del_path);
        if (!name) name = k_path_basename(g_del_path);
        snprintf(g_del_title, sizeof g_del_title, "%s %s%s", verb, name, tail);
        struct sys_stat st;
        char h[24], when[48];
        if (sys_stat(g_del_path, &st) == 0) {
            udate_format(when, sizeof when, &st.modified, UDATE_DATE | UDATE_TIME);
            if (st.is_dir) {
                snprintf(g_del_facts, sizeof g_del_facts, "Folder, modified %s -- and everything in it", when);
            } else {
                human_size(h, sizeof h, st.size);
                snprintf(g_del_facts, sizeof g_del_facts, "%s, %s, modified %s",
                         ufiletype_name(name, 0), h, when);
            }
        } else {
            g_del_facts[0] = '\0';
        }
    } else {
        unsigned long long bytes = 0;
        int dirs = 0;
        char path[PATH_MAX_LEN], h[24];
        for (int i = 0; i < marks; i++) {
            struct sys_stat st;
            if (!uui_fileview_marked_path(fv, i, path, sizeof path)) continue;
            if (uui_fileview_marked_is_dir(fv, i)) dirs++;
            else if (sys_stat(path, &st) == 0) bytes += st.size;
        }
        human_size(h, sizeof h, bytes);
        snprintf(g_del_title, sizeof g_del_title, "%s %d items%s", verb, n, tail);
        if (dirs)
            snprintf(g_del_facts, sizeof g_del_facts, "%d file%s (%s) and %d folder%s with what is in them",
                     n - dirs, n - dirs == 1 ? "" : "s", h, dirs, dirs == 1 ? "" : "s");
        else
            snprintf(g_del_facts, sizeof g_del_facts, "%d files, %s in all", n, h);
    }
    if (g_del_mode == DEL_PURGE) strlcpy(g_del_where, "in the Recycle Bin", sizeof g_del_where);
    else snprintf(g_del_where, sizeof g_del_where, "in %s", uui_fileview_dir(fv));

    if (g_del_mode == DEL_TRASH)
        strlcpy(g_del_note, "You can restore it from the Recycle Bin.", sizeof g_del_note);
    else if (where[0])
        snprintf(g_del_note, sizeof g_del_note,
                 "%s has no Recycle Bin, so this cannot be taken back.", where);
    else
        strlcpy(g_del_note, "This cannot be taken back.", sizeof g_del_note);

    // Verb buttons, the KDE/GNOME/macOS rule: the button says what it
    // does. Red only for what cannot be taken back.
    static const struct uui_dialog_button bin_btns[] = {
        { "Cancel", DLG_CANCEL, 0 },
        { "Move",   DLG_DELETE, 0 },
    };
    static const struct uui_dialog_button del_btns[] = {
        { "Cancel", DLG_CANCEL, 0 },
        { "Delete", DLG_DELETE, UUI_DLG_DANGER },
    };
    g_dialog_kind = DIALOG_DELETE;
    uui_dialog_open(&g_dialog, g_del_title, g_del_rows, 2,
                    g_del_mode == DEL_TRASH ? bin_btns : del_btns, 2, 1, DLG_CANCEL);
    uui_dialog_set_note(&g_dialog, g_del_note);
    delete_picture();
}

void do_empty_bin(void) {
    if (bin_count() == 0) { set_note("the Recycle Bin is empty"); return; }
    char h[24];
    human_size(h, sizeof h, bin_bytes());
    g_del_mode = DEL_EMPTY;
    g_del_path[0] = '\0';
    snprintf(g_del_title, sizeof g_del_title, "Empty the Recycle Bin?");
    snprintf(g_del_facts, sizeof g_del_facts, "%d item%s, %s in all", bin_count(),
             bin_count() == 1 ? "" : "s", h);
    strlcpy(g_del_where, "on every disk", sizeof g_del_where);
    static const struct uui_dialog_button btns[] = {
        { "Cancel", DLG_CANCEL, 0 },
        { "Empty",  DLG_DELETE, UUI_DLG_DANGER },
    };
    g_dialog_kind = DIALOG_DELETE;
    uui_dialog_open(&g_dialog, g_del_title, g_del_rows, 2, btns, 2, 1, DLG_CANCEL);
    uui_dialog_set_note(&g_dialog, "They are deleted for good.");
    delete_picture();
}

void commit_delete(void) {
    switch (g_del_mode) {
    case DEL_TRASH:
        if (queue_from(CMD_TRASH, "Delete", active(), "")) start_job();
        break;
    case DEL_FOREVER:
        if (queue_from(CMD_DELETE, "Delete", active(), "")) start_job();
        break;
    case DEL_PURGE:
        if (queue_from(CMD_PURGE, "Delete", active(), "")) start_job();
        break;
    case DEL_EMPTY:
        if (g_job_count > 0) { set_note("busy"); return; }
        g_job_at = g_job_failures = 0;
        g_job_op = CMD_PURGE;
        strlcpy(g_job_what, "Empty", sizeof g_job_what);
        g_job_dest[0] = '\0';
        g_job_count = bin_paths(g_job_path, JOB_MAX);
        for (int i = 0; i < g_job_count; i++) g_job_isdir[i] = 0;
        start_job();
        break;
    }
}

void commit_mkdir(const char *name) {
    char path[PATH_MAX_LEN];
    ulogf("files: mkdir %s in %s\n", name, uui_fileview_dir(active()));
    if (!name[0]) return;
    if (!k_path_join(uui_fileview_dir(active()), name, path, sizeof path)) {
        set_note("path too long");
        return;
    }
    // Straight to the syscall: mkdir is one call that cannot block, so
    // spawning /bin/mkdir would buy the process isolation a long copy
    // needs and nothing else.
    if (sys_mkdir(path) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not create %s", name);
    } else {
        set_note("created");
        undo_record(UFU_CREATE, 0, path);
        reload_pane(active());
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}

// An empty file, named by the prompt. Refused, not overwritten, when
// the name is taken: "New" never destroys anything.
void commit_newfile(const char *name) {
    char path[PATH_MAX_LEN];
    if (!name[0]) return;
    if (!k_path_join(uui_fileview_dir(active()), name, path, sizeof path)) {
        set_note("path too long");
        return;
    }
    struct sys_stat st;
    if (sys_stat(path, &st) == 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s already exists", name);
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not create %s", name);
        return;
    }
    fclose(f);
    set_note("created");
    undo_record(UFU_CREATE, 0, path);
    reload_pane(active());
    uui_fileview_select_name(active(), name);
    refresh_status();
}

void commit_rename(const char *name) {
    const char *from = uui_fileview_selected_name(active());
    if (from) commit_rename_named(from, name);
}

void commit_rename_named(const char *from_name, const char *name) {
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    ulogf("files: rename %s to %s in %s\n", from_name, name, uui_fileview_dir(active()));
    if (!name[0]) return;
    if (!k_path_join(uui_fileview_dir(active()), from_name, from, sizeof from)) return;
    if (!k_path_join(uui_fileview_dir(active()), name, to, sizeof to)) {
        set_note("path too long");
        return;
    }
    if (sys_rename(from, to) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not rename to %s", name);
    } else {
        set_note("renamed");
        if (strcmp(from, to)) undo_record(UFU_RENAME, from, to);
        reload_pane(active());
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}
