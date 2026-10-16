// The lock's connection card. See ns_card.h.
#include <stdio.h>
#include <string.h>

#include "browser/ns_card.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include "win_proto.h"   // WIN_CURSOR_*

// One line of the card: a caption in the left column and its value, or
// a value alone across the card (caption NULL).
struct line {
    const char *caption;
    char text[160];
    uint32_t color;
    int gap_before;   // a separator above it
};
#define LINES_MAX 16

static const char *const MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void date(char *out, size_t cap, int y, int m, int d) {
    snprintf(out, cap, "%d %s %d", d, m >= 1 && m <= 12 ? MONTHS[m - 1] : "?", y);
}

static const char *state_line(int state) {
    switch (state) {
    case NS_SITE_SECURE:   return "Connection is secure";
    case NS_SITE_ISSUES:   return "Parts of this page are not secure";
    case NS_SITE_OVERRIDE: return "Not secure: its certificate was not accepted";
    case NS_SITE_INSECURE: return "Connection is not secure";
    case NS_SITE_FILE:     return "A file on this computer";
    case NS_SITE_INTERNAL: return "A page of the Web Browser's own";
    }
    return "Nothing loaded";
}

static int https(int state) {
    return state == NS_SITE_SECURE || state == NS_SITE_ISSUES || state == NS_SITE_OVERRIDE;
}

static int build(const struct ns_card *c, struct line *L) {
    const struct ns_site *s = &c->site;
    uint32_t muted = utheme_current()->text_muted;
    uint32_t good = utheme_action(UTHEME_ACT_CREATE), bad = utheme_action(UTHEME_ACT_DANGER);
    int n = 0;
    memset(L, 0, sizeof(struct line) * LINES_MAX);
    snprintf(L[n].text, sizeof L[n].text, "%s", s->host[0] ? s->host : "This page");
    L[n++].color = UTHEME_TEXT;
    snprintf(L[n].text, sizeof L[n].text, "%s", state_line(s->state));
    L[n++].color = s->state == NS_SITE_SECURE ? good
                 : (s->state == NS_SITE_FILE || s->state == NS_SITE_INTERNAL) ? muted : bad;
    if (s->state == NS_SITE_INSECURE) {
        snprintf(L[n].text, sizeof L[n].text, "What you send or receive here can be read on the way.");
        L[n++].color = muted;
    }
    if (https(s->state)) {
        L[n].gap_before = 1;
        L[n].caption = "Protocol";
        snprintf(L[n].text, sizeof L[n].text, "%s", s->version[0] ? s->version : "Not recorded");
        L[n++].color = UTHEME_TEXT;
        L[n].caption = "Cipher";
        snprintf(L[n].text, sizeof L[n].text, "%s", s->cipher[0] ? s->cipher : "Not recorded");
        L[n++].color = UTHEME_TEXT;
        if (s->have_leaf) {
            const struct utls_cert_info *i = &s->leaf;
            char from[24], to[24];
            L[n].gap_before = 1;
            L[n].caption = "Issued to";
            snprintf(L[n].text, sizeof L[n].text, "%s", i->subject_cn[0] ? i->subject_cn : "(no name)");
            L[n++].color = UTHEME_TEXT;
            if (i->subject_org[0]) {
                L[n].caption = "";
                snprintf(L[n].text, sizeof L[n].text, "%s", i->subject_org);
                L[n++].color = muted;
            }
            L[n].caption = "Issued by";
            snprintf(L[n].text, sizeof L[n].text, "%s", i->issuer_cn[0] ? i->issuer_cn : i->issuer_org);
            L[n++].color = UTHEME_TEXT;
            date(from, sizeof from, i->not_before.year, i->not_before.mon, i->not_before.day);
            date(to, sizeof to, i->not_after.year, i->not_after.mon, i->not_after.day);
            L[n].caption = "Valid";
            snprintf(L[n].text, sizeof L[n].text, "%s - %s", from, to);
            L[n++].color = UTHEME_TEXT;
            L[n].caption = "Chain";
            snprintf(L[n].text, sizeof L[n].text, "%d certificate%s", s->chain_depth,
                     s->chain_depth == 1 ? "" : "s");
            L[n++].color = UTHEME_TEXT;
        }
    }
    if (s->state != NS_SITE_FILE && s->state != NS_SITE_INTERNAL && s->state != NS_SITE_NONE) {
        L[n].gap_before = 1;
        // Chrome's "Cookies and site data": what Clear deletes.
        L[n].caption = "Site data";
        char cookies[32] = "", logins[32] = "";
        if (s->cookies) snprintf(cookies, sizeof cookies, "%d cookie%s", s->cookies, s->cookies == 1 ? "" : "s");
        if (s->logins == 1) snprintf(logins, sizeof logins, "a sign-in");
        else if (s->logins) snprintf(logins, sizeof logins, "%d sign-ins", s->logins);
        if (cookies[0] && logins[0]) snprintf(L[n].text, sizeof L[n].text, "%s and %s", cookies, logins);
        else if (cookies[0] || logins[0]) snprintf(L[n].text, sizeof L[n].text, "%s", cookies[0] ? cookies : logins);
        else snprintf(L[n].text, sizeof L[n].text, "None stored for this site");
        if (!cookies[0] && logins[0]) L[n].text[0] = 'A';   // a sentence's capital
        L[n++].color = UTHEME_TEXT;
    }
    return n;
}

