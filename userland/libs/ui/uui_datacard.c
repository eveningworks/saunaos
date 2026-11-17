// The game-data card -- see uui_datacard.h. Moved out of DOOM's front
// end (userland/apps/apps/doom/doom_front.c) when ScummVM's free games
// needed the same card; its mockups were DOOM's W1 and L2, and the
// "ScummVM games options" canvas's G2.
#include "ui/uui_datacard.h"
#include "base/uconf.h"
#include "base/ulog.h"
#include "input_keys.h"
#include "keyboard.h"
#include "rt/sys.h"
#include "ui/utheme.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_ctxmenu.h"
#include "ui/uui_primitives.h"
#include "ui/utextflow.h"
#include "win_proto.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { C_FILE, C_CANCEL, C_PRIMARY, C_STOP, C_BACK, C_AGREE, C_ROW0 };
static const char *const CTL_NAME[C_ROW0] = {
    [C_FILE] = "usefile", [C_CANCEL] = "cancel", [C_PRIMARY] = "primary",
    [C_STOP] = "stop", [C_BACK] = "back", [C_AGREE] = "agree",
};

#define ITEM(c, i) (&(c)->d->items[i])

static int u(void)     { return ugfx_char_h(); }
static int btn_h(void) { return utheme_control_h(); }
static int btn_w(const char *label) { return ugfx_text_width(label) + 2 * ugfx_char_w() + u(); }

