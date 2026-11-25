// The login and lock screen -- see wm_login.h. Drawn as the Leave page
// is (leave_page.c): one blurred, dimmed snapshot of the WALLPAPER (never
// the windows -- a lock screen shows nothing of the session), then the
// controls on it, the whole screen repainted while it is up.
//
// THE PASSWORD IS CHECKED OFF THE FRAME: privd answers a wrong one two
// seconds later, so the call runs on its own thread and the screen goes
// BUSY until wm_login_poll() sees the answer.
#include "wm_login.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_log.h"
#include "desktop.h"
#include "layout_popup.h"
#include "confirm_dialog.h"
#include "base/udate.h"
#include "base/uinitctl.h"
#include "base/upriv.h"
#include "base/usetting.h"
#include "ui/uui_caret.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int wm_login_open;

static int g_mode;
static char *g_account, *g_real;      // owned; the real name is GECOS's first field
static struct uui_textbox g_pw;
static char g_err[96];
static int g_busy, g_signed_in;

// The worker: one password check at a time.
static pthread_t g_thr;
static volatile int g_done;
static int g_rc, g_errno;
static char *g_try;

static uint32_t *g_snap;
static int g_snap_w, g_snap_h, g_snap_ok;
static uint32_t g_snap_gen;
static int g_minute = -1;

enum { C_FIELD, C_GO, C_RESTART, C_SHUTDOWN, CTLS };
static const char *const CTL_NAME[CTLS] = { "field", "go", "restart", "shutdown" };
struct rect { int x, y, w, h; };
static struct rect g_r[CTLS];
static int g_hot = -1, g_pressed = -1;

#define WHITE ugfx_rgb(255, 255, 255)
#define SOFT  ugfx_rgb(223, 230, 238)

static int u(void) { return ugfx_px(8); }

static char *dup_or_empty(const char *s) {
    char *d = strdup(s ? s : "");
    return d ? d : (char *)"";
}

// The account the screen is for: in the greeter, the first PERSON's in
// /etc/passwd (users D3 lists them all); when locking, our own.
static void pick_account(void) {
    struct passwd *pw = 0;
    if (g_mode == WM_LOGIN_LOCK) {
        pw = getpwuid(getuid());
    } else {
        setpwent();
        while ((pw = getpwent()) && pw->pw_uid < 1000) { }
        endpwent();
    }
    free(g_account);
    free(g_real);
    g_account = dup_or_empty(pw ? pw->pw_name : "");
    g_real = dup_or_empty(pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos : g_account);
    char *comma = strchr(g_real, ',');
    if (comma) *comma = '\0';
}

void wm_login_show(int mode) {
    g_mode = mode;
    pick_account();
    uui_textbox_init(&g_pw, "");
    g_pw.masked = 1;
    g_pw.ed.flags |= UUI_EDIT_MASKED;
    uui_textbox_set_active(&g_pw, 1);
    uui_caret_reset();
    g_err[0] = '\0';
    g_busy = g_signed_in = 0;
    g_snap_ok = 0;
    wm_overlay_close_others(0);
    wm_login_open = 1;
    wm_logf("login: the %s screen for %s\n", mode == WM_LOGIN_LOCK ? "lock" : "login", g_account);
    WM_REDRAW();
}

// --- the check ------------------------------------------------------------

