// The login and lock screen -- see wm_login.h. Drawn as the Leave page
// is (leave_page.c): one blurred, dimmed snapshot of the WALLPAPER (never
// the windows -- a lock screen shows nothing of the session), then the
// controls on it, the whole screen repainted while it is up.
//
// SEVERAL PEOPLE (users D3a, mockup LA -- Windows 11's shape): the one
// who signed in last is in the middle, the others a list bottom-left; a
// click on one puts them in the middle. Who was last is
// /var/lib/greeter/last, which privd writes at a sign-in (GDM's is
// AccountsService's).
//
// THE NETWORK, bottom right (mockup N2, Windows 11's sign-in shape): an
// icon button left of Restart, opening a read-only card -- no switch, as
// nobody is signed in. network_poll() keeps the state on the greeter too.
//
// THE PASSWORD IS CHECKED OFF THE FRAME: privd answers a wrong one two
// seconds later, so the call runs on its own thread and the screen goes
// BUSY until wm_login_poll() sees the answer.
#include "wm_login.h"
#include "wm_priv.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_log.h"
#include "desktop.h"
#include "layout_popup.h"
#include "network_popup.h"
#include "media/icon_cache.h"
#include "confirm_dialog.h"
#include "base/udate.h"
#include "base/uinitctl.h"
#include "base/upriv.h"
#include "base/usetting.h"
#include "base/uusers.h"
#include "ui/uui_caret.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"   // the capsule button
#include "ui/utheme.h"
#include <errno.h>
#include <stdio.h>
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

// C_NETCARD is the open card: hit-tested FIRST, so it lights and clicks
// nothing beneath it.
enum { C_FIELD, C_GO, C_RESTART, C_SHUTDOWN, C_KEYBOARD, C_NET, C_NETCARD, CTLS };
static const char *const CTL_NAME[CTLS] = { "field", "go", "restart", "shutdown", "keyboard",
                                            "network", "netcard" };
struct rect { int x, y, w, h; };
static struct rect g_r[CTLS];
static int g_hot = -1, g_pressed = -1;
static int g_net_open;

#define WHITE ugfx_rgb(255, 255, 255)
#define SOFT  ugfx_rgb(223, 230, 238)

static int u(void) { return ugfx_px(8); }

static char *dup_or_empty(const char *s) {
    char *d = strdup(s ? s : "");
    return d ? d : (char *)"";
}

#define LAST_FILE "/var/lib/greeter/last"

// The people the greeter lists; none while locked (the lock is its
// session's own account's).
static struct uuser *g_people;
static int g_npeople;
static struct rect *g_pr;   // each one's row bottom-left; w 0 when not shown
static char **g_pr_name;    // "person.<name>", for wm_login_control()
static int g_hot_person = -1, g_pressed_person = -1;

static void people_free(void) {
    for (int i = 0; g_pr_name && i < g_npeople; i++) free(g_pr_name[i]);
    free(g_pr_name);
    free(g_pr);
    uusers_free(g_people, g_npeople);
    g_people = 0;
    g_pr = 0;
    g_pr_name = 0;
    g_npeople = 0;
}

static void set_account(const char *name, const char *real) {
    free(g_account);
    free(g_real);
    g_account = dup_or_empty(name);
    g_real = dup_or_empty(real && real[0] ? real : name);
    char *comma = strchr(g_real, ',');
    if (comma) *comma = '\0';
}