static int in(const struct uui_datacard_rect *r, int x, int y) {
    return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// FROM THE CACHE: the card asks several times per item per frame and per
// pointer motion, and each real answer is a path and an access().
static int present(const struct uui_datacard *c, int i) { return c->here[i]; }
static void read_presence(struct uui_datacard *c) {
    for (int i = 0; i < c->d->count; i++) c->here[i] = (unsigned char)udata_present(c->d->dir, ITEM(c, i));
}

// What the card offers: everything that downloads, and anything else
// that is here (a retail file brought by hand).
static int shown(const struct uui_datacard *c, int i) {
    return ITEM(c, i)->part_count > 0 || present(c, i);
}

static void mib(char *out, int cap, unsigned long bytes) {
    unsigned long t = bytes * 10 / (1024 * 1024);
    snprintf(out, (size_t)cap, "%lu.%lu MiB", t / 10, t % 10);
}

// Several: what Download would fetch.
static int wanted(const struct uui_datacard *c, unsigned long *bytes) {
    int n = 0;
    if (bytes) *bytes = 0;
    for (int i = 0; i < c->d->count; i++)
        if (c->checked[i] && !present(c, i)) {
            n++;
            if (bytes) *bytes += udata_size(ITEM(c, i));
        }
    return n;
}

static const char *primary_label(const struct uui_datacard *c) {
    if (c->d->multi || c->sel < 0) return "Download";
    return present(c, c->sel) ? "Use" : "Download";
}

static int primary_enabled(const struct uui_datacard *c) {
    if (c->d->multi) return wanted(c, NULL) > 0;
    return c->sel >= 0 && shown(c, c->sel);
}

// The licence being shown.
static const struct udata_item *lic_item(const struct uui_datacard *c) {
    return ITEM(c, c->queue[c->lic_at]);
}

// The licence's lines at width `w`: its paragraph flowed as it is drawn.
static int licence_lines(const struct uui_datacard *c, int w) {
    int widest = 0, n = utextflow_measure(c->lic.text, c->lic.len, w, &widest);
    return n > 0 ? n : 1;
}

static int rows(const struct uui_datacard *c) {
    int n = 0;
    for (int i = 0; i < c->d->count; i++) n += shown(c, i);
    return n;
}

// One place that says where every control is, for drawing and for the
// pointer alike.
static void layout(struct uui_datacard *c) {
    int W = c->W, H = c->H, p = u();
    int nc = C_ROW0 + c->d->count;
    memset(c->r, 0, (size_t)nc * sizeof *c->r);
    memset(&c->card, 0, sizeof c->card);
    int cw = p * 36 < W - 2 * p ? p * 36 : W - 2 * p;
    if (c->view == UUI_DATACARD_ROWS) {
        int rh = 3 * u() + u() / 2 + 4, n = rows(c);
        int ch = p + u() * 2 + u() + p + n * (rh + 6) + (c->note[0] ? u() + 4 : 0) + p + btn_h() + p;
        c->card = (struct uui_datacard_rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int y = c->card.y + p + u() * 2 + u() + p / 2;
        for (int i = 0; i < c->d->count; i++) {
            if (!shown(c, i)) continue;
            c->r[C_ROW0 + i] = (struct uui_datacard_rect){ c->card.x + p, y, cw - 2 * p, rh };
            y += rh + 6;
        }
        int by = c->card.y + ch - p - btn_h();
        if (c->d->aux_label)
            c->r[C_FILE] = (struct uui_datacard_rect){ c->card.x + p, by, btn_w(c->d->aux_label), btn_h() };
        int pw = btn_w("Download");
        c->r[C_PRIMARY] = (struct uui_datacard_rect){ c->card.x + cw - p - pw, by, pw, btn_h() };
        if (c->back && c->d->on_cancel) {
            int kw = btn_w("Cancel");
            c->r[C_CANCEL] = (struct uui_datacard_rect){ c->r[C_PRIMARY].x - p / 2 - kw, by, kw, btn_h() };
        }
    } else if (c->view == UUI_DATACARD_LICENCE) {
        const struct udata_item *it = lic_item(c);
        int lines = licence_lines(c, cw - 4 * p);
        int ch = p + u() * 2 + u() + p / 2 + p + lines * u() + u() + p + p / 2 + p + btn_h() + p;
        c->card = (struct uui_datacard_rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int by = c->card.y + ch - p - btn_h();
        c->r[C_BACK] = (struct uui_datacard_rect){ c->card.x + p, by, btn_w("Back"), btn_h() };
        int aw = btn_w(it->agree ? "Agree and download" : "Download");
        c->r[C_AGREE] = (struct uui_datacard_rect){ c->card.x + cw - p - aw, by, aw, btn_h() };
    } else if (c->view == UUI_DATACARD_FETCH) {
        int ch = p + u() * 2 + u() + p + u() / 2 + p + u() + p + u() + p + btn_h() + p;
        c->card = (struct uui_datacard_rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int kw = btn_w("Cancel");
        c->r[C_STOP] = (struct uui_datacard_rect){ c->card.x + cw - p - kw, c->card.y + ch - p - btn_h(),
                                                   kw, btn_h() };
    }
}

// Every control where it is, for a test to press by asking.
static void log_layout(const struct uui_datacard *c) {
    static const char *const VIEW[] = { "off", "data", "licence", "fetch" };
    const char *L = c->d->log;
    uapp_logf_layout("%s: layout view %s\n", L, VIEW[c->view]);
    for (int i = 0; i < C_ROW0 + c->d->count; i++) {
        const struct uui_datacard_rect *r = &c->r[i];
        if (r->w <= 0) continue;
        if (i < C_ROW0)
            uapp_logf_layout("%s: layout %s %d %d %d %d\n", L, CTL_NAME[i], r->x, r->y, r->w, r->h);
        else
            uapp_logf_layout("%s: layout row%d %d %d %d %d %s\n", L, i - C_ROW0, r->x, r->y, r->w, r->h,
                             ITEM(c, i - C_ROW0)->id);
    }
    if (c->view == UUI_DATACARD_LICENCE)
        uapp_logf_layout("%s: layout licence %d %d %d %d\n", L, c->lic.x, c->lic.y, c->lic.w, c->lic.h);
    if (c->view != UUI_DATACARD_ROWS) return;
    uapp_logf_layout("%s: layout selected %d\n", L, c->sel);
    if (!c->d->multi) return;
    char line[160];
    int n = snprintf(line, sizeof line, "%s: layout checked", L);
    for (int i = 0; i < c->d->count && n < (int)sizeof line - 8; i++)
        if (c->checked[i]) n += snprintf(line + n, sizeof line - (size_t)n, " %d", i);
    uapp_logf_layout("%s\n", line);
}

// --- drawing ------------------------------------------------------------------

static void bold(const char *t, struct ugfx_surface *s, int x, int y, int w, uint32_t bg) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, w, t, UTHEME_TEXT, bg);
    ugfx_set_font(was);
}

static void dim(const char *t, struct ugfx_surface *s, int x, int y, int w, uint32_t bg) {
    ugfx_draw_string_elided(s, x, y, w, t, ugfx_blend(UTHEME_TEXT, bg, 150), bg);
}

static void frame(struct ugfx_surface *s, const struct uui_datacard_rect *r) {
    int rad = u() / 2;
    uui_fill_round_rect(s, r->x, r->y, r->w, r->h, rad, UTHEME_OUTLINE);
    uui_fill_round_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, rad - 1, UTHEME_WINDOW_BG);
}

static void button(struct uui_datacard *c, struct ugfx_surface *s, int id, const char *label, int primary,
                   int enabled) {
    const struct uui_datacard_rect *r = &c->r[id];
    enum uui_state st = !enabled ? UUI_STATE_DISABLED
                      : c->pressed == id && c->hot == id ? UUI_STATE_PRESSED
                      : c->hot == id ? UUI_STATE_HOVER : UUI_STATE_REST;
    uui_button_draw(s, r->x, r->y, r->w, r->h, label, primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG,
                    primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, st);
}

static void radio(struct ugfx_surface *s, int x, int y, int on, uint32_t bg) {
    int d = u() * 3 / 4 + 2;
    uui_fill_round_rect(s, x, y, d, d, UUI_CAPSULE, on ? UTHEME_ACCENT : UTHEME_OUTLINE);
    uui_fill_round_rect(s, x + 2, y + 2, d - 4, d - 4, UUI_CAPSULE, on ? bg : UTHEME_WHITE);
    if (on) uui_fill_round_rect(s, x + d / 4 + 1, y + d / 4 + 1, d - 2 * (d / 4) - 2,
                                d - 2 * (d / 4) - 2, UUI_CAPSULE, UTHEME_ACCENT);
}

static void check(struct ugfx_surface *s, int x, int y, int on, int disabled, uint32_t bg) {
    struct uui_checkbox cb;
    uui_checkbox_init(&cb, x, y, 0, NULL, UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    cb.accent = 1;
    cb.checked = on;
    cb.disabled = disabled;
    cb.bg = bg;
    uui_checkbox_draw(s, &cb);
}

static void draw_rows(struct uui_datacard *c, struct ugfx_surface *s) {
    const struct uui_datacard_rect *k = &c->card;
    int p = u(), x = k->x + p, w = k->w - 2 * p;
    uint32_t bg = UTHEME_WINDOW_BG;
    frame(s, k);
    int any = 0;
    for (int i = 0; i < c->d->count; i++) any |= present(c, i);
    bold(any ? c->d->title_some : c->d->title_empty, s, x, k->y + p, w, bg);
    dim(c->d->subtitle, s, x, k->y + p + u() + u() / 2, w, bg);
    int last_y = 0;
    for (int i = 0; i < c->d->count; i++) {
        const struct uui_datacard_rect *r = &c->r[C_ROW0 + i];
        if (r->w <= 0) continue;
        const struct udata_item *it = ITEM(c, i);
        int here = present(c, i), sel = i == c->sel;
        int on = c->d->multi ? c->checked[i] || here : sel;
        uint32_t rb = (c->d->multi ? on && !here : sel) ? UTHEME_SELECTION
                    : c->hot == C_ROW0 + i ? uui_state_bg(UTHEME_WHITE, UUI_STATE_HOVER) : UTHEME_WHITE;
        uui_fill_round_rect(s, r->x, r->y, r->w, r->h, p / 3, sel ? UTHEME_ACCENT : UTHEME_SEPARATOR);
        uui_fill_round_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, p / 3 - 1, rb);
        if (c->d->multi) check(s, r->x + p / 2, r->y + p / 2, on, here, rb);
        else radio(s, r->x + p / 2, r->y + p / 2, on, rb);
        int tx = r->x + p / 2 + u() + p / 2, tw = r->x + r->w - tx - p / 2;
        bold(it->name, s, tx, r->y + p / 3, tw, rb);
        dim(it->detail, s, tx, r->y + p / 3 + u() + 2, tw, rb);
        char line[96], sz[24];
        mib(sz, sizeof sz, udata_size(it));
        if (here) snprintf(line, sizeof line, "Installed");
        else snprintf(line, sizeof line, "From %s, %s", it->source, sz);
        dim(line, s, tx, r->y + p / 3 + 2 * u() + 4, tw, rb);
        last_y = r->y + r->h;
    }
    if (c->note[0])
        ugfx_draw_string_elided(s, x, last_y + 6, w, c->note, utheme_action(UTHEME_ACT_DANGER), bg);
    const struct uui_datacard_rect *pb = &c->r[C_PRIMARY];
    ugfx_fill_rect(s, k->x + 1, pb->y - p / 2, k->w - 2, 1, UTHEME_SEPARATOR);
    if (c->d->aux_label) button(c, s, C_FILE, c->d->aux_label, 0, 1);
    if (c->r[C_CANCEL].w) button(c, s, C_CANCEL, "Cancel", 0, 1);
    if (c->d->multi) {
        unsigned long bytes;
        int n = wanted(c, &bytes);
        char t[96], sz[24];
        mib(sz, sizeof sz, bytes);
        if (n) snprintf(t, sizeof t, "%d %s, %s", n, n == 1 ? c->d->noun : c->d->nouns, sz);
        else snprintf(t, sizeof t, "Nothing chosen");
        int left = c->r[C_CANCEL].w ? c->r[C_CANCEL].x : pb->x;
        int fx = c->d->aux_label ? c->r[C_FILE].x + c->r[C_FILE].w + p : x;
        int tw = ugfx_text_width(t), fw = left - p / 2 - fx;
        if (fw > 0) dim(t, s, tw < fw ? left - p / 2 - tw : fx, pb->y + (btn_h() - u()) / 2, fw, bg);
    }
    button(c, s, C_PRIMARY, primary_label(c), 1, primary_enabled(c));
}

static void draw_licence(struct uui_datacard *c, struct ugfx_surface *s) {
    const struct uui_datacard_rect *k = &c->card;
    const struct udata_item *it = lic_item(c);
    int p = u(), x = k->x + p, w = k->w - 2 * p, y = k->y + p;
    uint32_t bg = UTHEME_WINDOW_BG;
    frame(s, k);
    char t[128];
    snprintf(t, sizeof t, "%s: the licence", it->name);
    bold(t, s, x, y, w, bg);
    dim(it->licence, s, x, y + u() + u() / 2, w, bg);
    y += u() * 2 + u() + p / 2;
    int lines = licence_lines(c, w - 2 * p);
    int bh = lines * u() + u() + p + p / 2;   // the text, a half-line gap, "Saved in"
    uui_fill_round_rect(s, x, y, w, bh, p / 3, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, bh - 2, p / 3 - 1, UTHEME_WHITE);
    c->lic.bg = UTHEME_WHITE;
    uui_para_ops.set_geometry(&c->lic, x + p, y + p / 2, w - 2 * p, lines * u());
    uui_para_ops.draw(s, &c->lic);
    if (it->licence_file) {
        snprintf(t, sizeof t, "Saved in /usr/share/licenses/%s.", it->licence_file);
        dim(t, s, x + p, y + bh - u() - p / 2, w - 2 * p, UTHEME_WHITE);
    }
    ugfx_fill_rect(s, k->x + 1, c->r[C_BACK].y - p / 2, k->w - 2, 1, UTHEME_SEPARATOR);
    button(c, s, C_BACK, "Back", 0, 1);
    button(c, s, C_AGREE, it->agree ? "Agree and download" : "Download", 1, 1);
}

static void draw_fetch(struct uui_datacard *c, struct ugfx_surface *s) {
    const struct uui_datacard_rect *k = &c->card;
    const struct udata_item *it = ITEM(c, c->queue[c->queue_at]);
    int p = u(), x = k->x + p, w = k->w - 2 * p, y = k->y + p;
    uint32_t bg = UTHEME_WINDOW_BG;
    frame(s, k);
    char t[160], of[24] = "";
    if (c->queue_n > 1) snprintf(of, sizeof of, " (%d of %d)", c->queue_at + 1, c->queue_n);
    snprintf(t, sizeof t, c->phase ? "Unpacking %s%s" : "Downloading %s%s", it->name, of);
    bold(t, s, x, y, w, bg);
    y += u() + u() / 2;
    snprintf(t, sizeof t, "From %s to %s/%s", c->host, c->d->dir, it->id);
    dim(t, s, x, y, w, bg);
    y += u() + p;
    unsigned long got = c->got, total = c->total;
    if (!total && !c->phase && c->part < it->part_count) total = it->parts[c->part].size;
    int fill = total ? (int)((unsigned long long)w * (got > total ? total : got) / total) : 0;
    uui_fill_round_rect(s, x, y, w, u() / 2, UUI_CAPSULE, UTHEME_SEPARATOR);
    if (fill > 0) uui_fill_round_rect(s, x, y, fill < u() / 2 ? u() / 2 : fill, u() / 2, UUI_CAPSULE, UTHEME_ACCENT);
    y += u() / 2 + p / 2;
    char a1[24], a2[24];
    mib(a1, sizeof a1, got);
    mib(a2, sizeof a2, total);
    if (total) snprintf(t, sizeof t, "%s of %s", a1, a2);
    else snprintf(t, sizeof t, c->phase ? "Reading the archive..." : "Connecting...");
    dim(t, s, x, y, w, bg);
    y += u() + p;
    dim("Checked against its known SHA-256 before it is used.", s, x, y, w, bg);
    button(c, s, C_STOP, "Cancel", 0, 1);
}

void uui_datacard_draw(struct uui_datacard *c, struct ugfx_surface *s) {
    if (c->view == UUI_DATACARD_OFF) return;
    c->W = s->w;
    c->H = s->h;
    layout(c);
    ugfx_fill(s, ugfx_rgb(0, 0, 0));
    if (c->view == UUI_DATACARD_ROWS) draw_rows(c, s);
    else if (c->view == UUI_DATACARD_LICENCE) draw_licence(c, s);
    else draw_fetch(c, s);
    log_layout(c);
}

// --- the worker -----------------------------------------------------------------

static void post_progress(struct uui_datacard *c) {
    unsigned long long now = sys_monotonic_ns();
    if (now - c->last_post < 100000000ULL) return;   // ten a second is plenty
    c->last_post = now;
    uapp_post(c->app, UUI_DATACARD_EV_PROGRESS, 0);
}

static void fetch_progress(void *ctx, int phase, int part, unsigned long got, unsigned long total) {
    struct uui_datacard *c = ctx;
    c->phase = phase;
    c->part = part;
    c->got = got;
    c->total = total;
    post_progress(c);
}

static void *worker(void *arg) {
    struct uui_datacard *c = arg;
    const struct udata_item *it = ITEM(c, c->queue[c->queue_at]);
    c->rc = udata_fetch_run(&c->fetch, it);
    uapp_post(c->app, UUI_DATACARD_EV_DONE, 0);
    return 0;
}

static void start_one(struct uui_datacard *c) {
    const struct udata_item *it = ITEM(c, c->queue[c->queue_at]);
    udata_host(&it->parts[0], c->mirror, c->host, sizeof c->host);
    c->got = c->total = 0;
    c->phase = c->part = 0;
    c->fetch = (struct udata_fetch){
        .dir = c->d->dir, .mirror = c->mirror[0] ? c->mirror : NULL, .download = c->d->download,
        .progress = fetch_progress, .cancel = &c->cancel, .ctx = c,
    };
    c->view = UUI_DATACARD_FETCH;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, worker, c) != 0) {
        c->view = UUI_DATACARD_ROWS;
        snprintf(c->note, sizeof c->note, "Could not start the download.");
    }
    uapp_redraw(c->app);
}

