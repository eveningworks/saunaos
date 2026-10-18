// The pictures in a gallery's cards (Widget=gallery), chosen by the
// setting's `Preview=` word. A TABLE of painters, so the wallpaper picker
// is one more row. Each painter draws choice `i` of the slot it is handed.
#include "settings_internal.h"
#include "lib/ucursor.h"
#include "lib/usetting_schema.h"
#include "lib/usetting_text.h"
#include "lib/ulivewall.h"
#include "lib/uimg.h"
#include "lib/uvid.h"
#include "ui/uui_fontsample.h"
#include "ui/uui_tiles.h"
#include "ui/uui_menubar.h"   // uui_menu_spacing_metrics
#include "ui/uui_popup.h"
#include "ui/utheme.h"
#include "lib/usetting.h"
#include "lib/icon_cache.h"
#include <stdlib.h>

// --- cursor sets and pictures: ui/uui_tiles.h, shared with First-boot setup

// Cached per page; freed by preview_reset().
static struct uui_cursor_tiles g_pv_cursor;
static struct uui_picture_tiles g_pv_pic, g_pv_live;
static struct uui_picture_tiles g_pv_icons, g_pv_menu, g_pv_glass;
static char g_wi_stem[SETTING_ABI_VALUE_MAX];   // wallpaper_info()'s, below
static struct uimg g_place[2];   // Fill's and Fit's, each at its drawn size
static char g_place_stem[2][SETTING_ABI_VALUE_MAX];
static int g_place_w[2], g_place_h[2];   // the size each was made for
#define PREVIEW_DARK UUI_TILE_DARK

static void pictures_reset(void);
static void fonts_reset(void);

void preview_reset(void) {
    pictures_reset();
    fonts_reset();
    uui_cursor_tiles_reset(&g_pv_cursor);
    theme_tiles_reset();
}

static void cursor_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    uui_cursor_tile(s, &g_pv_cursor, i, i >= 0 && i < sl->choice_count ? sl->choice_raw[i] : 0,
                    x, y, w, h);
}

static struct uimg g_mon;
static char g_mon_name[SETTING_ABI_VALUE_MAX];

static void pictures_reset(void) {
    uui_picture_tiles_reset(&g_pv_pic);
    uui_picture_tiles_reset(&g_pv_live);
    for (int k = 0; k < 2; k++) { uimg_free(&g_place[k]); g_place_stem[k][0] = '\0'; }
    uui_picture_tiles_reset(&g_pv_icons);
    uui_picture_tiles_reset(&g_pv_menu);
    g_wi_stem[0] = '\0';
    uui_picture_tiles_reset(&g_pv_glass);
    uimg_free(&g_mon);
    g_mon_name[0] = '\0';
}

// One still frame of an effect, from the effect's own code
// (lib/ulivewall.c), a fixed distance into its clock so it shows what it
// looks like moving rather than its first instant.
static int render_effect(const char *name, int w, int h, struct uimg *out) {
    static struct ulivewall fx;
    if (!ulivewall_open(&fx, name)) return 0;
    ulivewall_step(&fx, 20000);
    out->px = malloc((size_t)w * (size_t)h * 4);
    if (out->px) {
        out->w = w;
        out->h = h;
        struct ugfx_surface t = ugfx_surface_for_pixels(out->px, w, h);
        ulivewall_render(&fx, &t, 4);
    }
    ulivewall_close(&fx);
    return out->px != 0;
}

#define SAVER_PICTURES "/usr/wm/savers"   // a saver's thumbnail beside its descriptor

// A LIVE CHOICE is an effect, or else an animated picture, whose first
// frame stands for it.
static int load_live(const char *name, int w, int h, struct uimg *out, void *ctx) {
    (void)ctx;
    return render_effect(name, w, h, out) ||
           uui_tile_load_cover(LIVEWALL_ANIMATED_DIR, name, w, h, out);
}

static void draw_cached(struct ugfx_surface *s, struct uui_picture_tiles *c, const struct slot *sl,
                        int i, int x, int y, int w, int h, uui_tile_loader load, const char *dir) {
    uui_picture_tile(s, c, i, i >= 0 && i < sl->choice_count ? sl->choice_raw[i] : 0,
                     x, y, w, h, load, (void *)dir);
}

static void picture_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    draw_cached(s, &g_pv_pic, ctx, i, x, y, w, h, uui_tile_load_dir, "/usr/share/wallpapers");
}

static void saver_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    draw_cached(s, &g_pv_pic, ctx, i, x, y, w, h, uui_tile_load_dir, SAVER_PICTURES);
}

// THE PREVIEW MONITOR above the screensaver gallery (mockup G2): the
// chosen saver's picture on a screen with a bezel and a stand. A STILL --
// Windows' preview runs the saver into a child window, and nothing here
// can host another program's window yet.

// The bezel and stand, centred in (x, y, w, h), for a screen of aspect
// aw:ah; the screen's rect comes back black in *sx.. . 0 when it cannot fit.
static int monitor_frame(struct ugfx_surface *s, int x, int y, int w, int h, int aw, int ah,
                         int *sx, int *sy, int *sw, int *sh) {
    int bez = ugfx_char_h() / 2 + 2, stand = ugfx_char_h();
    *sh = h - 2 * bez - stand;
    *sw = *sh * aw / ah;
    if (*sw + 2 * bez > w) { *sw = w - 2 * bez; *sh = *sw * ah / aw; }
    if (*sw <= 0 || *sh <= 0) return 0;
    int mx = x + (w - *sw - 2 * bez) / 2;
    uui_fill_round_rect(s, mx, y, *sw + 2 * bez, *sh + 2 * bez, bez, 0x2b2d31);
    ugfx_fill_rect(s, mx + (*sw + 2 * bez) / 2 - stand, y + *sh + 2 * bez, 2 * stand, stand / 2, 0x2b2d31);
    ugfx_fill_rect(s, mx + (*sw + 2 * bez) / 2 - 2 * stand, y + *sh + 2 * bez + stand / 2, 4 * stand, stand / 2, 0x2b2d31);
    *sx = mx + bez;
    *sy = y + bez;
    ugfx_fill_rect(s, *sx, *sy, *sw, *sh, 0);
    return 1;
}