static int line_h(void) { return ugfx_char_h() + 6; }
static int pad(void) { return ugfx_char_advance('n') + 4; }
static int caption_w(void) { return ugfx_text_width("Issued by") + ugfx_char_advance('n') * 2; }

// The card's lines, built per draw: static, as a frame of them is past
// ring 3's stack-frame budget, and only the UI thread draws.
static struct line g_lines[LINES_MAX];

// A web page's: what runs on it is the site's to choose.
static int web_page(const struct ns_site *s) {
    return s->host[0] && s->state != NS_SITE_FILE && s->state != NS_SITE_INTERNAL &&
           s->state != NS_SITE_NONE;
}

static void layout(struct ns_card *c, int ax, int ay, int bw, int bh) {
    struct line *L = g_lines;
    int n = build(c, L), w = 0, h = 2 * pad();
    for (int i = 0; i < n; i++) {
        int lw = ugfx_text_width(L[i].text) + (L[i].caption ? caption_w() : 0);
        if (lw > w) w = lw;
        h += line_h() + (L[i].gap_before ? pad() : 0);
    }
    int lines_h = h, rw = 0, rh = 0;
    if (c->has_js) {
        uui_siteperm_natural_size(&c->js, caption_w(), &rw, &rh);
        if (rw > w) w = rw;
        h += pad() + rh;
    }
    w += 2 * pad();
    int min = ugfx_char_advance('n') * 34;
    if (w < min) w = min;
    if (w > bw - 8) w = bw - 8;
    c->clear_w = 0;
    if (L[n - 1].caption && !strcmp(L[n - 1].caption, "Site data") && (c->site.cookies || c->site.logins)) {
        c->clear_w = ugfx_text_width("Clear") + 2 * pad();
        c->clear_h = line_h();
    }
    c->w = w;
    c->h = h;
    c->x = ax + w > bw - 4 ? bw - 4 - w : ax;
    c->y = ay + h > bh - 4 ? bh - 4 - h : ay;
    if (c->x < 4) c->x = 4;
    if (c->y < 4) c->y = 4;
    c->clear_x = c->x + w - pad() - c->clear_w;
    c->clear_y = c->y + lines_h - pad() - line_h();
    if (c->has_js) uui_siteperm_set_geometry(&c->js, c->x + pad(), c->y + lines_h, caption_w());
}