// --- the views -----------------------------------------------------------------

static void show_rows(struct uui_datacard *c) {
    read_presence(c);
    uui_para_free(&c->lic);
    c->view = UUI_DATACARD_ROWS;
    c->hot = c->pressed = -1;
    ulogf("%s: game data card, %d choices\n", c->d->log, rows(c));
    uapp_redraw(c->app);
}

// The next licence still to show from `lic_at` on, or the downloads when
// none is left. A notice to read shows every time; one to AGREE to only
// until it has been (its text's CRC, kept in the app's conf).
static void next_licence(struct uui_datacard *c) {
    for (; c->lic_at < c->queue_n; c->lic_at++) {
        const struct udata_item *it = lic_item(c);
        if (!it->notice) continue;
        if (it->agree && udata_licence_agreed(c->d->conf, it)) {
            ulogf("%s: licence for %s accepted before\n", c->d->log, it->id);
            continue;
        }
        // A blank line between the notice's paragraphs, as it is drawn.
        size_t n = strlen(it->notice), extra = 0;
        for (size_t k = 0; k < n; k++) extra += it->notice[k] == '\n';
        char *text = malloc(n + extra + 1), *o = text;
        for (size_t k = 0; text && k < n; k++) {
            *o++ = it->notice[k];
            if (it->notice[k] == '\n') *o++ = '\n';
        }
        if (o) *o = 0;
        uui_para_free(&c->lic);
        uui_para_init(&c->lic, text ? text : it->notice);
        free(text);
        c->lic_drag = c->lic_menu = 0;
        c->view = UUI_DATACARD_LICENCE;
        c->enter_up = 0;
        c->hot = c->pressed = -1;
        ulogf("%s: licence for %s shown (%s)\n", c->d->log, it->id, it->agree ? "agree" : "notice");
        uapp_redraw(c->app);
        return;
    }
    c->queue_at = 0;
    c->cancel = 0;
    c->note[0] = 0;
    uconf_get(c->d->conf, "mirror", c->mirror, sizeof c->mirror);
    start_one(c);
}