void preview_monitor(struct ugfx_surface *s, int x, int y, int w, int h, const char *saver) {
    int sx, sy, sw, sh;
    if (!monitor_frame(s, x, y, w, h, 16, 9, &sx, &sy, &sw, &sh)) return;
    if (!saver || !saver[0]) return;
    if (strcmp(g_mon_name, saver) != 0 || g_mon.w < sw || g_mon.h < sh) {
        uimg_free(&g_mon);
        strlcpy(g_mon_name, saver, sizeof g_mon_name);
        uui_tile_load_cover(SAVER_PICTURES, saver, sw, sh, &g_mon);
    }
    if (!g_mon.px) return;
    int ox = (g_mon.w - sw) / 2, oy = (g_mon.h - sh) / 2;
    ugfx_blit(s, sx, sy, sw, sh, g_mon.px + (size_t)oy * (size_t)g_mon.w + ox, g_mon.w);
}

static void live_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    draw_cached(s, &g_pv_live, ctx, i, x, y, w, h, load_live, 0);
}

// --- colour: the swatch -------------------------------------------------

static void colour_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    uint32_t c = i >= 0 && i < sl->choice_count ? ulivewall_colour(sl->choice_raw[i]) : PREVIEW_DARK;
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, c);
}

// --- fonts: each face's sample, drawn in the face itself ------------------
//
// Opened once per page and shared by both galleries (Interface and
// Monospace offer the same faces), keyed by stem. `builtin` has no file:
// it draws through the baked tables (ugfx_font_baked()).
#define PREVIEW_FACES 16
static struct uui_fontface g_faces[PREVIEW_FACES];
static int g_nfaces;

static void fonts_reset(void) {
    for (int i = 0; i < g_nfaces; i++) uui_fontface_close(&g_faces[i]);
    g_nfaces = 0;
}

static struct uui_fontface *face_for(const char *stem) {
    if (!strcmp(stem, "builtin")) return 0;
    for (int i = 0; i < g_nfaces; i++)
        if (!strcmp(g_faces[i].stem, stem)) return &g_faces[i];
    if (g_nfaces < PREVIEW_FACES && uui_fontface_open(&g_faces[g_nfaces], UUI_FONT_DIR, stem))
        return &g_faces[g_nfaces++];
    return 0;
}

static void font_paint(struct ugfx_surface *s, int i, int x, int y, int w, int h,
                       const struct slot *sl, int mono) {
    if (i < 0 || i >= sl->choice_count) return;
    struct uui_fontface *f = face_for(sl->choice_raw[i]);
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, UTHEME_WHITE);
    if (!mono) {
        uui_fontsample_draw(s, f, UUI_FONTSAMPLE_CARD, x, y, w, h, UTHEME_WHITE, 0);
        return;
    }
    // A PROPORTIONAL FACE AS MONOSPACE is allowed -- ring 0 cannot tell --
    // and ruins every column: dimmed, and said so under its sample.
    int prop = f && !f->mono;
    int ch = ugfx_char_h(), note = prop ? ch + ch / 3 : 0;
    uui_fontsample_draw(s, f, UUI_FONTSAMPLE_TERMINAL, x + 4, y + 4, w - 8, h - 8 - note,
                        UTHEME_WHITE, prop);
    if (prop)
        ugfx_draw_string_clipped(s, x + 6, y + h - ch - 4, w - 12, "Not fixed-width",
                                 utheme_current()->severity[UTHEME_SEV_WARNING], UTHEME_WHITE);
}

static void font_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    font_paint(s, i, x, y, w, h, ctx, 0);
}

static void fontmono_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    font_paint(s, i, x, y, w, h, ctx, 1);
}

// A card is named by the family the file calls itself, not its stem:
// "DejaVu Sans Mono", not "dejavu-sans-mono".
static void font_labels(struct slot *sl) {
    for (int i = 0; i < sl->choice_count; i++) {
        struct uui_fontface *f = face_for(sl->choice_raw[i]);
        if (f) snprintf(sl->choice[i], sizeof sl->choice[i], "%s", f->family);
        else if (!strcmp(sl->choice_raw[i], "builtin"))
            snprintf(sl->choice[i], sizeof sl->choice[i], "Built-in");
    }
}

// --- window behaviour: the desktop in miniature ---------------------------
//
// One scene for four settings (mockups W1, W3): the desktop's colour, a
// taskbar strip, and windows drawn the way the choice draws them. Every
// size is a fraction of the tile, which the gallery derives from the font.

#define MINI_TITLE_ON  ugfx_rgb(50, 90, 160)     // wm_render.c's title bars
#define MINI_TITLE_OFF ugfx_rgb(120, 120, 130)
#define MINI_TASKBAR   ugfx_rgb(31, 37, 46)
#define MINI_GLASS_A   140

enum { MW_SHADOW = 1, MW_FOCUSED = 2, MW_GLASS_TITLE = 4, MW_GLASS_BODY = 8 };

