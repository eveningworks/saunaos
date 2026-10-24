// Install Windows 3.x: w16's drive C: given Windows' own files from its
// setup disks -- Disks, Check, Install, Done on a step rail (the OS
// installer's frame). The work is `w16 --install`'s, run as a child and
// read a line at a time: the disks are untrusted input, and w16 is the
// process that pledges before it reads them (lib/uwinsetup.h).
//
// Opened from Retro Launcher (Tools, and the bar it shows while drive C:
// has no Windows); NoDisplay in the Start menu.
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt/sys.h"         // and signal_abi.h: SIGTERM for Stop
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui_wizard.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_textbox.h"
#include "ui/uui_progress.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_primitives.h"
#include "lib/human.h"
#include "w16/w16prefs.h"   // the launcher's folders

#define W16_BIN      "/bin/w16"
#define LAUNCHER_BIN "/bin/wm/apps/retrolauncher"
#define LOG_LINES    8

enum { S_DISKS, S_CHECK, S_INSTALL, S_DONE, S_COUNT };
enum { ID_SRC = 1, ID_BROWSE, ID_KEEP, ID_STOP, ID_LAUNCHER, ID_DIALOG };
enum { ASK_OK = 1 };

static struct uapp *g_app;

// --- what the disks hold: `w16 --install DISKS --dry-run` ------------------------------

struct found {
    int ok;                       // the disks were read and a plan made
    char error[256];              // why not
    char title[96], lang[16], target[16], target_path[256];
    int disks, files, missing, in_windows, in_system;
    unsigned long long bytes;
    char missing_names[4][64];
};
static struct found g_found;
static char g_checked_src[512];   // what g_found describes

// A child's standard output AND error, to its end, into a fresh string.
static char *run_capture(char *const argv[]) {
    int fds[2];
    if (sys_pipe(fds) < 0) return 0;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = (char **)argv;
    o.stdout_fd = fds[1];
    o.stderr_fd = fds[1];
    int pid = sys_spawn_opts(W16_BIN, &o);
    sys_close(fds[1]);
    if (pid < 0) { sys_close(fds[0]); return 0; }
    char *out = 0;
    size_t n = 0, cap = 0;
    for (;;) {
        if (cap - n < 4096) {
            char *g = realloc(out, cap = cap ? cap * 2 : 16384);
            if (!g) { free(out); out = 0; break; }
            out = g;
        }
        int got = sys_read(fds[0], out + n, (int)(cap - n - 1));
        if (got <= 0) break;
        n += (size_t)got;
    }
    sys_close(fds[0]);
    sys_waitpid(pid, 0);
    if (out) out[n] = 0;
    return out;
}

static int starts(const char *line, const char *key, const char **rest) {
    size_t k = strlen(key);
    if (strncmp(line, key, k) || line[k] != ' ') return 0;
    *rest = line + k + 1;
    return 1;
}

static void check_disks(const char *src) {
    memset(&g_found, 0, sizeof g_found);
    snprintf(g_checked_src, sizeof g_checked_src, "%s", src);
    if (!src[0]) {
        snprintf(g_found.error, sizeof g_found.error, "Name the folder the disks are in.");
        return;
    }
    char *argv[] = { "w16", "--install", (char *)src, "--dry-run", 0 };
    char *out = run_capture(argv);
    if (!out) {
        snprintf(g_found.error, sizeof g_found.error, "w16 could not be started.");
        return;
    }
    for (char *line = strtok(out, "\n"); line; line = strtok(0, "\n")) {
        const char *v;
        if (starts(line, "setup", &v)) snprintf(g_found.title, sizeof g_found.title, "%s", v);
        else if (starts(line, "language", &v)) snprintf(g_found.lang, sizeof g_found.lang, "%s", v);
        else if (starts(line, "disks", &v)) g_found.disks = atoi(v);
        else if (starts(line, "files", &v)) g_found.files = atoi(v);
        else if (starts(line, "bytes", &v)) g_found.bytes = strtoull(v, 0, 10);
        else if (starts(line, "missing", &v)) {
            if (*v >= '0' && *v <= '9') g_found.missing = atoi(v);
            else if (g_found.missing_names[3][0] == 0) {
                for (int k = 0; k < 4; k++)
                    if (!g_found.missing_names[k][0]) {
                        snprintf(g_found.missing_names[k], sizeof g_found.missing_names[k], "%.63s", v);
                        break;
                    }
            }
        } else if (starts(line, "target", &v)) {
            const char *sp = strchr(v, ' ');
            snprintf(g_found.target, sizeof g_found.target, "%.*s", sp ? (int)(sp - v) : 15, v);
            if (sp) snprintf(g_found.target_path, sizeof g_found.target_path, "%s", sp + 1);
        } else if (starts(line, "file", &v)) {
            if (strstr(v, "\\SYSTEM\\")) g_found.in_system++;
            else g_found.in_windows++;
        } else if (!strncmp(line, "w16: ", 5)) {
            snprintf(g_found.error, sizeof g_found.error, "%s", line + 5);
        }
    }
    free(out);
    g_found.ok = !g_found.error[0] && g_found.files > 0;
    if (!g_found.ok && !g_found.error[0])
        snprintf(g_found.error, sizeof g_found.error, "These are not Windows 3.x setup disks.");
    ulogf("wininstall: %s -> %s\n", src, g_found.ok ? g_found.title : g_found.error);
}