static void download(struct uui_datacard *c) {
    c->queue_n = 0;
    if (c->d->multi) {
        for (int i = 0; i < c->d->count; i++)
            if (c->checked[i] && !present(c, i)) c->queue[c->queue_n++] = i;
    } else if (c->sel >= 0) {
        c->queue[c->queue_n++] = c->sel;
    }
    if (!c->queue_n) return;
    c->lic_at = 0;
    next_licence(c);
}

static void activate(struct uui_datacard *c, int id) {
    struct uapp *a = c->app;
    if (id != C_STOP) c->note[0] = 0;
    switch (id) {
    case C_FILE:
        if (c->d->on_aux) c->d->on_aux(a, c->d->ctx);
        return;
    case C_CANCEL:
        uui_datacard_close(c);
        if (c->d->on_cancel) c->d->on_cancel(a, c->d->ctx);
        return;
    case C_PRIMARY:
        if (!primary_enabled(c)) return;
        if (!c->d->multi && present(c, c->sel)) {
            const struct udata_item *it = ITEM(c, c->sel);
            uui_datacard_close(c);
            if (c->d->on_use) c->d->on_use(a, it, c->d->ctx);
            return;
        }
        download(c);
        return;
    case C_BACK:
        show_rows(c);
        return;
    case C_AGREE: {
        const struct udata_item *it = lic_item(c);
        if (it->agree) {
            udata_licence_agree(c->d->conf, it);
            ulogf("%s: licence for %s agreed\n", c->d->log, it->id);
        }
        c->lic_at++;
        next_licence(c);
        return;
    }
    case C_STOP:
        c->cancel = 1;
        return;
    default:
        if (id >= C_ROW0 && id < C_ROW0 + c->d->count) {
            int i = id - C_ROW0;
            c->sel = i;
            if (c->d->multi && !present(c, i)) c->checked[i] = !c->checked[i];
        }
        break;
    }
    uapp_redraw(a);
}