// The desktop and its taskbar, on a card's rounded tile or (square) a
// monitor's screen; returns the height above the taskbar.
static int mini_desktop(struct ugfx_surface *s, int x, int y, int w, int h, int icons, int square) {
    uui_fill_round_rect(s, x, y, w, h, square ? 0 : ugfx_char_h() / 3, PREVIEW_DARK);
    int bar = h / 9 < 3 ? 3 : h / 9;
    ugfx_fill_rect(s, x, y + h - bar, w, bar, MINI_TASKBAR);
    // Desktop icons, so a see-through window has something to show.
    static const uint32_t ICON[] = { 0xE2A33B, 0x5FA866, 0x7FA7D6, 0xC8CCD4 };
    int is = h / 6;
    for (int i = 0; i < icons; i++)
        uui_fill_round_rect(s, x + is / 2 + (i / 2) * (is * 3 / 2), y + is / 2 + (i % 2) * (is * 3 / 2),
                            is, is, is / 4, ICON[i]);
    return h - bar;
}

static void mini_window(struct ugfx_surface *s, int x, int y, int w, int h, int flags) {
    int tb = h / 6 < 4 ? 4 : h / 6;
    // Three nested panes, offset down and right: the soft edge a real
    // shadow has, at a size where one dark band would read as a border.
    for (int k = 3; (flags & MW_SHADOW) && k >= 1; k--)
        uui_glass_round_rect(s, x + 2 - k, y + 3 - k, w + 2 * k, h + 2 * k, k + 2, 0, 55, 0);
    uint32_t title = (flags & MW_FOCUSED) ? MINI_TITLE_ON : MINI_TITLE_OFF;
    if (flags & MW_GLASS_BODY) uui_glass_round_rect(s, x, y + tb, w, h - tb, 1, UTHEME_WINDOW_BG, MINI_GLASS_A, 0);
    else ugfx_fill_rect(s, x, y + tb, w, h - tb, UTHEME_WINDOW_BG);
    if (flags & MW_GLASS_TITLE) uui_glass_round_rect(s, x, y, w, tb, 1, title, MINI_GLASS_A, 0);
    else ugfx_fill_rect(s, x, y, w, tb, title);
    // Two lines of content, where they fit.
    for (int k = 0; k < 2; k++) {
        int ly = y + tb * (2 + k * 3 / 2);
        if (ly + tb / 2 < y + h - 2) ugfx_fill_rect(s, x + tb, ly, w * (3 - k) / 5, tb / 2 > 1 ? tb / 2 : 2, UTHEME_BORDER);
    }
}

// The rubber band a move or resize drags instead of the window.
static void dashed_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    int d = ugfx_char_h() / 5 + 1, t = 2;
    for (int i = 0; i < w; i += 2 * d) {
        int n = w - i < d ? w - i : d;
        ugfx_fill_rect(s, x + i, y, n, t, 0xFFFFFF);
        ugfx_fill_rect(s, x + i, y + h - t, n, t, 0xFFFFFF);
    }
    for (int i = 0; i < h; i += 2 * d) {
        int n = h - i < d ? h - i : d;
        ugfx_fill_rect(s, x, y + i, t, n, 0xFFFFFF);
        ugfx_fill_rect(s, x + w - t, y + i, t, n, 0xFFFFFF);
    }
}

// An arrow pointer with its tip at (x, y), sized from the font.
static void mini_pointer(struct ugfx_surface *s, int x, int y) {
    static const int PX[] = { 0, 0, 4, 7, 9, 7, 11 }, PY[] = { 0, 16, 12, 18, 17, 11, 11 };
    int k = ugfx_char_h() >= 24 ? 2 : 1, n = (int)(sizeof PX / sizeof PX[0]);
    int xs[7], ys[7];
    for (int i = 0; i < n; i++) { xs[i] = x + PX[i] * k * 3 / 4; ys[i] = y + PY[i] * k * 3 / 4; }
    ugfx_fill_polygon(s, xs, ys, n, 0xFFFFFF);
    ugfx_draw_polyline(s, xs, ys, n, 1, 0x141414, GEOM_AA);
}

static const char *choice_word(const struct slot *sl, int i) {
    return i >= 0 && i < sl->choice_count ? sl->choice_raw[i] : "";
}

// desktop.move_mode: the window where it was dragged to, or the window
// left behind with the outline there instead.
static void winmove_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int dh = mini_desktop(s, x, y, w, h, 0, 0);
    int ww = w * 2 / 5, wh = dh * 3 / 5, tx = x + w - ww - w / 8, ty = y + dh / 4;
    int outline = !strcmp(choice_word(ctx, i), "outline");
    if (outline) {
        mini_window(s, x + w / 10, y + dh / 10, ww, wh, MW_SHADOW | MW_FOCUSED);
        dashed_rect(s, tx, ty, ww, wh);
    } else {
        mini_window(s, tx, ty, ww, wh, MW_SHADOW | MW_FOCUSED);
    }
    mini_pointer(s, tx + ww / 2, ty + 2);
}

// desktop.resize_mode: the window grown to the pointer, or kept and the
// outline grown. Automatic shows what it shows when the app keeps up.
static void winresize_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int dh = mini_desktop(s, x, y, w, h, 0, 0);
    int wx = x + w / 10, wy = y + dh / 10, bw = w * 3 / 4, bh = dh * 4 / 5;
    if (!strcmp(choice_word(ctx, i), "outline")) {
        mini_window(s, wx, wy, bw * 3 / 5, bh * 3 / 5, MW_SHADOW | MW_FOCUSED);
        dashed_rect(s, wx, wy, bw, bh);
    } else {
        mini_window(s, wx, wy, bw, bh, MW_SHADOW | MW_FOCUSED);
    }
    mini_pointer(s, wx + bw - 2, wy + bh - 2);
}