// The account the screen is for: when locking, our own; in the greeter,
// the person who signed in last, else the first in /etc/passwd.
static void pick_account(void) {
    people_free();
    if (g_mode == WM_LOGIN_LOCK) {
        struct passwd *pw = getpwuid(getuid());
        set_account(pw ? pw->pw_name : "", pw ? pw->pw_gecos : "");
        return;
    }
    int n = uusers_list(&g_people);
    g_npeople = n < 0 ? 0 : n;
    g_pr = calloc((size_t)(g_npeople ? g_npeople : 1), sizeof *g_pr);
    g_pr_name = calloc((size_t)(g_npeople ? g_npeople : 1), sizeof *g_pr_name);
    for (int i = 0; g_pr_name && i < g_npeople; i++) {
        size_t k = strlen(g_people[i].name) + 8;
        if ((g_pr_name[i] = malloc(k))) snprintf(g_pr_name[i], k, "person.%s", g_people[i].name);
    }
    char last[64] = "";
    FILE *f = fopen(LAST_FILE, "r");
    if (f) {
        if (fgets(last, sizeof last, f)) last[strcspn(last, "\r\n")] = '\0';
        fclose(f);
    }
    int pick = 0;
    for (int i = 0; i < g_npeople; i++)
        if (!strcmp(g_people[i].name, last)) pick = i;
    if (g_npeople) set_account(g_people[pick].name, g_people[pick].real);
    else set_account("", "");
}

// Another person chosen from the list: their name in the middle, an
// empty field, no old error.
static void choose_person(int i) {
    if (i < 0 || i >= g_npeople || g_busy || g_signed_in) return;
    if (!strcmp(g_people[i].name, g_account)) return;
    set_account(g_people[i].name, g_people[i].real);
    uui_textbox_set_text(&g_pw, "");
    uui_caret_reset();
    g_err[0] = '\0';
    wm_logf("login: %s chosen\n", g_account);
    WM_REDRAW();
}