// --- the public face ------------------------------------------------------------

void uui_datacard_open(struct uui_datacard *c, struct uapp *a, const struct uui_datacard_desc *d,
                       const struct udata_item *select, int back) {
    if (c->d != d || !c->r) {
        free(c->r);
        free(c->checked);
        free(c->queue);
        free(c->here);
        c->r = calloc((size_t)(C_ROW0 + d->count), sizeof *c->r);
        c->checked = calloc((size_t)d->count, 1);
        c->queue = calloc((size_t)d->count, sizeof *c->queue);
        c->here = calloc((size_t)d->count, 1);
        uui_para_init(&c->lic, "");
    }
    c->d = d;
    read_presence(c);
    c->app = a;
    c->back = back;
    c->note[0] = 0;
    // The focus: the one asked for, else the first that downloads, is not
    // here and asks no agreement (the free one: Freedoom before id's
    // shareware), else the first that downloads and is not here.
    c->sel = -1;
    for (int i = 0; i < d->count; i++)
        if (&d->items[i] == select) c->sel = i;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; c->sel < 0 && i < d->count; i++)
            if (d->items[i].part_count > 0 && !present(c, i) && (pass || !d->items[i].agree)) c->sel = i;
    for (int i = 0; c->sel < 0 && i < d->count; i++)
        if (shown(c, i)) c->sel = i;
    if (d->multi && c->sel >= 0 && !present(c, c->sel)) c->checked[c->sel] = 1;
    show_rows(c);
}