// Two windows, one behind the other: the shadow and see-through settings
// both read in how the pair overlaps.
static void window_pair(struct ugfx_surface *s, int x, int y, int w, int h, int back, int front) {
    int dh = mini_desktop(s, x, y, w, h, 4, 0);
    mini_window(s, x + w / 40, y + dh / 14, w / 2, dh * 2 / 3, back);
    mini_window(s, x + w * 3 / 10, y + dh / 4, w / 2, dh * 2 / 3, front | MW_FOCUSED);
}

// desktop.shadows
static void shadow_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int f = !strcmp(choice_word(ctx, i), "off") ? 0 : MW_SHADOW;
    window_pair(s, x, y, w, h, f, f);
}

// desktop.transparency_windows: which of the two lets the desktop through.
static void seethrough_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const char *c = choice_word(ctx, i);
    int title = MW_GLASS_TITLE, all = MW_GLASS_TITLE | MW_GLASS_BODY, back = 0, front = 0;
    if (!strcmp(c, "titlebars")) back = front = title;
    else if (!strcmp(c, "inactive")) back = all;
    else if (!strcmp(c, "all")) back = front = all;
    window_pair(s, x, y, w, h, back | MW_SHADOW, front | MW_SHADOW);
}

// --- the Screen page's monitor (mockup D1) ---------------------------------
//
// The panel, and the mode on it the way the scaler places it. A mode the
// panel shows natively, or a display with no scaler or no EDID, fills it.

static void place_mode(int pw, int ph, int mw, int mh, const char *scaling,
                       int *ow, int *oh) {
    *ow = pw; *oh = ph;
    if (!strcmp(scaling, "center")) {
        *ow = mw < pw ? mw : pw;
        *oh = mh < ph ? mh : ph;
    } else if (!strcmp(scaling, "aspect")) {
        if ((int64_t)mw * ph > (int64_t)pw * mh) *oh = (int)((int64_t)pw * mh / mw);
        else *ow = (int)((int64_t)ph * mw / mh);
    }
}

void preview_screen(struct ugfx_surface *s, int x, int y, int w, int h, struct preview_screen *ps) {
    int ch = ugfx_char_h(), cap = ch + ch / 2;
    ps->caption[0] = '\0';
    int pw = ps->panel_w, ph = ps->panel_h, mw = ps->mode_w, mh = ps->mode_h;
    if (mw <= 0 || mh <= 0) return;
    int scaled = pw > 0 && ph > 0 && ps->can_scale && (pw != mw || ph != mh);
    if (!scaled) { pw = mw; ph = mh; }
    int sx, sy, sw, sh;
    if (!monitor_frame(s, x, y, w, h - cap, pw, ph, &sx, &sy, &sw, &sh)) return;
    int ow, oh;
    place_mode(pw, ph, mw, mh, scaled ? ps->scaling : "full", &ow, &oh);
    int rw = (int)((int64_t)sw * ow / pw), rh = (int)((int64_t)sh * oh / ph);
    int rx = sx + (sw - rw) / 2, ry = sy + (sh - rh) / 2;
    // The scene in the mode's own pixels, so a stretch shows as one.
    int dh = mini_desktop(s, rx, ry, rw, rh, 0, 1);
    int sun = mh / 7;
    ugfx_fill_ellipse(s, rx + rw * (mw - sun) / mw, ry + rh * sun * 3 / 2 / mh,
                      rw * sun / 2 / mw, rh * sun / 2 / mh, 0xE2A33B);
    mini_window(s, rx + rw / 9, ry + dh / 6, rw / 2, dh / 2, MW_SHADOW | MW_FOCUSED);

    char *text = ps->caption;
    size_t cap_n = sizeof ps->caption;
    if (!scaled)
        snprintf(text, cap_n, "%dx%d%s", mw, mh,
                 ps->panel_w == mw && ps->panel_h == mh ? ", the panel's own size" : "");
    else
        snprintf(text, cap_n, "%dx%d on a %dx%d panel: %s", mw, mh, pw, ph,
                 !strcmp(ps->scaling, "center") ? "centred, not scaled"
                 : !strcmp(ps->scaling, "full") ? "stretched to fill" : "scaled, shape kept");
    int tw = ugfx_text_width(text);
    ugfx_draw_string_clipped(s, x + (tw < w ? (w - tw) / 2 : 0), y + h - ch - ch / 4, w, text,
                             UTHEME_TEXT, UTHEME_WINDOW_BG);
}

// --- the desktop: placement, icon size, menu spacing, corners (round 3) ---
//
// Each drawn from what the setting really does: the wallpaper you have,
// icons at their real size, a menu at its real row height, the corner
// radii utheme.c rounds with. A value from ANOTHER setting is read from
// this page when it is on it (staged), else as it is in force.

#define WALLPAPERS "/usr/share/wallpapers"
static int g_scr_w = 16, g_scr_h = 9;   // the screen's shape, from attach

// Always into `buf`, which is also returned.
static const char *page_value(const char *name, char *buf, int cap) {
    struct slot *sl = slot_find(name);
    if (sl) strlcpy(buf, staged_value(sl), (size_t)cap);
    else if (!usetting_get(name, buf, (size_t)cap)) buf[0] = '\0';
    return buf;
}