// --- the run: `w16 --install DISKS`, a line at a time ---------------------------------

struct run {
    pthread_mutex_t lock;
    int pid;
    int running, finished, ok, stopped;
    int done, total;
    char current[96], current_disk[16];
    int current_disk_no;
    char lines[LOG_LINES][128];
    int nlines;
    char error[256];
    int kept;
};
static struct run g_run = { .lock = PTHREAD_MUTEX_INITIALIZER };
static uint64_t g_t0, g_took_ms;

static void run_line(char *line) {
    const char *v;
    pthread_mutex_lock(&g_run.lock);
    if (starts(line, "copy", &v) || starts(line, "kept", &v) || starts(line, "absent", &v)) {
        // "copy 3/256 C:\WINDOWS\SYSTEM\MAIN.CPL MAIN.CP_ 4"
        int i = 0, n = 0, disk = 0;
        char path[96] = "", from[16] = "";
        sscanf(v, "%d/%d %95s %15s %d", &i, &n, path, from, &disk);
        g_run.done = i;
        g_run.total = n;
        snprintf(g_run.current, sizeof g_run.current, "%s", path);
        snprintf(g_run.current_disk, sizeof g_run.current_disk, "%s", from);
        g_run.current_disk_no = disk;
        if (line[0] == 'k') g_run.kept++;
        if (line[0] != 'a') {
            if (g_run.nlines == LOG_LINES) {
                memmove(g_run.lines[0], g_run.lines[1], sizeof g_run.lines[0] * (LOG_LINES - 1));
                g_run.nlines--;
            }
            snprintf(g_run.lines[g_run.nlines++], sizeof g_run.lines[0], "%-13s %s %s",
                     line[0] == 'k' ? "(kept)" : from, line[0] == 'k' ? "  " : "->", path);
        }
    } else if (starts(line, "done", &v)) {
        g_run.ok = 1;
    } else if (!strncmp(line, "w16: ", 5)) {
        snprintf(g_run.error, sizeof g_run.error, "%s", line + 5);
    }
    pthread_mutex_unlock(&g_run.lock);
}

static void *reader(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t n = 0;
    for (;;) {
        int got = sys_read(fd, buf + n, (int)(sizeof buf - 1 - n));
        if (got <= 0) break;
        n += (size_t)got;
        char *nl;
        while ((nl = memchr(buf, '\n', n))) {
            *nl = 0;
            run_line(buf);
            size_t used = (size_t)(nl - buf) + 1;
            memmove(buf, nl + 1, n - used);
            n -= used;
        }
        if (n == sizeof buf - 1) n = 0;   // a line longer than any w16 writes: dropped
    }
    sys_close(fd);
    int status = 0;
    sys_waitpid(g_run.pid, &status);
    pthread_mutex_lock(&g_run.lock);
    g_run.finished = 1;
    if (status != 0) g_run.ok = 0;
    pthread_mutex_unlock(&g_run.lock);
    return 0;
}