void ns_card_open(struct ns_card *c, const struct ns_site *site, int ax, int ay, int bw, int bh) {
    c->site = *site;
    c->open = 1;
    c->armed = c->hot = 0;
    c->has_js = web_page(site);
    if (c->has_js) uui_siteperm_init(&c->js, "JavaScript", site->js_perm, site->js_default_on);
    layout(c, ax, ay, bw, bh);
}

int ns_card_js(const struct ns_card *c) { return uui_siteperm_value(&c->js); }

void ns_card_close(struct ns_card *c) {
    if (c->open) c->event = NS_CARD_CLOSED;
    c->open = 0;
}

int ns_card_take(struct ns_card *c) {
    int e = c->event;
    c->event = NS_CARD_NONE;
    return e;
}

static void c_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = 0;
    *oh = 0;
}

// Placed by ns_card_open(), never by a layout.
static void c_geometry(void *w, int x, int y, int width, int height) {
    (void)w; (void)x; (void)y; (void)width; (void)height;
}

static void c_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct ns_card *c = w;
    *x = c->x;
    *y = c->y;
    *ow = c->open ? c->w : 0;
    *oh = c->open ? c->h : 0;
}

static void c_draw(struct ugfx_surface *s, const void *w) {
    const struct ns_card *c = w;
    if (!c->open) return;
    int r = uui_popup_radius();
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, r, uui_popup_border());
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, r > 1 ? r - 1 : 0, uui_popup_bg());
    struct line *L = g_lines;
    int n = build(c, L), y = c->y + pad(), x = c->x + pad();
    int max = c->w - 2 * pad();
    for (int i = 0; i < n; i++) {
        if (L[i].gap_before) {
            ugfx_fill_rect(s, c->x + 1, y + pad() / 2 - 1, c->w - 2, 1, UTHEME_SEPARATOR);
            y += pad();
        }
        int ty = y + 3;
        if (L[i].caption) {
            ugfx_draw_string_clipped(s, x, ty, caption_w(), L[i].caption, utheme_current()->text_muted,
                                     uui_popup_bg());
            int room = max - caption_w() - (c->clear_w && i == n - 1 ? c->clear_w + pad() : 0);
            ugfx_draw_string_elided(s, x + caption_w(), ty, room, L[i].text, L[i].color, uui_popup_bg());
        } else {
            ugfx_draw_string_elided(s, x, ty, max, L[i].text, L[i].color, uui_popup_bg());
        }
        y += line_h();
    }
    if (c->has_js) {
        ugfx_fill_rect(s, c->x + 1, c->js.y - pad() / 2 - 1, c->w - 2, 1, UTHEME_SEPARATOR);
        uui_siteperm_draw(s, &c->js, uui_popup_bg());
    }
    if (c->clear_w) {
        uint32_t bg = c->armed == 1 ? uui_state_bg(UTHEME_BUTTON_BG, UUI_STATE_PRESSED)
                    : c->hot ? uui_state_bg(UTHEME_BUTTON_BG, UUI_STATE_HOVER) : UTHEME_BUTTON_BG;
        uui_fill_round_rect(s, c->clear_x, c->clear_y, c->clear_w, c->clear_h, uui_control_radius(4), bg);
        ugfx_draw_string(s, c->clear_x + pad(), c->clear_y + 3, "Clear", UTHEME_TEXT, bg);
    }
}

static int on_clear(const struct ns_card *c, int cx, int cy) {
    return c->clear_w && uui_hit(c->clear_x, c->clear_y, c->clear_w, c->clear_h, cx, cy);
}

static int c_hit(const void *w, int cx, int cy) {
    const struct ns_card *c = w;
    return c->open && uui_hit(c->x, c->y, c->w, c->h, cx, cy);
}

// Offered EVERY press while open (overlay_active). One outside closes
// the card and is declined, so it reaches what is under it -- a click on
// a link while the card is up follows the link.
static int c_press(void *w, int cx, int cy, unsigned mods) {
    struct ns_card *c = w;
    if (!c->open) return 0;
    if (!c_hit(c, cx, cy)) {
        ns_card_close(c);
        return 0;
    }
    c->armed = on_clear(c, cx, cy);
    if (!c->armed && c->has_js && uui_siteperm_ops.press(&c->js, cx, cy, mods)) c->armed = 2;
    return 1;
}