static uint32_t desktop_colour(void) {
    char v[SETTING_ABI_VALUE_MAX];
    return ulivewall_colour(page_value("desktop.background_colour", v, sizeof v));
}

// The wallpaper's stem and its size, by the first extension that has a
// header -- the desktop's own order (wm/desktop.c). 0 with no picture.
// Remembered by stem: a header read is up to 256 KiB, and every card of
// three galleries asks on every paint.
static int g_wi_w, g_wi_h;

static int wallpaper_info(char *stem, int cap, int *iw, int *ih) {
    static const char *const EXT[] = { "jpg", "gif", "png", "qoi" };
    page_value("desktop.wallpaper", stem, cap);
    if (!stem[0] || !strcmp(stem, "none")) return 0;
    if (strcmp(stem, g_wi_stem) != 0) {
        strlcpy(g_wi_stem, stem, sizeof g_wi_stem);
        g_wi_w = g_wi_h = 0;
        for (unsigned e = 0; e < sizeof EXT / sizeof EXT[0] && !g_wi_w; e++) {
            char path[SETTING_ABI_VALUE_MAX + 40];
            struct uimg_info in;
            snprintf(path, sizeof path, WALLPAPERS "/%s.%s", stem, EXT[e]);
            if (uimg_load_info(path, &in) == 0 && in.w > 0 && in.h > 0) {
                g_wi_w = in.w;
                g_wi_h = in.h;
            }
        }
    }
    *iw = g_wi_w;
    *ih = g_wi_h;
    return g_wi_w > 0;
}

int preview_placement_same(void) {
    char stem[SETTING_ABI_VALUE_MAX];
    int iw, ih;
    if (!wallpaper_info(stem, sizeof stem, &iw, &ih)) return 0;
    int64_t a = (int64_t)iw * g_scr_h, b = (int64_t)ih * g_scr_w;
    return (a > b ? a - b : b - a) * 100 <= a;   // within 1%
}

// The picture COVERING (x, y, w, h), or the desktop's colour without one.
static void desktop_backdrop(struct ugfx_surface *s, struct uui_picture_tiles *c, int i,
                             int x, int y, int w, int h) {
    char stem[SETTING_ABI_VALUE_MAX];
    int iw, ih;
    ugfx_fill_rect(s, x, y, w, h, desktop_colour());
    if (wallpaper_info(stem, sizeof stem, &iw, &ih))
        uui_picture_tile(s, c, i, stem, x, y, w, h, uui_tile_load_dir, (void *)WALLPAPERS);
}

// desktop.wallpaper_mode: the wallpaper on a screen of this screen's
// shape, covering it or whole on the desktop's colour.
static void placement_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    int sx, sy, sw, sh;
    if (!monitor_frame(s, x, y, w, h, g_scr_w, g_scr_h, &sx, &sy, &sw, &sh)) return;
    ugfx_fill_rect(s, sx, sy, sw, sh, desktop_colour());
    char stem[SETTING_ABI_VALUE_MAX];
    int iw, ih, dw = sw, dh = sh;
    if (!wallpaper_info(stem, sizeof stem, &iw, &ih)) return;
    if (!strcmp(choice_word(ctx, i), "fit")) uimg_fit_size(iw, ih, sw, sh, UIMG_FIT_CONTAIN, &dw, &dh);
    // A cover of EXACTLY the drawn size, which fitted is the whole
    // picture -- so made again at any other size, never a centre crop of
    // a bigger one, as uui_picture_tile() would reuse.
    if (i < 0 || i > 1) return;
    struct uimg *im = &g_place[i];
    if (strcmp(g_place_stem[i], stem) || g_place_w[i] != dw || g_place_h[i] != dh) {
        uimg_free(im);
        strlcpy(g_place_stem[i], stem, sizeof g_place_stem[i]);
        g_place_w[i] = dw;
        g_place_h[i] = dh;
        if (!uui_tile_load_cover(WALLPAPERS, stem, dw, dh, im)) return;
    }
    // The cover may be a pixel over in one axis: its centre.
    if (im->px && im->w >= dw && im->h >= dh)
        ugfx_blit(s, sx + (sw - dw) / 2, sy + (sh - dh) / 2, dw, dh,
                  im->px + (size_t)((im->h - dh) / 2) * (size_t)im->w + (im->w - dw) / 2, im->w);
}

// desktop.icon_size: three desktop icons at the size, over the wallpaper.
// The pixel sizes are wm/desktop.c's icon_size_reload().
static int icon_px_of(const char *word) {
    return !strcmp(word, "small") ? 32 : !strcmp(word, "large") ? 64 : 48;
}
#define ICONS_TILE_H (64 + ugfx_char_h() * 2)

static void iconsize_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    static const char *const ICON[] = { "files", "notepad", "trash-empty" };
    static const char *const LABEL[] = { "File Manager", "Notepad", "Recycle Bin" };
    desktop_backdrop(s, &g_pv_icons, i, x, y, w, h);
    int px = icon_px_of(choice_word(ctx, i)), ch = ugfx_char_h();
    int col = (w - ch / 3) / 3;   // a third each: the labels are the width, not the icons
    if (col < px + ch / 2) col = px + ch / 2;
    for (int k = 0; k < 3; k++) {
        int cx = x + ch / 3 + k * col;
        if (cx + col > x + w) break;   // what fits; a larger size shows fewer
        const struct uimg *ic = icon_get(ICON[k], px);
        int iy = y + ch / 3 + (64 - px) / 2;
        if (ic) ugfx_blit_alpha(s, cx + (col - ic->w) / 2, iy, ic->w, ic->h, ic->px, ic->w);
        int tw = ugfx_text_width(LABEL[k]);
        ugfx_draw_string_clipped_shadowed(s, cx + (tw < col ? (col - tw) / 2 : 0), iy + px + 2, col,
                                          LABEL[k], 0xFFFFFF);
    }
}