static struct uui_checkbox g_keep;

static void start_install(void) {
    pthread_mutex_lock(&g_run.lock);
    g_run.running = 1;
    g_run.finished = g_run.ok = g_run.stopped = g_run.done = g_run.nlines = g_run.kept = 0;
    g_run.total = g_found.files;
    g_run.error[0] = g_run.current[0] = 0;
    pthread_mutex_unlock(&g_run.lock);
    g_t0 = sys_monotonic_ns();
    int fds[2];
    char *argv[] = { "w16", "--install", g_checked_src, g_keep.checked ? 0 : "--replace-ini", 0 };
    if (sys_pipe(fds) < 0) goto fail;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.stdout_fd = fds[1];
    o.stderr_fd = fds[1];
    g_run.pid = sys_spawn_opts(W16_BIN, &o);
    sys_close(fds[1]);
    if (g_run.pid < 0) { sys_close(fds[0]); goto fail; }
    ulogf("wininstall: installing from %s (pid %d)%s\n", g_checked_src, g_run.pid,
          g_keep.checked ? "" : ", INI files made afresh");
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, reader, (void *)(intptr_t)fds[0]) == 0) return;
    sys_kill(g_run.pid, SIGTERM);
    sys_close(fds[0]);
fail:
    pthread_mutex_lock(&g_run.lock);
    g_run.finished = 1;
    snprintf(g_run.error, sizeof g_run.error, "w16 could not be started.");
    pthread_mutex_unlock(&g_run.lock);
}

// Stop: the child goes, and the one file it was writing goes with it --
// the rest are whole, written under .part and renamed.
static void stop_install(void) {
    pthread_mutex_lock(&g_run.lock);
    int pid = g_run.running && !g_run.finished ? g_run.pid : -1;
    char cur[96];
    snprintf(cur, sizeof cur, "%s", g_run.current);
    g_run.stopped = pid > 0;
    pthread_mutex_unlock(&g_run.lock);
    if (pid <= 0) return;
    sys_kill(pid, SIGTERM);
    // C:\WINDOWS\SYSTEM\MAIN.CPL -> <drive C:>/WINDOWS/SYSTEM/MAIN.CPL.part
    size_t tl = strlen(g_found.target);
    if (g_found.target_path[0] && !strncmp(cur, g_found.target, tl)) {
        char part[512];
        snprintf(part, sizeof part, "%s%s.part", g_found.target_path, cur + tl);
        for (char *c = part + strlen(g_found.target_path); *c; c++) if (*c == '\\') *c = '/';
        unlink(part);
    }
    ulogf("wininstall: stopped at %s\n", cur);
}

// Windows' own Setup made Program Manager groups; here the group is Retro
// Launcher's: C:\WINDOWS joins its folders as "Windows", unless it is
// there already (by any name -- the user may have renamed it).
static void add_windows_folder(void) {
    if (!g_found.target_path[0]) return;
    struct w16_folder *f = 0;
    int n = w16_folders_load(&f);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(f[i].path, g_found.target_path)) { w16_folders_free(f, n); return; }
    struct w16_folder *g = realloc(f, sizeof *f * (size_t)(n + 1));
    if (g) {
        f = g;
        f[n].path = strdup(g_found.target_path);
        f[n].name = strdup("Windows");
        if (f[n].path && f[n].name) {
            n++;
            ulogf("wininstall: Retro Launcher's folders gain %s: %s\n", g_found.target_path,
                  w16_folders_save(f, n) == 0 ? "saved" : "NOT saved");
        } else {
            free(f[n].path);
            free(f[n].name);
        }
    }
    w16_folders_free(f, n);
}

// --- the pages --------------------------------------------------------------------------

#define FACTS_MAX 6
struct facts { int n; const char *k[FACTS_MAX]; char v[FACTS_MAX][160]; };
static struct facts g_disk_facts, g_check_facts, g_done_facts;
static struct uui_custom g_disk_card, g_check_card, g_done_card, g_log;

static int fact_row_h(void) { return ugfx_char_h() * 2; }