void uui_datacard_close(struct uui_datacard *c) { c->view = UUI_DATACARD_OFF; }
int uui_datacard_up(const struct uui_datacard *c) { return c->view != UUI_DATACARD_OFF; }

void uui_datacard_note(struct uui_datacard *c, const char *text) {
    snprintf(c->note, sizeof c->note, "%s", text);
    if (c->app) uapp_redraw(c->app);
}

static int ctl_at(struct uui_datacard *c, int x, int y) {
    for (int i = 0; i < C_ROW0 + c->d->count; i++) if (in(&c->r[i], x, y)) return i;
    return -1;
}

static int on_licence(const struct uui_datacard *c, int x, int y) {
    return c->view == UUI_DATACARD_LICENCE && uui_para_ops.hit(&c->lic, x, y);
}

// The licence's right-click menu: the one every selectable text has.
enum { LIC_COPY = 1, LIC_ALL };
static const struct uui_menu_item LIC_MENU[] = {
    UUI_MENU("Copy", LIC_COPY, 0),
    UUI_MENU("Select All", LIC_ALL, 0),
};
static int lic_run(void *w, int code) {
    struct uui_datacard *c = w;
    uui_para_key(&c->lic, code == LIC_COPY ? 0x03 : 0x01, 0);
    uapp_redraw(c->app);
    return 0;
}