static void *check(void *arg) {
    (void)arg;
    g_rc = g_mode == WM_LOGIN_LOCK ? upriv_auth(g_account, g_try) : upriv_login(g_account, g_try);
    g_errno = errno;
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void submit(void) {
    if (g_busy || g_signed_in || !g_account[0]) return;
    g_try = strdup(uui_textbox_text(&g_pw));
    if (!g_try) return;
    g_done = 0;
    g_busy = 1;
    g_err[0] = '\0';
    g_pw.ed.flags |= UUI_EDIT_READONLY;
    if (pthread_create(&g_thr, 0, check, 0) != 0) {
        g_busy = 0;
        g_pw.ed.flags &= ~UUI_EDIT_READONLY;
        snprintf(g_err, sizeof g_err, "Could not check the password.");
        free(g_try);
        g_try = 0;
    }
    WM_REDRAW();
}

static void answered(void) {
    pthread_join(g_thr, 0);
    memset(g_try, 0, strlen(g_try));
    free(g_try);
    g_try = 0;
    g_busy = 0;
    g_pw.ed.flags &= ~UUI_EDIT_READONLY;
    if (g_rc == 0) {
        uui_textbox_set_text(&g_pw, "");
        if (g_mode == WM_LOGIN_LOCK) {
            wm_logf("login: unlocked\n");
            wm_login_open = 0;
        } else {
            // init stops this compositor and starts the session's.
            wm_logf("login: %s signed in\n", g_account);
            g_signed_in = 1;
        }
        WM_REDRAW();
        return;
    }
    snprintf(g_err, sizeof g_err, "%s",
             g_errno == EACCES ? "Wrong password. Try again."
             : g_errno == EAGAIN ? "Wait a moment, then try again."
             : g_errno == EBUSY ? "Someone is already signed in."
             : g_errno == ENOENT ? "The sign-in service is not running."
             : "Could not sign in.");
    wm_logf("login: refused (%s)\n", g_err);
    uui_textbox_set_text(&g_pw, "");
    uui_caret_reset();
    WM_REDRAW();
}

void wm_login_poll(void) {
    if (!wm_login_open) return;
    if (g_busy && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) answered();
    time_t t = time(0);
    struct tm tm;
    if (localtime_r(&t, &tm) && tm.tm_min != g_minute) {
        g_minute = tm.tm_min;
        WM_REDRAW();
    }
    if (g_snap_ok && desktop_background_gen() != g_snap_gen) {   // the wallpaper arrived
        g_snap_ok = 0;
        WM_REDRAW();
    }
}

// --- drawing ---------------------------------------------------------------

static void snapshot(int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    if (!g_snap || g_snap_w != w || g_snap_h != h) {
        free(g_snap);
        g_snap = malloc(n * 4);
        g_snap_w = g_snap ? w : 0;
        g_snap_h = g_snap ? h : 0;
    }
    if (!g_snap) return;
    struct ugfx_surface bg;
    memset(&bg, 0, sizeof bg);
    bg.pixels = malloc(n * 4);
    bg.w = w;
    bg.h = h;
    for (size_t i = 0; i < n; i++) g_snap[i] = ugfx_rgb(20, 30, 42);
    if (bg.pixels) {
        g_snap_gen = desktop_background_gen();
        desktop_draw_background_into(&bg);
        ugfx_blur_rect(&bg, 0, 0, w, h, u(), g_snap);
        free(bg.pixels);
    }
    // Darkened toward the mockup's slate, so white text reads on any picture.
    for (size_t i = 0; i < n; i++) {
        uint32_t p = g_snap[i];
        uint32_t r = ((p >> 16) & 0xFF) * 45 / 100 + 10, g = ((p >> 8) & 0xFF) * 45 / 100 + 16,
                 b = (p & 0xFF) * 45 / 100 + 24;
        g_snap[i] = r << 16 | g << 8 | b;
    }
    g_snap_ok = 1;
}

static void text_c(struct ugfx_surface *s, int cx, int y, const char *t, uint32_t c) {
    int w = ugfx_text_width(t), max = screen_w - 2 * u();
    if (w > max) w = max;
    ugfx_draw_string_clipped(s, cx - w / 2, y, w + 1, t, c, UGFX_TRANSPARENT);
}

// The picture: a light disc with a head and shoulders in the accent.
static void picture(struct ugfx_surface *s, int cx, int cy, int r) {
    ugfx_fill_circle(s, cx, cy, r + ugfx_px(3), WHITE);
    ugfx_fill_circle(s, cx, cy, r, ugfx_rgb(220, 220, 226));
    uint32_t a = UTHEME_ACCENT;
    ugfx_fill_circle(s, cx, cy - r / 4, r * 3 / 10, a);
    ugfx_fill_circle(s, cx, cy - r / 4, r * 3 / 10 - ugfx_px(3), ugfx_rgb(220, 220, 226));
    // The shoulders, clipped at the disc's middle -- NESTED in the frame's
    // own damage clip, which is put back after rather than cleared.
    int sr = r * 6 / 10, sy = cy + r * 6 / 10;
    struct ugfx_clip was;
    ugfx_clip_save(s, &was);
    ugfx_clip_intersect(s, cx - r, cy, 2 * r, r * 6 / 10 + 1);
    ugfx_fill_circle(s, cx, sy, sr, a);
    ugfx_fill_circle(s, cx, sy, sr - ugfx_px(3), ugfx_rgb(220, 220, 226));
    ugfx_clip_restore(s, &was);
}

static void layout(void) {
    int ch = ugfx_char_h();
    int cx = screen_w / 2;
    int pic_r = ch * 3;
    int top = screen_h * 36 / 100;
    int field_w = ch * 15, field_h = ch + u() + ugfx_px(2);
    int fy = top + pic_r * 2 + ch * 2 + u() * 2;
    g_r[C_FIELD] = (struct rect){ cx - (field_w + field_h + u() / 2) / 2, fy, field_w, field_h };
    g_r[C_GO] = (struct rect){ g_r[C_FIELD].x + field_w + u() / 2, fy, field_h, field_h };
    uui_textbox_set_geometry(&g_pw, g_r[C_FIELD].x, g_r[C_FIELD].y, g_r[C_FIELD].w, g_r[C_FIELD].h);
    int bw = ugfx_text_width("Shut down") + u() * 2, bh = ch * 3;
    int by = screen_h - bh - u() * 2;
    g_r[C_SHUTDOWN] = (struct rect){ screen_w - bw - u() * 3, by, bw, bh };
    g_r[C_RESTART] = (struct rect){ g_r[C_SHUTDOWN].x - bw - u(), by, bw, bh };
}

// Restart's arrow and Shut down's power sign, from circles and bars.
static void power_glyph(struct ugfx_surface *s, int restart, int cx, int cy, int r, uint32_t c, uint32_t bg) {
    int t = ugfx_px(2);
    ugfx_fill_circle(s, cx, cy, r, c);
    ugfx_fill_circle(s, cx, cy, r - t, bg);
    if (restart) {
        ugfx_fill_rect(s, cx + r / 3, cy - r - t, r, r, bg);
        ugfx_fill_rect(s, cx + r / 3, cy - r - t, r * 2 / 3, t, c);
        ugfx_fill_rect(s, cx + r / 3 + r * 2 / 3 - t, cy - r - t, t, r * 2 / 3, c);
    } else {
        ugfx_fill_rect(s, cx - r / 3, cy - r - t, r * 2 / 3, r, bg);
        ugfx_fill_rect(s, cx - t / 2, cy - r - t, t, r, c);
    }
}

void wm_login_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!wm_login_open) return;
    struct ugfx_surface *s = wm_surface();
    if (!g_snap_ok || g_snap_w != s->w || g_snap_h != s->h) snapshot(s->w, s->h);
    if (g_snap_ok) ugfx_blit(s, 0, 0, g_snap_w, g_snap_h, g_snap, g_snap_w);
    else ugfx_fill_rect(s, 0, 0, s->w, s->h, ugfx_rgb(20, 30, 42));
    layout();
    int ch = ugfx_char_h(), cx = screen_w / 2;

    // The clock and the date, at the top.
    time_t t = time(0);
    struct tm tm;
    char clock[32] = "", date[64] = "";
    if (localtime_r(&t, &tm)) {
        udate_format_tm(clock, sizeof clock, &tm, UDATE_TIME);
        udate_format_tm(date, sizeof date, &tm, UDATE_DATE | UDATE_LONG);
    }
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_hero());
    int clock_y = screen_h * 9 / 100;
    text_c(s, cx, clock_y, clock, WHITE);
    int hero_h = ugfx_char_h();
    ugfx_set_font(was);
    text_c(s, cx, clock_y + hero_h + u() / 2, date, SOFT);

    // The account.
    int pic_r = ch * 3, top = screen_h * 36 / 100;
    picture(s, cx, top + pic_r, pic_r);
    was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    text_c(s, cx, top + pic_r * 2 + u() * 2, g_real ? g_real : "", WHITE);
    ugfx_set_font(was);

    // The password and its arrow.
    struct rect *f = &g_r[C_FIELD], *go = &g_r[C_GO];
    ugfx_fill_rect(s, f->x - ugfx_px(2), f->y - ugfx_px(2), f->w + ugfx_px(4), f->h + ugfx_px(4), UTHEME_ACCENT);
    uui_textbox_draw(s, &g_pw);
    if (!uui_textbox_text(&g_pw)[0] && !g_busy)
        ugfx_draw_string_clipped(s, f->x + u(), f->y + (f->h - ch) / 2, f->w - 2 * u(), "Password",
                                 ugfx_rgb(110, 114, 122), UGFX_TRANSPARENT);
    uint32_t gobg = g_busy ? ugfx_rgb(130, 150, 176) : g_hot == C_GO ? ugfx_rgb(56, 92, 138) : UTHEME_ACCENT;
    ugfx_fill_rect(s, go->x, go->y, go->w, go->h, gobg);
    int ax = go->x + go->w / 2, ay = go->y + go->h / 2, al = go->w / 4;
    ugfx_fill_rect(s, ax - al, ay - ugfx_px(1), al * 2, ugfx_px(2), WHITE);
    for (int i = 0; i < al / 2 + 1; i++) {
        ugfx_fill_rect(s, ax + al - i - ugfx_px(1), ay - i, ugfx_px(2), 1, WHITE);
        ugfx_fill_rect(s, ax + al - i - ugfx_px(1), ay + i, ugfx_px(2), 1, WHITE);
    }
    const char *line = g_signed_in ? "Signing in..." : g_busy ? "Checking..." : g_err;
    if (line[0]) text_c(s, cx, f->y + f->h + u(), line, g_err[0] && !g_busy ? ugfx_rgb(250, 214, 165) : SOFT);

    // Bottom left: the keyboard layout and the computer's name.
    char host[64] = "";
    if (!usetting_get("system.hostname", host, sizeof host)) host[0] = '\0';
    char kb[48] = "";
    const char *code = layout_count() > 0 ? layout_code(layout_active()) : 0;
    if (code && code[0]) snprintf(kb, sizeof kb, "Keyboard: %s", code);
    char bl[128];
    snprintf(bl, sizeof bl, "%s%s%s", kb, kb[0] && host[0] ? "    " : "", host);
    ugfx_draw_string_clipped(s, u() * 3, screen_h - ch - u() * 3, screen_w / 2, bl, SOFT, UGFX_TRANSPARENT);

    // Bottom right: Restart and Shut down.
    for (int k = C_RESTART; k <= C_SHUTDOWN; k++) {
        struct rect *r = &g_r[k];
        if (g_hot == k)
            for (int y = r->y; y < r->y + r->h; y++) ugfx_blend_hspan(s, r->x, y, r->w, WHITE, 0, 40);
        int gr = ch / 2 + ugfx_px(2);
        uint32_t under = s->pixels[(r->y + gr) * s->w + r->x + r->w / 2];
        power_glyph(s, k == C_RESTART, r->x + r->w / 2, r->y + gr + ugfx_px(4), gr, WHITE, under);
        text_c(s, r->x + r->w / 2, r->y + r->h - ch - ugfx_px(2), k == C_RESTART ? "Restart" : "Shut down", WHITE);
    }
}