static void draw_facts(struct ugfx_surface *s, const struct uui_custom *c) {
    const struct facts *f = c->state;
    if (!f->n) return;
    int r = ugfx_char_h() / 2, pad = ugfx_char_w() * 2;
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, r, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, r - 1, UTHEME_WHITE);
    int lw = ugfx_char_advance('n') * 14, rh = fact_row_h();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    for (int i = 0; i < f->n; i++) {
        int y = c->y + i * rh;
        if (i) ugfx_fill_rect(s, c->x + pad, y, c->w - 2 * pad, 1, UTHEME_WINDOW_BG);
        int ty = y + (rh - ugfx_char_h()) / 2;
        ugfx_draw_string_clipped(s, c->x + pad, ty, lw, f->k[i], dim, UTHEME_WHITE);
        ugfx_draw_string_clipped(s, c->x + pad + lw, ty, c->w - 2 * pad - lw, f->v[i], UTHEME_TEXT, UTHEME_WHITE);
    }
}

static void fact(struct facts *f, const char *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fact(struct facts *f, const char *k, const char *fmt, ...) {
    if (f->n >= FACTS_MAX) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->v[f->n], sizeof f->v[f->n], fmt, ap);
    va_end(ap);
    f->k[f->n++] = k;
}

// The last lines of the install, in the fixed face: where each file went.
static void draw_log(struct ugfx_surface *s, const struct uui_custom *c) {
    int r = ugfx_char_h() / 2, pad = ugfx_char_w();
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, r, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, r - 1, UTHEME_WHITE);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    int lh = ugfx_char_h() + 2;
    pthread_mutex_lock(&g_run.lock);
    for (int i = 0; i < g_run.nlines; i++)
        ugfx_draw_string_clipped(s, c->x + pad, c->y + pad / 2 + i * lh, c->w - 2 * pad, g_run.lines[i],
                                 UTHEME_TEXT, UTHEME_WHITE);
    pthread_mutex_unlock(&g_run.lock);
    ugfx_set_font(was);
}

static struct uui_label w_intro, w_disk_error, w_missing, w_install_line, w_done_title,
                        w_done_text;
static char g_missing_text[400], g_install_line[160], g_done_title_text[128], g_done_text_text[300];
static struct uui_textbox g_src;
static struct uui_button g_browse, g_stop, g_open_launcher;
static struct uui_progress g_bar;
static struct uui_filedialog g_chooser;     // ~20 KB: file scope
static struct uui_dialog g_ask;
static struct uui_wizard g_wz;
static struct uui_layout g_root;

static void say(const char *title, const char *line) {
    static const char *rows[1];
    rows[0] = line;
    static const struct uui_dialog_button ok[] = { { "OK", ASK_OK, UUI_DLG_PRIMARY } };
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(&g_ask, title, rows, 1, ok, 1, 0, ASK_OK);
    uapp_redraw(g_app);
}

static void card_fit(struct uui_custom *c, const struct facts *f) { c->h = fact_row_h() * (f->n ? f->n : 1); }

static void refresh_disks(void) {
    struct facts *f = &g_disk_facts;
    f->n = 0;
    w_disk_error.text = "";
    if (g_found.ok && !strcmp(g_checked_src, uui_textbox_text(&g_src))) {
        char size[24];
        human_size_iec(size, sizeof size, g_found.bytes);
        fact(f, "Found", "%s", g_found.title);
        fact(f, "Disks", "%d %s", g_found.disks, g_found.disks == 1 ? "folder or image" : "folders and images");
        fact(f, "Files", "%d, %s once expanded", g_found.files, size);
        fact(f, "Install to", "%s  --  %s", g_found.target, g_found.target_path);
    } else if (g_found.error[0] && !strcmp(g_checked_src, uui_textbox_text(&g_src))) {
        w_disk_error.text = g_found.error;
    }
    card_fit(&g_disk_card, f);
}