static int c_motion(void *w, int cx, int cy, unsigned buttons) {
    struct ns_card *c = w;
    int moved = c->open && c->has_js && uui_siteperm_ops.motion(&c->js, cx, cy, buttons);
    int hot = c->open && on_clear(c, cx, cy);
    if (hot == c->hot) return moved;
    c->hot = hot;
    return 1;
}

static int c_release(void *w, int cx, int cy) {
    struct ns_card *c = w;
    if (c->armed == 1 && on_clear(c, cx, cy)) c->event = NS_CARD_CLEAR;
    if (c->armed == 2) {
        int was = uui_siteperm_value(&c->js);
        uui_siteperm_ops.release(&c->js, cx, cy);
        if (uui_siteperm_value(&c->js) != was) c->event = NS_CARD_JS;
    }
    c->armed = 0;
    return 1;
}

static int c_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct ns_card *c = w;
    if (!c->open || key != 0x1B) return 0;
    ns_card_close(c);
    return 1;
}

static int c_accepts_focus(const void *w) { (void)w; return 0; }
static int c_overlay(const void *w) { return ((const struct ns_card *)w)->open; }

static int c_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

static void c_describe(const void *w, const struct uui_describe *d) {
    const struct ns_card *c = w;
    uui_describe_int(d, "open", c->open);
    if (c->open && c->clear_w) uui_describe_rect(d, "clear", c->clear_x, c->clear_y, c->clear_w, c->clear_h);
    if (c->open && c->has_js) {   // "card.js.slot i ..." and "card.js.selected"
        char name[48];
        snprintf(name, sizeof name, "%s.js", d->name);
        struct uui_describe row = *d;
        row.name = name;
        uui_siteperm_ops.describe(&c->js, &row);
    }
}

const struct uui_widget_ops ns_card_ops = {
    .natural_size   = c_natural,
    .set_geometry   = c_geometry,
    .bounds         = c_bounds,
    .draw           = c_draw,
    .hit            = c_hit,
    .press          = c_press,
    .motion         = c_motion,
    .release        = c_release,
    .key            = c_key,
    .accepts_focus  = c_accepts_focus,
    .overlay_active = c_overlay,
    .cursor         = c_cursor,
    .describe       = c_describe,
};

void ns_site_from(struct ns_site *out, const struct wv_site *in, int cookies, int logins) {
    memset(out, 0, sizeof *out);
    out->state = in->state;
    snprintf(out->host, sizeof out->host, "%s", in->host);
    snprintf(out->version, sizeof out->version, "%s", in->version);
    snprintf(out->cipher, sizeof out->cipher, "%s", in->cipher);
    out->chain_depth = in->chain_depth;
    out->have_leaf = in->have_leaf;
    const struct wv_cert *c = &in->leaf;
    snprintf(out->leaf.subject_cn, sizeof out->leaf.subject_cn, "%s", c->subject_cn);
    snprintf(out->leaf.subject_org, sizeof out->leaf.subject_org, "%s", c->subject_org);
    snprintf(out->leaf.issuer_cn, sizeof out->leaf.issuer_cn, "%s", c->issuer_cn);
    snprintf(out->leaf.issuer_org, sizeof out->leaf.issuer_org, "%s", c->issuer_org);
    out->leaf.not_before.year = c->nb_year, out->leaf.not_before.mon = c->nb_mon;
    out->leaf.not_before.day = c->nb_day;
    out->leaf.not_after.year = c->na_year, out->leaf.not_after.mon = c->na_mon;
    out->leaf.not_after.day = c->na_day;
    out->cookies = cookies < 0 ? 0 : cookies;
    out->logins = logins < 0 ? 0 : logins;
    out->js = in->js;
    out->scripts = in->scripts;
}