// --- input -----------------------------------------------------------------

static int ctl_at(int x, int y) {
    for (int i = 0; i < CTLS; i++) {
        const struct rect *r = &g_r[i];
        if (r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

static void activate(int id) {
    switch (id) {
    case C_GO: submit(); break;
    case C_RESTART:
    case C_SHUTDOWN:
        wm_logf("login: %s\n", id == C_RESTART ? "restart" : "shut down");
        uinitctl_shutdown(id == C_RESTART);
        break;
    default: break;
    }
}

int wm_login_click(int mx, int my) {
    (void)mx; (void)my;
    return wm_login_open;   // modal: acted on at release (update_press)
}

void wm_login_update_press(int mx, int my, uint8_t buttons) {
    if (!wm_login_open) return;
    int at = ctl_at(mx, my);
    if (buttons & 1) {
        if (g_pressed < 0) g_pressed = at < 0 ? -2 : at;
        if (g_pressed == C_FIELD) uui_textbox_ops.press(&g_pw, mx, my, 0);
        return;
    }
    int was = g_pressed;
    g_pressed = -1;
    if (was >= 0 && was == at && was != C_FIELD) activate(was);
}

int wm_login_hover_at(int mx, int my) {
    if (!wm_login_open) return 0;
    int at = ctl_at(mx, my);
    g_hot = at == C_FIELD ? -1 : at;
    return g_hot + 1;
}

void wm_login_damage(void) { if (wm_login_open) WM_REDRAW(); }

int wm_login_key(int key, uint8_t mods) {
    if (!wm_login_open) return 0;
    if (key == '\n' || key == '\r') { submit(); return 1; }
    if (!g_busy && !g_signed_in && uui_textbox_key_mods(&g_pw, key, mods)) {
        g_err[0] = '\0';
        uui_caret_reset();
        WM_REDRAW();
    }
    return 1;   // everything: no shortcut reaches the session behind it
}

int wm_login_covers(void) { return wm_login_open; }

int wm_keys_private(void) { return wm_login_open || confirm_dialog_password() != 0; }

int wm_login_state(int *mode, const char **account, const char **err, int *busy, int *typed) {
    if (!wm_login_open) return 0;
    *mode = g_mode;
    *account = g_account ? g_account : "";
    *err = g_signed_in ? "signed in" : g_err;
    *busy = g_busy;
    *typed = (int)strlen(uui_textbox_text(&g_pw));
    return 1;
}

int wm_login_control(int i, const char **name, int *x, int *y, int *w, int *h) {
    if (!wm_login_open || i < 0 || i >= CTLS) return 0;
    *name = CTL_NAME[i];
    *x = g_r[i].x; *y = g_r[i].y; *w = g_r[i].w; *h = g_r[i].h;
    return 1;
}