static void refresh_check(void) {
    struct facts *f = &g_check_facts;
    f->n = 0;
    fact(f, g_found.target, "%d files -- programs, Help, sounds", g_found.in_windows);
    fact(f, "...\\SYSTEM", "%d files -- DLLs, drivers, fonts", g_found.in_system);
    fact(f, "INI files", "%s", g_keep.checked ? "kept where drive C: has them" : "made afresh");
    card_fit(&g_check_card, f);
    g_missing_text[0] = 0;
    if (g_found.missing) {
        snprintf(g_missing_text, sizeof g_missing_text, "%d file%s SETUP.INF names %s on none of these disks:",
                 g_found.missing, g_found.missing == 1 ? "" : "s", g_found.missing == 1 ? "is" : "are");
        for (int k = 0; k < 4 && g_found.missing_names[k][0]; k++) {
            strlcat(g_missing_text, k ? ", " : " ", sizeof g_missing_text);
            strlcat(g_missing_text, g_found.missing_names[k], sizeof g_missing_text);
        }
        strlcat(g_missing_text, ". The install cannot start until every disk is there.", sizeof g_missing_text);
    }
    w_missing.text = g_missing_text;
    uui_wizard_hold(&g_wz, 0, g_found.missing > 0, 0);
}

static void refresh_done(void) {
    struct facts *f = &g_done_facts;
    f->n = 0;
    if (g_run.ok) {
        snprintf(g_done_title_text, sizeof g_done_title_text, "Installed: %s", g_found.title);
        snprintf(g_done_text_text, sizeof g_done_text_text,
                 "Its programs are in Retro Launcher under WINDOWS. Control Panel, Help and the fonts "
                 "find their files where Windows' own Setup put them.");
        fact(f, "Files", "%d%s", g_found.files, g_run.kept ? ", the INI files kept" : "");
        char took[32];
        snprintf(took, sizeof took, "%llu.%llu s", (unsigned long long)(g_took_ms / 1000),
                 (unsigned long long)(g_took_ms % 1000 / 100));
        fact(f, "Took", "%s", took);
    } else {
        snprintf(g_done_title_text, sizeof g_done_title_text, g_run.stopped ? "The install was stopped"
                                                                             : "The install did not finish");
        snprintf(g_done_text_text, sizeof g_done_text_text, "%s", g_run.stopped
                 ? "The files already copied are whole; the one being written was removed. Install again to finish."
                 : g_run.error[0] ? g_run.error : "w16 stopped without saying why.");
        fact(f, "Copied", "%d of %d files", g_run.done ? g_run.done - 1 : 0, g_run.total);
    }
    card_fit(&g_done_card, f);
    w_done_title.text = g_done_title_text;
    w_done_text.text = g_done_text_text;
}

// --- the wizard ---------------------------------------------------------------------------

static struct uui_focusable g_focusables[8];
static struct uui_focus g_focus;

static void focus_for(int step) {
    int n = 0;
    struct uui_focusable *f = g_focusables;
    if (step == S_DISKS) {
        f[n++] = (struct uui_focusable){ &g_src, &uui_textbox_ops };
        f[n++] = (struct uui_focusable){ &g_browse, &uui_button_ops };
    } else if (step == S_CHECK) {
        f[n++] = (struct uui_focusable){ &g_keep, &uui_checkbox_ops };
    } else if (step == S_INSTALL) {
        f[n++] = (struct uui_focusable){ &g_stop, &uui_button_ops };
    } else {
        f[n++] = (struct uui_focusable){ &g_open_launcher, &uui_button_ops };
    }
    const struct uui_widget_ops *ops[3];
    void *w[3];
    int k = uui_wizard_focusables(&g_wz, ops, w, 3);
    for (int j = 0; j < k && n < 8; j++) f[n++] = (struct uui_focusable){ w[j], ops[j] };
    uui_focus_init(&g_focus, g_focusables, n);
    uui_focus_set(&g_focus, 0);
}

static void relayout(void) {
    if (g_app) uui_layout_run(&g_root, 0, 0, uapp_width(g_app), uapp_height(g_app));
}

static void on_enter(int step) {
    ulogf("wininstall: step %d\n", step);
    if (step == S_DISKS) refresh_disks();
    if (step == S_CHECK) refresh_check();
    if (step == S_DONE) refresh_done();
    relayout();
    focus_for(step);
    if (g_app) uapp_redraw(g_app);
}