void uui_datacard_press(struct uui_datacard *c, int x, int y, unsigned buttons) {
    layout(c);
    if (on_licence(c, x, y)) {
        if (buttons & WIN_MOUSE_BTN_SECONDARY) c->lic_menu = 1;
        else if (buttons & WIN_MOUSE_BTN_PRIMARY) c->lic_drag = uui_para_ops.press(&c->lic, x, y, 0);
        uapp_redraw(c->app);
        return;
    }
    if (!(buttons & WIN_MOUSE_BTN_PRIMARY)) return;
    c->pressed = ctl_at(c, x, y);
    uapp_redraw(c->app);
}

void uui_datacard_motion(struct uui_datacard *c, int x, int y) {
    layout(c);
    if (c->lic_drag) {
        uui_para_ops.motion(&c->lic, x, y, WIN_MOUSE_BTN_PRIMARY);
        uapp_redraw(c->app);
        return;
    }
    uapp_set_cursor(c->app, on_licence(c, x, y) ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT);
    int h = ctl_at(c, x, y);
    if (h != c->hot) { c->hot = h; uapp_redraw(c->app); }
}

void uui_datacard_release(struct uui_datacard *c, int x, int y, unsigned buttons) {
    (void)buttons;
    layout(c);
    if (c->lic_drag) {
        uui_para_ops.release(&c->lic, x, y);
        c->lic_drag = 0;
        uapp_redraw(c->app);
        return;
    }
    if (c->lic_menu) {
        static struct uui_ctxmenu_src src = { 0, LIC_MENU, 2, 0, lic_run };
        src.w = c;
        c->lic_menu = 0;
        if (on_licence(c, x, y)) uapp_popup_menu(&src, 0, x, y);
        return;
    }
    int id = c->pressed;
    c->pressed = -1;
    if (id >= 0 && ctl_at(c, x, y) == id) activate(c, id);   // commit over the same control
    else uapp_redraw(c->app);
}