// desktop.menu_spacing: the desktop's own right-click menu at the row
// height and padding uui_menubar gives the spacing.
static void menuspacing_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    static const struct { const char *label; int sub, tick, dim, hot; } ROW[] = {
        { "New", 1, 0, 0, 0 }, { "Paste", 0, 0, 1, 0 }, { "Refresh", 0, 0, 0, 1 },
        { "Sort by", 1, 0, 0, 0 }, { "Icon size", 1, 0, 0, 0 },
        { "Show desktop icons", 0, 1, 0, 0 }, { "System Settings", 0, 0, 0, 0 },
    };
    desktop_backdrop(s, &g_pv_menu, i, x, y, w, h);
    int row, air, ch = ugfx_char_h();
    uui_menu_spacing_metrics(choice_word(ctx, i), &row, &air);
    int mx = x + ch / 2, my = y + ch / 3, mw = w - ch, r = utheme_radius(UTHEME_R_POPUP);
    int n = 0;
    while (n < (int)(sizeof ROW / sizeof ROW[0]) && my + 2 * air + (n + 1) * row <= y + h) n++;
    int mh = 2 * air + n * row;
    uint32_t bg = uui_popup_bg(), fg = UTHEME_TEXT;
    for (int k = 3; k >= 1; k--)
        uui_glass_round_rect(s, mx + 2 - k, my + 3 - k, mw + 2 * k, mh + 2 * k, r + k, 0, 55, 0);
    uui_fill_round_rect(s, mx, my, mw, mh, r, bg);
    uui_stroke_round_rect(s, mx, my, mw, mh, r, uui_popup_border());
    int gutter = ch + ugfx_char_advance('n') * 2;
    for (int k = 0; k < n; k++) {
        int ry = my + air + k * row;
        uint32_t rbg = bg;
        if (ROW[k].hot) {
            rbg = uui_state_bg(bg, UUI_STATE_HOVER);
            uui_fill_round_rect(s, mx + air, ry, mw - 2 * air, row, air, rbg);
        }
        uint32_t ink = ROW[k].dim ? ugfx_blend(fg, bg, 170) : fg;
        int ty = ry + (row - ch) / 2;
        if (ROW[k].tick) {
            int u = ch / 4, tx = mx + air + ch / 2, cy = ry + row / 2;
            const int xs[] = { tx - u, tx, tx + 2 * u }, ys[] = { cy, cy + u, cy - 2 * u + 1 };
            ugfx_draw_polyline(s, xs, ys, 3, 2, UTHEME_ACCENT, GEOM_AA);
        }
        ugfx_draw_string_clipped(s, mx + air + gutter, ty, mw - 2 * air - gutter - ch, ROW[k].label, ink, rbg);
        if (ROW[k].sub) {
            int u = ch / 5 + 1, cx = mx + mw - air - ch, cy = ry + row / 2;
            const int xs[] = { cx, cx + u, cx }, ys[] = { cy - u, cy, cy + u };
            ugfx_draw_polyline(s, xs, ys, 3, 1, ugfx_blend(fg, bg, 120), GEOM_AA);
        }
    }
}

// desktop.corners: a window, a selected row, a button, a scrollbar and a
// menu at the preset's radii, in pixels; Custom at the five custom values.
static int corner_radius(const char *preset, enum utheme_round k) {
    static const char *const KEY[UTHEME_R_COUNT] = {
        "desktop.corners_controls", "desktop.corners_popups", "desktop.corners_windows",
        "desktop.corners_selection", "desktop.corners_scrollbars",
    };
    int r = utheme_radius_preset(preset, k);
    if (r != -2) return r;
    struct slot *sl = slot_find(KEY[k]);
    r = 0;
    if (sl) r = atoi(staged_value(sl));
    else if (!usetting_get_int(KEY[k], &r)) r = utheme_radius_preset("small", k);
    return r > 16 ? 16 : r < 0 ? 0 : r;
}