static int can_leave(int step) {
    if (step == S_DISKS) {
        if (strcmp(g_checked_src, uui_textbox_text(&g_src)) || !g_found.ok) check_disks(uui_textbox_text(&g_src));
        refresh_disks();
        relayout();
        if (!g_found.ok) { say("These disks can't be used", g_found.error); return 0; }
    }
    if (step == S_CHECK) {
        if (g_found.missing) return 0;
        uui_wizard_hold(&g_wz, 1, 1, 1);
        start_install();
    }
    return 1;
}

static void quit(void) {
    if (g_run.running && !g_run.finished) return;
    uapp_quit(g_app, 0);
}

static void on_finish(void) { quit(); }

static void chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return;
    uui_textbox_set_text(&g_src, path);
    check_disks(path);
    refresh_disks();
    relayout();
    uapp_redraw(g_app);
}

// From the tick: the install's lines into the page, and its end into Done -- ONCE.
static int poll_install(void) {
    pthread_mutex_lock(&g_run.lock);
    int running = g_run.running, finished = g_run.finished, done = g_run.done, total = g_run.total;
    if (running) {
        if (g_run.current[0])
            snprintf(g_install_line, sizeof g_install_line, "Disk %d  --  %s  ->  %s", g_run.current_disk_no,
                     g_run.current_disk, g_run.current);
        else
            snprintf(g_install_line, sizeof g_install_line, "Reading the disks");
    }
    pthread_mutex_unlock(&g_run.lock);
    if (!running) return 0;
    uui_progress_set(&g_bar, total ? done * 1000 / total : 0);
    if (!finished) return 1;
    g_run.running = 0;
    g_took_ms = (sys_monotonic_ns() - g_t0) / 1000000;
    ulogf("wininstall: %s in %llu ms%s%s\n", g_run.ok ? "done" : g_run.stopped ? "STOPPED" : "FAILED",
          (unsigned long long)g_took_ms, g_run.error[0] ? " -- " : "", g_run.error);
    if (g_run.ok) {
        uui_progress_set(&g_bar, 1000);
        add_windows_folder();
    }
    uui_wizard_hold(&g_wz, 0, 0, 0);
    uui_wizard_go(&g_wz, S_DONE);
    uui_wizard_hold(&g_wz, g_run.ok, 0, g_run.ok);   // a failed one may go Back and try again
    return 1;
}

// --- uapp ---------------------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_SRC) refresh_disks();     // the card describes the folder checked, not one typed since
    else if (id == ID_KEEP) refresh_check();
    else if (id == ID_DIALOG) uui_dialog_take_code(&g_ask);
    relayout();
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    switch (code) {
    case ID_BROWSE: {
        if (uui_filedialog_is_open(&g_chooser)) break;
        struct uui_filedialog_opts o = { .mode = UUI_FILEDIALOG_FOLDER,
                                         .title = "Choose the folder the Windows disks are in",
                                         .start_dir = "/home" };
        uui_filedialog_open(a, &g_chooser, &o, chosen, 0);
        break;
    }
    case ID_STOP: stop_install(); break;
    case ID_LAUNCHER: uapp_spawn(a, LAUNCHER_BIN, 0); quit(); break;
    default: uui_wizard_action(&g_wz, code); break;
    }
    uapp_redraw(a);
}

// Closing mid-install asks to Stop first: a stop is what removes the half-written file.
static int on_close(struct uapp *a) {
    if (!g_run.running || g_run.finished) return 1;
    if (!uapp_question_open(a)) say("Windows is still being installed", "Stop it first; that leaves no half-written file.");
    return 0;
}

static void on_draw(struct uapp *a, struct uapp_draw *dr) {
    (void)dr;
    uapp_log_layout(a, "wininstall");
    uapp_logf_layout("wininstall: layout state %d %d %d %d\n", uui_wizard_step(&g_wz), g_run.done,
                     g_run.finished, g_run.ok);
}

static int on_tick(struct uapp *a) {
    (void)a;
    return poll_install();
}

static struct uui_item P_SRC[2], P_DISKS[4], P_CHECK[3], P_INSTALL[3], P_DONE_BTNS[1], P_DONE[4];
static struct uui_layout L_SRC, L_DISKS, L_CHECK, L_INSTALL, L_DONE_BTNS, L_DONE;
static struct uui_wizard_step g_steps[S_COUNT];
static struct uui_item g_widgets[2];