void wm_login_show(int mode) {
    g_mode = mode;
    if (mode == WM_LOGIN_LOCK) wm_priv_forget();
    pick_account();
    uui_textbox_init(&g_pw, "");
    g_pw.masked = 1;
    g_pw.ed.flags |= UUI_EDIT_MASKED;
    uui_textbox_set_active(&g_pw, 1);
    uui_caret_reset();
    g_err[0] = '\0';
    g_busy = g_signed_in = 0;
    g_net_open = 0;
    g_snap_ok = 0;
    wm_overlay_close_others(0);
    wm_login_open = 1;
    // The text terminals change to their login form while it is up.
    if (mode == WM_LOGIN_LOCK) {
        int rc = uinitctl_lock(1);
        if (rc != INITCTL_OK) wm_logf("login: init did not take the lock (%d)\n", rc);
    }
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
    // A right password and init stops this greeter AT ONCE (SIGTERM) and
    // starts the session's desktop: armed before asking, so the screen
    // keeps this frame instead of showing the console between.
    if (g_mode != WM_LOGIN_LOCK) wm_compositor_handover(1);
    if (pthread_create(&g_thr, 0, check, 0) != 0) {
        g_busy = 0;
        g_pw.ed.flags &= ~UUI_EDIT_READONLY;
        snprintf(g_err, sizeof g_err, "Could not check the password.");
        free(g_try);
        g_try = 0;
        if (g_mode != WM_LOGIN_LOCK) wm_compositor_handover(0);
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
            uinitctl_lock(0);
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
    if (g_mode != WM_LOGIN_LOCK) wm_compositor_handover(0);
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

// The picture's white ring and strokes: thinner on a list row's small one.
static int pic_line(int r) { return r >= ugfx_char_h() * 2 ? ugfx_px(3) : ugfx_px(2); }

// The picture: a light disc with a head and shoulders in the accent.
static void picture(struct ugfx_surface *s, int cx, int cy, int r) {
    int t = pic_line(r);
    ugfx_fill_circle(s, cx, cy, r + t, WHITE);
    ugfx_fill_circle(s, cx, cy, r, ugfx_rgb(220, 220, 226));
    uint32_t a = UTHEME_ACCENT;
    ugfx_fill_circle(s, cx, cy - r / 4, r * 3 / 10, a);
    ugfx_fill_circle(s, cx, cy - r / 4, r * 3 / 10 - t, ugfx_rgb(220, 220, 226));
    // The shoulders, clipped at the disc's middle -- NESTED in the frame's
    // own damage clip, which is put back after rather than cleared.
    int sr = r * 6 / 10, sy = cy + r * 6 / 10;
    struct ugfx_clip was;
    ugfx_clip_save(s, &was);
    ugfx_clip_intersect(s, cx - r, cy, 2 * r, r * 6 / 10 + 1);
    ugfx_fill_circle(s, cx, sy, sr, a);
    ugfx_fill_circle(s, cx, sy, sr - t, ugfx_rgb(220, 220, 226));
    ugfx_clip_restore(s, &was);
}

// "Keyboard: FI" -- the active layout's code upper-cased, as a keycap
// would be. ONE builder: the button is measured on what is drawn.
static void keyboard_label(char *out, size_t cap) {
    const char *code = layout_count() > 0 ? layout_code(layout_active()) : 0;
    out[0] = '\0';
    if (!code || !code[0]) return;
    snprintf(out, cap, "Keyboard: %s", code);
    for (char *p = out + 10; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}

// The network card's words, read by its size and its drawing alike.
struct net_card {
    char title[32], sub[64];
    int rows;
    char key[2][16], val[2][48];
};

static void net_card(struct net_card *c) {
    struct network_view v;
    network_view(&v);
    network_headline(c->title, sizeof c->title, c->sub, sizeof c->sub, 0);
    c->rows = 0;
    if (!v.have_device) return;
    snprintf(c->key[0], sizeof c->key[0], "IPv4 address");
    if (v.ip[0] && strcmp(v.ip, "none")) snprintf(c->val[0], sizeof c->val[0], "%s/%d", v.ip, v.prefix);
    else snprintf(c->val[0], sizeof c->val[0], "none");
    snprintf(c->key[1], sizeof c->key[1], "Adapter");
    snprintf(c->val[1], sizeof c->val[1], "%s%s%s", v.driver, v.location[0] ? " at " : "", v.location);
    c->rows = 2;
}

static int net_icon_px(void) { return ugfx_char_h() * 2; }
static int net_row_h(void) { return ugfx_char_h() + u() / 2; }

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
    g_r[C_NET] = (struct rect){ g_r[C_RESTART].x - bw - u(), by, bw, bh };
    g_r[C_NETCARD] = (struct rect){ 0, 0, 0, 0 };
    if (g_net_open) {
        // Sized to its words, its right edge on the button's.
        struct net_card c;
        net_card(&c);
        int pad = u() * 2, icon = net_icon_px(), kw = 0, vw = 0;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        int tw = ugfx_text_width(c.title);
        ugfx_set_font(was);
        int sw = ugfx_text_width(c.sub);
        for (int i = 0; i < c.rows; i++) {
            int k = ugfx_text_width(c.key[i]), v = ugfx_text_width(c.val[i]);
            if (k > kw) kw = k;
            if (v > vw) vw = v;
        }
        int inner = icon + u() + (tw > sw ? tw : sw);
        if (c.rows && kw + u() * 2 + vw > inner) inner = kw + u() * 2 + vw;
        int w = pad * 2 + inner;
        if (w < ch * 16) w = ch * 16;
        if (w > screen_w - u() * 6) w = screen_w - u() * 6;
        int head = icon > ch * 2 + ugfx_px(2) ? icon : ch * 2 + ugfx_px(2);
        int h = pad + head + (c.rows ? u() * 2 + 1 + c.rows * net_row_h() : 0) + pad;
        int x = g_r[C_NET].x + g_r[C_NET].w - w;
        if (x < u() * 3) x = u() * 3;
        g_r[C_NETCARD] = (struct rect){ x, by - u() - h, w, h };
    }
    // The keyboard layout, bottom left: a button opening the tray's list
    // when there is a choice (layout_popup.h), none when there is not.
    g_r[C_KEYBOARD] = (struct rect){ 0, 0, 0, 0 };
    if (layout_count() > 1) {
        char kb[48];
        keyboard_label(kb, sizeof kb);
        // A capsule (mockup D1): the text, a gap, the chevron -- and each end
        // as deep as its curve, half the height, or the chevron rides it.
        int kh = ch + u(), end = kh / 2;
        int kw = end + ugfx_text_width(kb) + u() + ch / 2 + end;
        g_r[C_KEYBOARD] = (struct rect){ u() * 3, screen_h - kh - u() * 2, kw, kh };
    }
    // The people, bottom-left above the keyboard row, the last nearest it
    // -- only when there is more than one to choose from. Rows that do
    // not fit below the middle's picture are not drawn.
    int rh = ch * 2 + u() / 2, rw = ch * 12;
    for (int i = 0; i < g_npeople; i++) {
        int w = rh + u() * 2 + ugfx_text_width(g_people[i].real);
        if (w > rw) rw = w;
    }
    if (rw > screen_w / 3) rw = screen_w / 3;
    int bottom = screen_h - (ch + u()) - u() * 5, ceiling = top + pic_r * 2;
    for (int i = 0; g_pr && i < g_npeople; i++) {
        int y = bottom - (g_npeople - i) * (rh + ugfx_px(2));
        g_pr[i] = g_npeople > 1 && y >= ceiling ? (struct rect){ u() * 3, y, rw, rh } : (struct rect){ 0, 0, 0, 0 };
    }
}

int wm_login_keyboard_rect(int *x, int *y, int *w, int *h) {
    if (!wm_login_open) return 0;
    layout();
    const struct rect *r = &g_r[C_KEYBOARD];
    *x = r->x; *y = r->y; *w = r->w; *h = r->h;
    return r->w > 0;
}

// Restart's and Shut down's icons -- the Leave page's own (leave_page.c),
// tinted white.
static void power_glyph(struct ugfx_surface *s, int restart, int cx, int cy, int size) {
    const struct uimg *ico = icon_get(restart ? "tb-refresh" : "tb-power", size);
    if (ico) ugfx_blit_tinted(s, cx - ico->w / 2, cy - ico->h / 2, ico->w, ico->h, ico->px, ico->w, WHITE);
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

    // Bottom left: the keyboard layout -- a button when there is a choice
    // (mockups K1, D1) -- and the computer's name.
    char host[64] = "";
    if (!usetting_get("system.hostname", host, sizeof host)) host[0] = '\0';
    char kb[48] = "";
    keyboard_label(kb, sizeof kb);
    int hx = u() * 3, hy = screen_h - ch - u() * 3;
    const struct rect *k = &g_r[C_KEYBOARD];
    if (k->w > 0) {
        // Flat at rest, as Restart and Shut down are: the chevron says it
        // opens something, and the capsule shows under the pointer or open.
        if (g_hot == C_KEYBOARD || layout_open)
            uui_glass_round_rect(s, k->x, k->y, k->w, k->h, UUI_CAPSULE, WHITE, 80, 100);
        int ty = k->y + (k->h - ch) / 2, end = k->h / 2;
        ugfx_draw_string_clipped(s, k->x + end, ty, k->w - 2 * end, kb, WHITE, UGFX_TRANSPARENT);
        // The chevron, a gap after the text, points the way the list opens.
        int cx = k->x + end + ugfx_text_width(kb) + u() + ch / 4, cy = k->y + k->h / 2;
        ugfx_draw_line(s, cx - ch / 4, cy + ch / 8, cx, cy - ch / 8, WHITE, GEOM_AA);
        ugfx_draw_line(s, cx, cy - ch / 8, cx + ch / 4, cy + ch / 8, WHITE, GEOM_AA);
        hx = k->x + k->w + u() * 2;
        hy = ty;   // the hostname on the button's line
        kb[0] = '\0';
    }
    char bl[128];
    snprintf(bl, sizeof bl, "%s%s%s", kb, kb[0] && host[0] ? "    " : "", host);
    ugfx_draw_string_clipped(s, hx, hy, screen_w / 2, bl, SOFT, UGFX_TRANSPARENT);

    // Bottom left, above the keyboard: everyone who may sign in, the one
    // in the middle marked.
    for (int i = 0; g_pr && i < g_npeople; i++) {
        const struct rect *r = &g_pr[i];
        if (r->w <= 0) continue;
        int on = !strcmp(g_people[i].name, g_account), hot = i == g_hot_person;
        if (on || hot)
            uui_glass_round_rect(s, r->x, r->y, r->w, r->h, u(), WHITE, on ? 70 : 40, on ? 90 : 60);
        // The picture a row's height less a margin, its ring inside it.
        int pr = r->h / 2 - u() / 2 - pic_line(0), pcx = r->x + u() / 2 + pic_line(0) + pr;
        picture(s, pcx, r->y + r->h / 2, pr);
        int tx = pcx + pr + pic_line(0) + u();
        ugfx_draw_string_clipped(s, tx, r->y + (r->h - ch) / 2, r->x + r->w - u() - tx,
                                 g_people[i].real, WHITE, UGFX_TRANSPARENT);
    }

    // Bottom right: Restart and Shut down.
    for (int k = C_RESTART; k <= C_SHUTDOWN; k++) {
        struct rect *r = &g_r[k];
        if (g_hot == k)
            for (int y = r->y; y < r->y + r->h; y++) ugfx_blend_hspan(s, r->x, y, r->w, WHITE, 0, 40);
        power_glyph(s, k == C_RESTART, r->x + r->w / 2, r->y + ch / 2 + ugfx_px(6), ch + ugfx_px(4));
        text_c(s, r->x + r->w / 2, r->y + r->h - ch - ugfx_px(2), k == C_RESTART ? "Restart" : "Shut down", WHITE);
    }

    // The network button -- Restart's shape, lit while its card is open --
    // then the card.
    const struct rect *nb = &g_r[C_NET];
    if (g_hot == C_NET || g_net_open)
        for (int y = nb->y; y < nb->y + nb->h; y++)
            ugfx_blend_hspan(s, nb->x, y, nb->w, WHITE, 0, g_net_open ? 56 : 40);
    const struct uimg *ni = icon_get(network_icon(), ch + ugfx_px(4));
    if (ni) ugfx_blit_tinted(s, nb->x + (nb->w - ni->w) / 2, nb->y + ch / 2 + ugfx_px(6) - ni->h / 2, ni->w, ni->h,
                             ni->px, ni->w, WHITE);
    text_c(s, nb->x + nb->w / 2, nb->y + nb->h - ch - ugfx_px(2), "Network", WHITE);
    const struct rect *cr = &g_r[C_NETCARD];
    if (cr->w > 0) {
        // The keyboard list's dark glass (layout_popup.c's login_draw).
        // NOT SELECTABLE, unlike text elsewhere (userland/CLAUDE.md): a
        // login screen copies nothing out -- the greeter's clipboard is
        // its own and wiped -- as Windows' sign-in flyouts do not.
        struct net_card c;
        net_card(&c);
        int pad = u() * 2, icon = net_icon_px(), rad = ugfx_px(8);
        uui_glass_round_rect(s, cr->x, cr->y, cr->w, cr->h, rad, ugfx_rgb(30, 34, 46), 235, 0);
        uui_glass_round_rect(s, cr->x, cr->y, cr->w, cr->h, rad, WHITE, 0, 46);
        int head = icon > ch * 2 + ugfx_px(2) ? icon : ch * 2 + ugfx_px(2);
        int hy = cr->y + pad;
        const struct uimg *ci = icon_get(network_icon(), icon);
        if (ci) ugfx_blit_tinted(s, cr->x + pad, hy + (head - ci->h) / 2, ci->w, ci->h, ci->px, ci->w, WHITE);
        int tx = cr->x + pad + icon + u(), tw = cr->x + cr->w - pad - tx;
        int ty = hy + (head - (ch * 2 + ugfx_px(2))) / 2;
        const struct ugfx_font *bold = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, tx, ty, tw, c.title, WHITE, UGFX_TRANSPARENT);
        ugfx_set_font(bold);
        ugfx_draw_string_clipped(s, tx, ty + ch + ugfx_px(2), tw, c.sub, SOFT, UGFX_TRANSPARENT);
        if (c.rows) {
            int ry = hy + head + u();
            ugfx_blend_hspan(s, cr->x + pad, ry, cr->w - 2 * pad, WHITE, 0, 56);
            ry += 1 + u();
            int kw = 0;
            for (int i = 0; i < c.rows; i++) {
                int k = ugfx_text_width(c.key[i]);
                if (k > kw) kw = k;
            }
            int vx = cr->x + pad + kw + u() * 2;
            for (int i = 0; i < c.rows; i++, ry += net_row_h()) {
                int y = ry + (net_row_h() - ch) / 2;
                ugfx_draw_string_clipped(s, cr->x + pad, y, kw, c.key[i], SOFT, UGFX_TRANSPARENT);
                ugfx_draw_string_clipped(s, vx, y, cr->x + cr->w - pad - vx, c.val[i], WHITE, UGFX_TRANSPARENT);
            }
        }
    }
}

// --- input -----------------------------------------------------------------

static int person_at(int x, int y) {
    for (int i = 0; g_pr && i < g_npeople; i++) {
        const struct rect *r = &g_pr[i];
        if (r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

// From the last: the open card (C_NETCARD) before anything beneath it.
static int ctl_at(int x, int y) {
    for (int i = CTLS - 1; i >= 0; i--) {
        const struct rect *r = &g_r[i];
        if (r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

static void activate(int id) {
    switch (id) {
    case C_GO: submit(); break;
    case C_KEYBOARD:
        g_net_open = 0;
        if (layout_open) layout_close();
        else layout_open_page();
        WM_REDRAW();
        break;
    case C_NET:
        g_net_open = !g_net_open;
        wm_logf("login: network card %s\n", g_net_open ? "opened" : "closed");
        WM_REDRAW();
        break;
    case C_NETCARD:   // read-only: a click on it keeps it open
        break;
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
    int at = ctl_at(mx, my), person = person_at(mx, my);
    if (buttons & 1) {
        // The field hears the press EDGE once: its eye toggles on a press,
        // and this runs every tick the button is held.
        if (g_pressed < 0 && g_pressed != -3) {
            // A press outside the open network card only closes it, as
            // a click outside any flyout does.
            if (g_net_open && at != C_NET && at != C_NETCARD) {
                g_net_open = 0;
                g_pressed = -3;
                WM_REDRAW();
                return;
            }
            g_pressed = at < 0 ? -2 : at;
            g_pressed_person = person;
            if (g_pressed == C_FIELD) { uui_textbox_ops.press(&g_pw, mx, my, 0); WM_REDRAW(); }
        }
        return;
    }
    int was = g_pressed, was_person = g_pressed_person;
    g_pressed = g_pressed_person = -1;
    if (layout_take_dismissed()) return;   // the press only closed the layout list
    if (was_person >= 0 && was_person == person) { choose_person(person); return; }
    if (was >= 0 && was == at && was != C_FIELD) activate(was);
}

int wm_login_hover_at(int mx, int my) {
    if (!wm_login_open) return 0;
    int at = ctl_at(mx, my), person = person_at(mx, my);
    g_hot = at == C_FIELD ? -1 : at;
    if (person != g_hot_person) { g_hot_person = person; WM_REDRAW(); }
    return person >= 0 ? CTLS + 1 + person : g_hot + 1;
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

int wm_login_text_at(int mx, int my) {
    return wm_login_open && g_r[C_FIELD].w > 0 && uui_textbox_text_at(&g_pw, mx, my);
}

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

// What the pointer lights, in wm_login_control()'s numbering; -1 none.
int wm_login_hot(void) {
    if (!wm_login_open) return -1;
    return g_hot_person >= 0 ? CTLS + g_hot_person : g_hot;
}

// The screen's controls, then each person's row ("person.<name>").
int wm_login_control(int i, const char **name, int *x, int *y, int *w, int *h) {
    if (!wm_login_open || i < 0 || i >= CTLS + (g_pr ? g_npeople : 0)) return 0;
    if (i >= CTLS) {
        const struct rect *r = &g_pr[i - CTLS];
        *name = g_pr_name[i - CTLS] ? g_pr_name[i - CTLS] : "person";
        *x = r->x; *y = r->y; *w = r->w; *h = r->h;
        return 1;
    }
    *name = CTL_NAME[i];
    *x = g_r[i].x; *y = g_r[i].y; *w = g_r[i].w; *h = g_r[i].h;
    return 1;
}