// The next row shown, from `i` on in direction `dir`.
static int step(const struct uui_datacard *c, int i, int dir) {
    for (int j = i + dir; j >= 0 && j < c->d->count; j += dir)
        if (shown(c, j)) return j;
    return i;
}

int uui_datacard_key(struct uui_datacard *c, int key, unsigned mods) {
    int enter = key == '\n' || key == '\r';
    if (c->view == UUI_DATACARD_ROWS) {
        if (key == KEY_ARROW_UP || key == KEY_ARROW_DOWN) {
            c->sel = step(c, c->sel, key == KEY_ARROW_UP ? -1 : 1);
            uapp_redraw(c->app);
            return 1;
        }
        if (key == ' ' && c->d->multi && c->sel >= 0) { activate(c, C_ROW0 + c->sel); return 1; }
        if (enter) { activate(c, C_PRIMARY); return 1; }
        if (key == 0x1B && c->back && c->d->on_cancel) { activate(c, C_CANCEL); return 1; }
    } else if (c->view == UUI_DATACARD_LICENCE) {
        if (uui_para_key(&c->lic, key, mods)) { uapp_redraw(c->app); return 1; }
        if (enter) {
            if (c->enter_up) activate(c, C_AGREE);
            return 1;
        }
        if (key == 0x1B) { activate(c, C_BACK); return 1; }
    } else if (c->view == UUI_DATACARD_FETCH) {
        if (key == 0x1B) { c->cancel = 1; return 1; }
    }
    return 0;
}

void uui_datacard_phys(struct uui_datacard *c, int keycode, int down) {
    if (!down && (keycode == INPUT_KEY_ENTER || keycode == INPUT_KEY_KPENTER)) c->enter_up = 1;
}

int uui_datacard_user(struct uui_datacard *c, int a0, int a1) {
    (void)a1;
    if (a0 == UUI_DATACARD_EV_PROGRESS) { uapp_redraw(c->app); return 1; }
    if (a0 != UUI_DATACARD_EV_DONE) return 0;
    const struct udata_item *it = ITEM(c, c->queue[c->queue_at]);
    if (c->rc != 0) {
        ulogf("%s: download of %s failed: %s\n", c->d->log, it->id, c->fetch.err);
        snprintf(c->note, sizeof c->note, "%s", c->cancel ? "Download cancelled." : c->fetch.err);
        show_rows(c);
        return 1;
    }
    ulogf("%s: downloaded %s, checksum matches\n", c->d->log, it->id);
    read_presence(c);
    if (c->d->multi) c->checked[c->queue[c->queue_at]] = 0;
    if (++c->queue_at < c->queue_n) {
        start_one(c);
        return 1;
    }
    uui_datacard_close(c);
    if (c->d->on_done) c->d->on_done(c->app, it, c->d->ctx);
    return 1;
}