static void para(struct uui_label *l, const char *text, int dim) {
    uui_label_init(l, text);
    if (dim) l->dim = 1;
    uui_label_set_wrap(l, 3);
}

static struct uui_item item(const struct uui_widget_ops *ops, void *w, int id, const char *name, unsigned flags) {
    return (struct uui_item){ .ops = ops, .widget = w, .id = id, .name = name, .flags = flags };
}

static struct uui_layout column(struct uui_item *items, int count) {
    return (struct uui_layout){ .dir = UUI_COLUMN, .items = items, .count = count, .gap = ugfx_char_h() / 2 };
}

static void build(const char *start) {
    int cw = ugfx_char_advance('n'), lh = ugfx_char_h();

    para(&w_intro, "Windows 3.x's own setup disks: a folder holding their files, a folder per disk, or the "
                   "floppy images. They are only read.", 0);
    uui_textbox_init(&g_src, start ? start : "");
    uui_textbox_set_max(&g_src, (int)sizeof g_checked_src - 1);
    uui_button_init(&g_browse, 0, 0, 0, 0, "Browse...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_BROWSE);
    g_browse.outlined = 1;
    P_SRC[0] = item(&uui_textbox_ops, &g_src, ID_SRC, "src", UUI_FILL_W);
    P_SRC[1] = item(&uui_button_ops, &g_browse, ID_BROWSE, "browse", 0);
    L_SRC = (struct uui_layout){ .dir = UUI_ROW, .items = P_SRC, .count = 2, .gap = cw };
    g_disk_card = (struct uui_custom){ .w = cw * 70, .h = fact_row_h(), .draw = draw_facts, .state = &g_disk_facts };
    para(&w_disk_error, "", 0);
    P_DISKS[0] = item(&uui_label_ops, &w_intro, 0, "intro", UUI_FILL_W);
    P_DISKS[1] = item(&uui_layout_ops, &L_SRC, 0, 0, UUI_FILL_W);
    P_DISKS[2] = item(&uui_custom_ops, &g_disk_card, 0, "found", UUI_FILL_W);
    P_DISKS[3] = item(&uui_label_ops, &w_disk_error, 0, "disk_error", UUI_FILL_W);
    L_DISKS = column(P_DISKS, 4);

    g_check_card = (struct uui_custom){ .w = cw * 70, .h = fact_row_h(), .draw = draw_facts, .state = &g_check_facts };
    uui_checkbox_init(&g_keep, 0, 0, 0, "Keep the INI files drive C: already has",
                      UUI_ROLE(window_bg), UUI_ROLE(text));
    g_keep.accent = 1;
    g_keep.checked = 1;
    para(&w_missing, "", 0);
    P_CHECK[0] = item(&uui_custom_ops, &g_check_card, 0, "plan", UUI_FILL_W);
    P_CHECK[1] = item(&uui_checkbox_ops, &g_keep, ID_KEEP, "keep_ini", 0);
    P_CHECK[2] = item(&uui_label_ops, &w_missing, 0, "missing", UUI_FILL_W);
    L_CHECK = column(P_CHECK, 3);

    uui_label_init(&w_install_line, g_install_line);
    uui_progress_init(&g_bar);
    g_log = (struct uui_custom){ .w = cw * 70, .h = (lh + 2) * LOG_LINES + lh / 2, .draw = draw_log };
    uui_button_init(&g_stop, 0, 0, 0, 0, "Stop", UUI_ROLE(control_bg), UUI_ROLE(text), ID_STOP);
    g_stop.outlined = 1;
    P_INSTALL[0] = item(&uui_label_ops, &w_install_line, 0, "install_line", UUI_FILL_W);
    P_INSTALL[1] = item(&uui_progress_ops, &g_bar, 0, "bar", UUI_FILL_W);
    P_INSTALL[2] = item(&uui_custom_ops, &g_log, 0, "log", UUI_FILL_W);
    L_INSTALL = column(P_INSTALL, 3);
    static struct uui_item P_INSTALL_ALL[2];
    static struct uui_layout L_INSTALL_ALL;
    P_INSTALL_ALL[0] = item(&uui_layout_ops, &L_INSTALL, 0, 0, UUI_FILL_W);
    P_INSTALL_ALL[1] = item(&uui_button_ops, &g_stop, ID_STOP, "stop", 0);
    L_INSTALL_ALL = column(P_INSTALL_ALL, 2);

    uui_label_init(&w_done_title, g_done_title_text);
    w_done_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    uui_label_set_wrap(&w_done_title, 2);
    para(&w_done_text, g_done_text_text, 0);
    g_done_card = (struct uui_custom){ .w = cw * 70, .h = fact_row_h(), .draw = draw_facts, .state = &g_done_facts };
    uui_button_init(&g_open_launcher, 0, 0, 0, 0, "Open Retro Launcher", UUI_ROLE(control_bg), UUI_ROLE(text),
                    ID_LAUNCHER);
    g_open_launcher.outlined = 1;
    P_DONE_BTNS[0] = item(&uui_button_ops, &g_open_launcher, ID_LAUNCHER, "open_launcher", 0);
    L_DONE_BTNS = (struct uui_layout){ .dir = UUI_ROW, .items = P_DONE_BTNS, .count = 1, .gap = cw };
    P_DONE[0] = item(&uui_label_ops, &w_done_title, 0, "done_title", UUI_FILL_W);
    P_DONE[1] = item(&uui_label_ops, &w_done_text, 0, "done_text", UUI_FILL_W);
    P_DONE[2] = item(&uui_custom_ops, &g_done_card, 0, "done_facts", UUI_FILL_W);
    P_DONE[3] = item(&uui_layout_ops, &L_DONE_BTNS, 0, 0, 0);
    L_DONE = column(P_DONE, 4);

    static const struct { const char *title, *note, *next; struct uui_layout *page; } STEPS[S_COUNT] = {
        { "Disks", "Where are the Windows disks?", 0, &L_DISKS },
        { "Check", "What goes where. Nothing has been changed yet.", "Install", &L_CHECK },
        { "Install", "Expanding each file into drive C:.", 0, &L_INSTALL_ALL },
        { "Done", 0, "Close", &L_DONE },
    };
    for (int i = 0; i < S_COUNT; i++)
        g_steps[i] = (struct uui_wizard_step){ STEPS[i].title, STEPS[i].note,
                                               item(&uui_layout_ops, STEPS[i].page, 0, 0, 0), STEPS[i].next };
    uui_dialog_init(&g_ask);
    struct uui_wizard_desc d = {
        .heading = "Install Windows 3.x",
        .steps = g_steps,
        .count = S_COUNT,
        .skip = "Cancel",
        .finish = "Close",
        .can_leave = can_leave,
        .on_enter = on_enter,
        .on_finish = on_finish,
        .on_skip = quit,
    };
    uui_wizard_init(&g_wz, &d);
    g_widgets[0] = *uui_wizard_item(&g_wz);
    g_widgets[1] = item(&uui_dialog_ops, &g_ask, ID_DIALOG, "dialog", 0);
}

static const char *g_start;   // a folder to start from: `wininstall DISKS`

static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 100;
    *h = ugfx_char_h() * 32;
}

// BUILT HERE: the bold and display faces and every width come from the session font.
static void on_open(struct uapp *a) {
    g_app = a;
    build(g_start);
    g_root.count = 1;
    uapp_set_widgets(a, g_widgets, 2);
    relayout();
    if (g_start) { check_disks(g_start); refresh_disks(); relayout(); }
    on_enter(uui_wizard_step(&g_wz));
}

int main(int argc, char **argv) {
    if (argc > 1) g_start = argv[1];
    g_root = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_widgets, .count = 0, .margin = 1 };
    struct uapp_desc desc = {
        .title     = "Install Windows 3.x",
        .app_id    = "wininstall",
        .layout    = &g_root,
        .on_size   = on_size,
        .focus     = &g_focus,
        .flags     = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .tick_ms   = 100,
        .on_open   = on_open,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_close  = on_close,
        .on_tick   = on_tick,
        .on_draw   = on_draw,
    };
    return uapp_run(&desc);
}