static void corners_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const char *p = choice_word(ctx, i);
    int rc = corner_radius(p, UTHEME_R_CONTROL), rp = corner_radius(p, UTHEME_R_POPUP);
    int rw = corner_radius(p, UTHEME_R_WINDOW), rs = corner_radius(p, UTHEME_R_SELECTION);
    int rb = corner_radius(p, UTHEME_R_SCROLLBAR);
    int ch = ugfx_char_h();
    uui_fill_round_rect(s, x, y, w, h, ch / 3, desktop_colour());
    // The window: its title in the title colour, its body the window's.
    int wx = x + ch / 2, wy = y + ch / 2, ww = w * 3 / 4, wh = h - ch;
    int tb = ch * 3 / 4;
    for (int k = 3; k >= 1; k--)
        uui_glass_round_rect(s, wx + 2 - k, wy + 3 - k, ww + 2 * k, wh + 2 * k, rw + k, 0, 55, 0);
    uui_fill_round_rect(s, wx, wy, ww, wh, rw, MINI_TITLE_ON);
    ugfx_fill_rect(s, wx, wy + tb, ww, wh - tb - rw, UTHEME_WINDOW_BG);
    uui_fill_round_rect(s, wx, wy + wh - 2 * rw - 1, ww, 2 * rw + 1, rw, UTHEME_WINDOW_BG);
    // A selected row, a line of text, a button and the scrollbar.
    uui_fill_round_rect(s, wx + ch / 3, wy + tb + ch / 3, ww * 2 / 3, ch * 3 / 4, rs, UTHEME_ACCENT);
    ugfx_fill_rect(s, wx + ch / 3, wy + tb + ch * 3 / 2, ww / 2, 3, UTHEME_BORDER);
    int bw = ch * 5 / 2, bh = ch, by = wy + wh - bh - ch / 3;
    uui_fill_round_rect(s, wx + ch / 3, by, bw, bh, rc, UTHEME_BUTTON_BG);
    uui_stroke_round_rect(s, wx + ch / 3, by, bw, bh, rc, UTHEME_BORDER);
    uui_fill_round_rect(s, wx + ww - 7, wy + tb + 3, 4, wh / 2, rb, UTHEME_BORDER);
    // The menu, over the window's corner.
    int pw = w / 2 - ch / 2, ph = h / 2, px = x + w - pw - ch / 3, py = y + h - ph - ch / 3;
    for (int k = 3; k >= 1; k--)
        uui_glass_round_rect(s, px + 2 - k, py + 3 - k, pw + 2 * k, ph + 2 * k, rp + k, 0, 55, 0);
    uui_fill_round_rect(s, px, py, pw, ph, rp, uui_popup_bg());
    uui_stroke_round_rect(s, px, py, pw, ph, rp, uui_popup_border());
    uui_fill_round_rect(s, px + 3, py + 4, pw - 6, ch * 2 / 3, rs, uui_state_bg(uui_popup_bg(), UUI_STATE_HOVER));
    for (int k = 1; k <= 2 && py + 4 + k * ch < py + ph - 3; k++)
        ugfx_fill_rect(s, px + ch / 2, py + 4 + k * ch, pw / 2, 3, UTHEME_BORDER);
}

// --- Appearance > Transparency's sample (mockup T1) ---------------------
//
// The taskbar, Start, a menu and a window over the wallpaper, each at the
// opacity and glass its own settings stage -- so a slider is seen moving
// before Apply. A frosted pane blurs what is under it at the compositor's
// radius (wm_glass.c); "wallpaper" has only wallpaper under it here, so
// it reads as frosted. The window is the FOCUSED one: "inactive" leaves it
// solid, which is what that choice does to it.

static uint32_t *g_glass_buf;   // a backdrop: the surface's size, its coordinates
static int g_glass_w, g_glass_h;

static int stage_pct(const char *name, int dflt) {
    char v[SETTING_ABI_VALUE_MAX];
    const char *s = page_value(name, v, sizeof v);
    int p = s[0] ? atoi(s) : dflt;
    return p < 1 ? dflt : p > 100 ? 100 : p;
}

static int stage_is(const char *name, const char *word) {
    char v[SETTING_ABI_VALUE_MAX];
    return !strcmp(page_value(name, v, sizeof v), word);
}

static void glass_pane(struct ugfx_surface *s, int x, int y, int w, int h, int r,
                       uint32_t tint, int alpha, int frosted) {
    if (alpha >= 255) { uui_fill_round_rect(s, x, y, w, h, r, tint); return; }
    const uint32_t *b = 0;
    if (frosted && g_glass_buf) {
        int br = ugfx_char_h() < 4 ? 4 : ugfx_char_h();
        ugfx_blur_rect(s, x, y, w, h, br, g_glass_buf);
        b = g_glass_buf;
    }
    uui_backdrop_round_rect(s, x, y, w, h, r, b, tint, (uint8_t)alpha);
}

void preview_glass(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (g_glass_w != s->w || g_glass_h != s->h) {
        free(g_glass_buf);
        g_glass_buf = malloc((size_t)s->w * (size_t)s->h * 4);
        g_glass_w = g_glass_buf ? s->w : 0;
        g_glass_h = g_glass_buf ? s->h : 0;
    }
    int on = stage_is("desktop.transparency", "on");
    static const char *const PCT[] = { "desktop.transparency_taskbar", "desktop.transparency_start",
                                       "desktop.transparency_menus",
                                       "desktop.transparency_window_opacity" };
    static const char *const KIND[] = { "desktop.transparency_taskbar_glass",
                                        "desktop.transparency_start_glass",
                                        "desktop.transparency_menus_glass",
                                        "desktop.transparency_window_glass" };
    static const int DFLT[] = { 75, 85, 85, 85 };   // wm_glass.c's
    int a[4], fr[4];
    for (int k = 0; k < 4; k++) {
        a[k] = on ? stage_pct(PCT[k], DFLT[k]) * 255 / 100 : 255;
        fr[k] = !stage_is(KIND[k], "clear");
    }
    int wins_title = on && (stage_is("desktop.transparency_windows", "titlebars") ||
                            stage_is("desktop.transparency_windows", "all"));
    int wins_body = on && stage_is("desktop.transparency_windows", "all");

    int ch = ugfx_char_h(), r = utheme_radius(UTHEME_R_POPUP);
    desktop_backdrop(s, &g_pv_glass, 0, x, y, w, h);
    int bar = ch * 2, top = y + ch / 2, room = h - bar - ch;

    // The window, behind the rest.
    int wx = x + w * 2 / 5, wy = top, ww = w / 2, wh = room * 4 / 5, tb = utheme_titlebar_h();
    int rw = utheme_radius(UTHEME_R_WINDOW);
    glass_pane(s, wx, wy, ww, tb + rw, rw, MINI_TITLE_ON, wins_title ? a[3] : 255, fr[3]);
    glass_pane(s, wx, wy + tb, ww, wh - tb, 0, UTHEME_WINDOW_BG, wins_body ? a[3] : 255, fr[3]);
    ugfx_draw_string_clipped(s, wx + ch / 2, wy + (tb - ch) / 2, ww - ch, "Notepad", 0xFFFFFF, UGFX_TRANSPARENT);
    ugfx_draw_string_clipped(s, wx + ch / 2, wy + tb + ch / 2, ww - ch, "Terve vaan!", UTHEME_TEXT, UGFX_TRANSPARENT);

    // Start, at the left, with a few of its apps.
    int sx = x + ch / 2, sw = w / 3, sh = room;
    glass_pane(s, sx, top, sw, sh, r, MINI_TASKBAR, a[1], fr[1]);
    ugfx_draw_string_clipped(s, sx + ch / 2, top + ch / 2, sw - ch, "Start", 0xFFFFFF, UGFX_TRANSPARENT);
    static const char *const APPS[] = { "files", "notepad", "calculator", "browser", "charmap", "diskmark" };
    int ip = ch * 2, gap = (sw - 3 * ip) / 4;
    for (int k = 0; k < 6; k++) {
        const struct uimg *ic = icon_get(APPS[k], ip);
        int ix = sx + gap + (k % 3) * (ip + gap), iy = top + ch * 2 + (k / 3) * (ip + ch / 2);
        if (ic && iy + ip < top + sh) ugfx_blit_alpha(s, ix, iy, ic->w, ic->h, ic->px, ic->w);
    }

    // A menu over the window's edge.
    int row, air;
    uui_menu_spacing_metrics(0, &row, &air);
    int mw = ch * 8, mx = wx - mw / 3, my = top + room * 3 / 5, mh = 2 * air + 3 * row;
    if (my + mh > y + h - bar) my = y + h - bar - mh - 2;
    glass_pane(s, mx, my, mw, mh, r, uui_popup_bg(), a[2], fr[2]);
    static const char *const ROWS[] = { "New", "Refresh", "Sort by" };
    for (int k = 0; k < 3; k++)
        ugfx_draw_string_clipped(s, mx + air + ch, my + air + k * row + (row - ch) / 2, mw - 2 * air - ch,
                                 ROWS[k], UTHEME_TEXT, UGFX_TRANSPARENT);

    // The taskbar along the bottom.
    glass_pane(s, x, y + h - bar, w, bar, 0, MINI_TASKBAR, a[0], fr[0]);
    ugfx_draw_string(s, x + ch / 2, y + h - bar + (bar - ch) / 2, "Start", 0xFFFFFF, UGFX_TRANSPARENT);
    uapp_logf_layout("settings: glass on %d taskbar %d start %d menus %d window %d\n",
                     on, a[0], a[1], a[2], a[3]);
}

// --- the table -----------------------------------------------------------

static const struct {
    const char *word;
    void (*paint)(struct ugfx_surface *, int, int, int, int, int, void *);
} PAINTERS[] = {
    { "cursor", cursor_tile },
    { "picture", picture_tile },
    { "live", live_tile },
    { "colour", colour_tile },
    { "saver", saver_tile },
    { "font", font_tile },
    { "fontmono", fontmono_tile },
    { "winmove", winmove_tile },
    { "winresize", winresize_tile },
    { "shadow", shadow_tile },
    { "seethrough", seethrough_tile },
    { "theme", theme_tile },
    { "placement", placement_tile },
    { "iconsize", iconsize_tile },
    { "menuspacing", menuspacing_tile },
    { "corners", corners_tile },
};

void preview_attach(struct slot *sl, int idx) {
    sl->gallery.draw_tile = 0;
    sl->gallery.ctx = sl;
    // g_name is "<ns>.<name>"; the text file is named the same way.
    const char *name = g_name[idx];
    size_t nl = strlen(g_ns[idx]);
    if (nl && !strncmp(name, g_ns[idx], nl) && name[nl] == '.') name += nl + 1;
    char word[16];
    if (!uschema_text_word(g_ns[idx], name, SETTING_TEXT_KEY_PREVIEW, word, sizeof word)) return;
    for (unsigned p = 0; p < sizeof PAINTERS / sizeof PAINTERS[0]; p++)
        if (!strcmp(word, PAINTERS[p].word)) sl->gallery.draw_tile = PAINTERS[p].paint;
    // Seven savers on a page half as wide as the wallpapers': smaller
    // cards, four across, under the monitor that shows the chosen one big.
    if (!strcmp(word, "saver")) sl->gallery.min_w = ugfx_char_h() * 8;
    // A theme is a window: four across keeps a card near the screen's
    // shape, where two stretched one into a strip.
    if (!strcmp(word, "theme")) sl->gallery.min_w = ugfx_char_h() * 8;
    if (!strcmp(word, "font") || !strcmp(word, "fontmono")) font_labels(sl);
    // Real sizes: a 64 px icon and seven rows of a menu do not fit the
    // default tile. Corners: five across, the preset row and Custom.
    int ch = ugfx_char_h();
    if (!strcmp(word, "iconsize")) sl->gallery.tile_h = ICONS_TILE_H;
    if (!strcmp(word, "menuspacing")) sl->gallery.tile_h = ch * 9;
    if (!strcmp(word, "corners")) { sl->gallery.min_w = ch * 8; sl->gallery.tile_h = ch * 6; }
    if (!strcmp(word, "placement")) {
        struct query_display d;
        memset(&d, 0, sizeof d);
        if (sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d) > 0 && d.width && d.height) {
            g_scr_w = (int)d.width;
            g_scr_h = (int)d.height;
        }
        sl->gallery.tile_h = ch * 6;
    }
}
