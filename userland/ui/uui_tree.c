// tree -- see ui/uui_tree.h for the contract and why the nodes are the
// app's flat const array.
#include "ui/uui_tree.h"
#include "ui/uui_route.h" // UUI_NOWHERE -- a drag_over's leave
#include "ui/uui_widget.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include "lib/icon_cache.h" // icon_get() -- a node may carry an icon
#include <stdio.h>
#include <string.h>

#define UUI_TREE_PAD_X   4  // left inset before the first expander
#define UUI_TREE_INDENT  12 // per depth level
#define UUI_TREE_EXP_W   10 // the expander's hit width

void uui_tree_init(struct uui_tree *t, int x, int y, int w, int h,
                    const struct uui_tree_node *nodes, int count) {
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->nodes = nodes;
    t->count = count;
    t->selected = count > 0 ? 0 : -1;
    t->hovered = -1;
    t->drop_node = -1;
    t->dropped_node = -1;
    t->can_drag = 0;
    t->can_insert = 0;
    t->reorder_ctx = 0;
    t->press_node = t->pre_press_sel = -1;
    t->drag_node = -1;
    t->action_hot = t->action_press = t->acted = -1;
    t->ins_y = t->ins_after = t->ins_before = -1;
    t->dropped_from = t->dropped_after = t->dropped_before = -1;
    t->dropped_insert = 0;
    t->drag_label[0] = '\0';
    t->top = 0;
    t->row_h = 0; // derive from the font
    uui_sbar_init(&t->sb);
    uui_scrollanim_init(&t->anim);
    t->collapsed = 0; // EXPANDED by default -- see the header
    t->on_toggle = 0;
    t->toggle_ctx = 0;
    // The theme's, resolved at draw (ui/utheme.h's UUI_COLOR).
    t->bg = t->fg = t->sel_bg = t->sel_fg = t->guide = UUI_COLOR_UNSET;
    t->sel_style = UUI_SEL_SOFT;
}

// The ground when the app names none: a step lighter than the page.
static uint32_t tree_bg(const struct uui_tree *t) {
    return UUI_COLOR(t->bg, ugfx_blend(UTHEME_PANEL_BG, UTHEME_WHITE, 141));
}

// --- the filter -----------------------------------------------------------

#define FILTER_HIT_BG (utheme_current()->find_hit)   // Notepad's search hit

static int is_header(const struct uui_tree *t, int node);
static int node_hidden(const struct uui_tree *t, int node);
static void tree_clamp(struct uui_tree *t);

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

// Where `needle` starts in `hay`, ignoring case, or -1.
static int find_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return -1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] && lower((unsigned char)hay[i + k]) == lower((unsigned char)needle[k])) k++;
        if (!needle[k]) return i;
    }
    return -1;
}

int uui_tree_text_matches(const char *haystack, const char *needle) {
    return find_ci(haystack, needle) >= 0;
}

static void set_shown(struct uui_tree *t, int node) {
    if (node >= 0 && node < UUI_TREE_FILTER_MAX) t->filter_shown[node / 64] |= 1ull << (node % 64);
}

// A matching node shows, and so does every ancestor of it (the nearest
// shallower node before it, walking back) -- the path to the match --
// and everything under it: "Sound" finds the sound devices, not a
// heading with nothing beneath it.
static int compute_filter(struct uui_tree *t) {
    memset(t->filter_shown, 0, sizeof t->filter_shown);
    if (!t->filter[0]) return 0;
    int matches = 0;
    for (int i = 0; i < t->count && i < UUI_TREE_FILTER_MAX; i++) {
        if (is_header(t, i)) continue;
        int hit = t->filter_match ? t->filter_match(t->filter_ctx, i, t->filter)
                                  : find_ci(t->nodes[i].label, t->filter) >= 0;
        if (!hit) continue;
        matches++;
        set_shown(t, i);
        for (int k = i + 1; k < t->count && t->nodes[k].depth > t->nodes[i].depth; k++)
            set_shown(t, k);
        int d = t->nodes[i].depth;
        for (int a = i - 1; a >= 0 && d > 0; a--) {
            if (t->nodes[a].depth < d) { set_shown(t, a); d = t->nodes[a].depth; }
        }
    }
    // A section heading shows when anything under it does.
    for (int i = 0; i < t->count && i < UUI_TREE_FILTER_MAX; i++) {
        if (!is_header(t, i)) continue;
        for (int k = i + 1; k < t->count && k < UUI_TREE_FILTER_MAX && !is_header(t, k); k++)
            if ((t->filter_shown[k / 64] >> (k % 64)) & 1u) { set_shown(t, i); break; }
    }
    return matches;
}

int uui_tree_set_filter(struct uui_tree *t, const char *text,
                        int (*match)(void *ctx, int node, const char *text), void *ctx) {
    snprintf(t->filter, sizeof t->filter, "%s", text ? text : "");
    t->filter_match = match;
    t->filter_ctx = ctx;
    int n = compute_filter(t);
    t->top = 0;
    t->hovered = -1;
    // A selection the filter hides moves to the first match, so what the
    // page beside the tree shows is something the tree still shows.
    if (t->filter[0] && (t->selected < 0 || node_hidden(t, t->selected))) {
        t->selected = -1;
        for (int i = 0; i < t->count; i++)
            if (!node_hidden(t, i) && !is_header(t, i) && !uui_tree_is_parent(t, i)) { t->selected = i; break; }
    }
    tree_clamp(t);
    return n;
}

void uui_tree_set_nodes(struct uui_tree *t, const struct uui_tree_node *nodes, int count) {
    t->nodes = nodes;
    t->count = count;
    if (t->selected >= count) t->selected = count > 0 ? count - 1 : -1;
    t->hovered = -1;
    t->drop_node = -1;
    t->top = 0;
    // Collapsed state is DROPPED with the nodes it described: bit 3 of
    // an old tree means nothing about a new one, and keeping it would
    // silently hide an unrelated row. The filter is KEPT and re-run.
    t->collapsed = 0;
    compute_filter(t);
}

static void tree_clamp(struct uui_tree *t);

void uui_tree_set_nodes_keep(struct uui_tree *t, const struct uui_tree_node *nodes,
                              int count) {
    int top = t->top;
    uui_tree_set_nodes(t, nodes, count);
    t->top = top;
    tree_clamp(t);
}

void uui_tree_set_on_toggle(struct uui_tree *t,
                             void (*fn)(void *ctx, int id, int expand), void *ctx) {
    t->on_toggle = fn;
    t->toggle_ctx = ctx;
}

// --- structure, derived from the depth run (or declared -- see `kind`) --

static int is_header(const struct uui_tree *t, int node) {
    return node >= 0 && node < t->count && t->nodes[node].kind == UUI_TREE_HEADER;
}

int uui_tree_is_parent(const struct uui_tree *t, int node) {
    if (node < 0 || node >= t->count) return 0;
    if (is_header(t, node)) return 0;
    if (t->nodes[node].kind != UUI_TREE_AUTO) return 1; // declared lazy parent
    if (node >= t->count - 1) return 0;
    return t->nodes[node + 1].depth > t->nodes[node].depth;
}

int uui_tree_is_collapsed(const struct uui_tree *t, int node) {
    if (node < 0 || node >= t->count || is_header(t, node)) return 0;
    if (t->filter[0]) return 0;   // a filtered tree is fully open
    if (t->nodes[node].kind != UUI_TREE_AUTO)
        return t->nodes[node].kind == UUI_TREE_CLOSED;
    if (node >= UUI_TREE_MAX_NODES) return 0;
    return (t->collapsed >> node) & 1u;
}

// Is `node` hidden by a COLLAPSED ANCESTOR? Walks forward tracking the
// shallowest collapsed depth still in effect, rather than searching
// backwards per node -- the same single pass every other loop here uses,
// so they cannot disagree about what is visible.
static int node_hidden(const struct uui_tree *t, int node) {
    if (t->filter[0])
        return node < UUI_TREE_FILTER_MAX && !((t->filter_shown[node / 64] >> (node % 64)) & 1u);
    int hide_below = -1; // depth at or under which everything is hidden
    for (int i = 0; i <= node && i < t->count; i++) {
        int d = t->nodes[i].depth;
        if (hide_below >= 0 && d <= hide_below) hide_below = -1; // out of it
        if (i == node) return hide_below >= 0;
        if (hide_below < 0 && uui_tree_is_parent(t, i) && uui_tree_is_collapsed(t, i))
            hide_below = d;
    }
    return 0;
}

int uui_tree_visible_count(const struct uui_tree *t) {
    int n = 0;
    for (int i = 0; i < t->count; i++) if (!node_hidden(t, i)) n++;
    return n;
}

int uui_tree_node_at_row(const struct uui_tree *t, int row) {
    if (row < 0) return -1;
    int n = 0;
    for (int i = 0; i < t->count; i++) {
        if (node_hidden(t, i)) continue;
        if (n == row) return i;
        n++;
    }
    return -1;
}

// The visible ROW a node sits on, or -1 when it is hidden.
static int row_of_node(const struct uui_tree *t, int node) {
    int n = 0;
    for (int i = 0; i < t->count; i++) {
        if (node_hidden(t, i)) continue;
        if (i == node) return n;
        n++;
    }
    return -1;
}

int uui_tree_set_collapsed(struct uui_tree *t, int node, int collapsed) {
    if (!uui_tree_is_parent(t, node)) return 0; // a leaf: no-op, not an error
    if (t->nodes[node].kind != UUI_TREE_AUTO) {
        // The APP owns this node's state: report the request and flip
        // nothing -- the rebuilt array's `kind` is what changes it.
        // NOTE the callback may call uui_tree_set_nodes_keep() before
        // this returns, so `node` must not be used after it.
        if (uui_tree_is_collapsed(t, node) == !!collapsed) return 0;
        if (!t->on_toggle) return 0;
        t->on_toggle(t->toggle_ctx, t->nodes[node].id, !collapsed);
        return 1;
    }
    if (node >= UUI_TREE_MAX_NODES) return 0;
    int was = uui_tree_is_collapsed(t, node);
    if (was == !!collapsed) return 0;
    if (collapsed) t->collapsed |= (uint64_t)1 << node;
    else t->collapsed &= ~((uint64_t)1 << node);
    // Collapsing over the selection would leave a selected row nobody
    // can see, and an app reading uui_tree_selected_id() would keep
    // getting an id whose row is gone. Move it to the parent, which is
    // what every desktop tree does.
    if (collapsed && t->selected >= 0 && node_hidden(t, t->selected))
        t->selected = node;
    return 1;
}

void uui_tree_expand_all(struct uui_tree *t) { t->collapsed = 0; }

void uui_tree_collapse_all(struct uui_tree *t) {
    for (int i = 0; i < t->count && i < UUI_TREE_MAX_NODES; i++)
        if (uui_tree_is_parent(t, i)) t->collapsed |= (uint64_t)1 << i;
    if (t->selected >= 0 && node_hidden(t, t->selected)) {
        // Same rule as above, applied to whatever ancestor survives.
        for (int i = t->selected; i >= 0; i--)
            if (!node_hidden(t, i)) { t->selected = i; break; }
    }
}

// --- ids --------------------------------------------------------------

int uui_tree_selected_id(const struct uui_tree *t) {
    if (t->selected < 0 || t->selected >= t->count) return -1;
    return t->nodes[t->selected].id;
}

int uui_tree_select_id(struct uui_tree *t, int id) {
    for (int i = 0; i < t->count; i++) {
        if (t->nodes[i].id != id) continue;
        if (t->selected == i && !node_hidden(t, i)) return 0;
        // EXPAND WHATEVER IS HIDING IT. Selecting a node and leaving it
        // invisible looks to a user exactly like the call did nothing.
        while (node_hidden(t, i)) {
            for (int j = i - 1; j >= 0; j--) {
                if (t->nodes[j].depth < t->nodes[i].depth &&
                    uui_tree_is_collapsed(t, j)) {
                    uui_tree_set_collapsed(t, j, 0);
                    break;
                }
            }
        }
        t->selected = i;
        // INTO VIEW, as every tree's ensureVisible does: a selection
        // the widget made and then scrolled away from is one the user
        // cannot see moved.
        int row = row_of_node(t, i);
        int vis = uui_tree_visible_rows(t);
        if (row >= 0 && vis > 0) {
            if (row < t->top) t->top = row;
            else if (row >= t->top + vis) t->top = row - vis + 1;
            tree_clamp(t);
        }
        return 1;
    }
    return 0;
}

int uui_tree_drop_id(const struct uui_tree *t) {
    if (t->dropped_insert) return -1;   // a GAP, not a row: uui_tree_drop_insert()
    int n = t->dropped_node;
    return (n >= 0 && n < t->count) ? t->nodes[n].id : -1;
}

void uui_tree_set_reorder(struct uui_tree *t, int (*can_drag)(void *ctx, int id),
                          int (*can_insert)(void *ctx, int from, int after, int before),
                          void *ctx) {
    t->can_drag = can_drag;
    t->can_insert = can_insert;
    t->reorder_ctx = ctx;
}

int uui_tree_drop_insert(const struct uui_tree *t, int *from, int *after, int *before) {
    if (!t->dropped_insert) return 0;
    if (from) *from = t->dropped_from;
    if (after) *after = t->dropped_after;
    if (before) *before = t->dropped_before;
    return 1;
}

static int id_of(const struct uui_tree *t, int node) {
    return node >= 0 && node < t->count ? t->nodes[node].id : -1;
}

// --- geometry ---------------------------------------------------------

int uui_tree_row_h(const struct uui_tree *t) {
    return t->row_h > 0 ? t->row_h : ugfx_char_h() + 6;
}

int uui_tree_visible_rows(const struct uui_tree *t) {
    int rh = uui_tree_row_h(t);
    int n = rh > 0 ? t->h / rh : 0;
    return n > 0 ? n : 1;
}

int uui_tree_scrollbar_visible(const struct uui_tree *t) {
    return uui_tree_visible_count(t) > uui_tree_visible_rows(t);
}

static int tree_bar_x(const struct uui_tree *t) { return t->x + t->w - uui_sbar_width(); }

// The bar in ROWS, filled in before anything asks it.
static struct uui_sbar *tree_sbar(struct uui_tree *t) {
    uui_sbar_place(&t->sb, tree_bar_x(t), t->y, t->h);
    uui_sbar_set(&t->sb, uui_tree_visible_count(t), uui_tree_visible_rows(t), t->top);
    return &t->sb;
}

static void tree_clamp(struct uui_tree *t) {
    int vis = uui_tree_visible_rows(t);
    int total = uui_tree_visible_count(t);
    int max_top = total - vis;
    if (max_top < 0) max_top = 0;
    if (t->top > max_top) t->top = max_top;
    if (t->top < 0) t->top = 0;
}

// The icon column: the text's height plus a gap, or 0 in a tree where no
// node has an icon -- so an existing tree draws exactly as it did.
static int icon_gutter(const struct uui_tree *t) {
    for (int i = 0; i < t->count; i++)
        if (t->nodes[i].icon || t->nodes[i].badge) return ugfx_char_h() + 4;
    return 0;
}

// The widget WANTS room for its deepest, longest row -- measured with
// every node counted, collapsed or not, because natural size must not
// depend on the widget's current state any more than on where it is.
// A tree that shrank when collapsed would make a layout twitch as the
// user clicked. See CLAUDE.md on natural_size.
void uui_tree_natural_size(const struct uui_tree *t, int *out_w, int *out_h) {
    int widest = 0, gutter = icon_gutter(t);
    for (int i = 0; i < t->count; i++) {
        int w = t->nodes[i].depth * UUI_TREE_INDENT + UUI_TREE_EXP_W + gutter +
                ugfx_text_width(t->nodes[i].label);
        if (w > widest) widest = w;
    }
    if (out_w) *out_w = UUI_TREE_PAD_X * 2 + widest + uui_sbar_width();
    if (out_h) *out_h = uui_tree_row_h(t) * (t->count > 0 ? t->count : 1);
}

// --- drawing ----------------------------------------------------------

// A usage meter's width: four digits' worth, from the face.
static int meter_w(void) { return ugfx_char_advance('0') * 4; }

static int row_text_x(const struct uui_tree *t, int node) {
    return t->x + UUI_TREE_PAD_X + t->nodes[node].depth * UUI_TREE_INDENT +
           UUI_TREE_EXP_W + icon_gutter(t);
}

// A CHEVRON, two pixels thick: right when collapsed, down when expanded
// -- Windows 11's and Plasma's disclosure mark, in a muted colour so the
// labels lead. Drawn, not spelled, so it does not depend on the font.
static void draw_expander(struct ugfx_surface *s, int cx, int cy, int open,
                           uint32_t col) {
    for (int i = 0; i < 4; i++) {
        if (open) {
            ugfx_fill_rect(s, cx - 3 + i, cy - 1 + i, 2, 1, col);
            ugfx_fill_rect(s, cx + 2 - i, cy - 1 + i, 2, 1, col);
        } else {
            ugfx_fill_rect(s, cx - 1 + i, cy - 3 + i, 1, 2, col);
            ugfx_fill_rect(s, cx - 1 + i, cy + 2 - i, 1, 2, col);
        }
    }
}

// The right edge a row's note, meter and action are laid from.
static int row_right(const struct uui_tree *t) {
    int bar = uui_tree_scrollbar_visible(t) ? uui_sbar_width() : 0;
    return t->x + t->w - bar - UUI_TREE_PAD_X - (t->sel_style == UUI_SEL_ROUNDED ? 6 : 0);
}

static int action_size(const struct uui_tree *t) { return uui_tree_row_h(t) - 6; }

// Node `node`'s action button, where its row is drawn now; 0 when it has
// none or its row is not on screen.
static int action_rect(const struct uui_tree *t, int node, int *x, int *y, int *w, int *h) {
    if (node < 0 || node >= t->count || !t->nodes[node].action) return 0;
    int row = row_of_node(t, node);
    if (row < 0) return 0;
    int rh = uui_tree_row_h(t), sz = action_size(t);
    *w = *h = sz;
    *x = row_right(t) - sz;
    *y = t->y + (row - t->top) * rh + t->anim.disp + (rh - sz) / 2;
    return *y + sz > t->y && *y < t->y + t->h;
}

// The node whose action button is at (cx, cy), or -1.
static int action_at(const struct uui_tree *t, int cx, int cy) {
    int node = uui_tree_hit(t, cx, cy), x, y, w, h;
    if (node >= 0 && action_rect(t, node, &x, &y, &w, &h) && uui_hit(x, y, w, h, cx, cy))
        return node;
    return -1;
}

int uui_tree_take_action(struct uui_tree *t) {
    int n = t->acted;
    t->acted = -1;
    return n >= 0 && n < t->count ? id_of(t, n) : -1;
}

void uui_tree_draw(struct ugfx_surface *s, const struct uui_tree *t) {
    int rh = uui_tree_row_h(t);
    int vis = uui_tree_visible_rows(t);
    int total = uui_tree_visible_count(t);
    int bar = uui_tree_scrollbar_visible(t) ? uui_sbar_width() : 0;
    uint32_t bg = tree_bg(t), fg = UUI_COLOR(t->fg, UTHEME_TEXT);
    uint32_t guide = UUI_COLOR(t->guide, UTHEME_SEPARATOR);

    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, bg);

    // The selected row's SCREEN y, captured as the rows are walked --
    // there is no node-index-to-row conversion to call, and the loop
    // already has the answer.
    int sel_ry = -1;
    int strong = t->sel_style == UUI_SEL_STRONG;
    int rounded = t->sel_style == UUI_SEL_ROUNDED;
    uint32_t muted = ugfx_blend(fg, bg, 120);
    // METERS ARE ALL OR NONE: one volume with a bar and its neighbour
    // without reads as a fault, so they show only if every one fits.
    int meters = 1;
    for (int n = 0; n < t->count && meters; n++) {
        const struct uui_tree_node *m = &t->nodes[n];
        if (!m->meter_on) continue;
        int room = t->x + t->w - uui_sbar_width() - UUI_TREE_PAD_X - (rounded ? 6 : 0)
                 - (row_text_x(t, n) + ugfx_text_width(m->label) + 6)
                 - (m->note ? ugfx_text_width(m->note) + 6 : 0)
                 - (m->action ? action_size(t) + 4 : 0);
        if (room < meter_w() + 6) meters = 0;
    }
    uint32_t sel_bg = strong ? UTHEME_ACCENT : UUI_COLOR(t->sel_bg, UTHEME_SELECTION);
    uint32_t sel_fg = strong ? UTHEME_ACCENT_TEXT : UUI_COLOR(t->sel_fg, UTHEME_TEXT);

    // The glide (ui/uui_scrollanim.h): `top` has moved; the rows are
    // drawn `disp` px from where it puts them for a few frames, plus the
    // rows the displacement uncovers, clipped to the widget. The
    // animation state is the draw's own; the tree is const to its caller.
    int disp = uui_scrollanim_sync((struct uui_scrollanim *)&t->anim, t->top * rh);
    int extra = uui_scrollanim_extra_rows(disp, rh);
    int first = disp > 0 ? t->top - extra : t->top;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, t->x, t->y, t->w, t->h);
    for (int i = 0; i < vis + 1 + extra; i++) {
        int r = first + i;
        if (r < 0) continue;
        int node = uui_tree_node_at_row(t, r);
        if (node < 0) break;
        int ry = t->y + (r - t->top) * rh + disp;
        const struct uui_tree_node *nd = &t->nodes[node];
        if (nd->kind == UUI_TREE_HEADER) {
            // A caption, bold and muted, sitting low in its row so it
            // reads as the head of what follows.
            const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
            int hy = ry + rh - ugfx_char_h() - 2;
            ugfx_draw_string_clipped(s, t->x + UUI_TREE_PAD_X + 4, hy,
                                      t->w - bar - 2 * UUI_TREE_PAD_X - 4, nd->label,
                                      muted, bg);
            ugfx_set_font(was);
            continue;
        }
        int selected = (node == t->selected);
        if (selected) sel_ry = ry;
        if (rounded && (selected || node == t->hovered)) {
            // Inset and rounded; the edge is the focus ring when focused.
            int rx = t->x + 4, rw = t->w - bar - 8;
            if (selected) {
                uint32_t edge = t->focused ? UTHEME_ACCENT
                                           : ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 130);
                uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, uui_select_radius(0), edge);
                uui_fill_round_rect(s, rx + 1, ry + 2, rw - 2, rh - 4, uui_select_radius(1), sel_bg);
            } else {
                uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, uui_select_radius(0),
                                    uui_state_bg(bg, UUI_STATE_HOVER));
            }
        } else if (selected)
            ugfx_fill_rect(s, t->x, ry, t->w - bar, rh, sel_bg);
        else if (node == t->hovered)
            ugfx_fill_rect(s, t->x, ry, t->w - bar, rh,
                            uui_state_bg(bg, UUI_STATE_HOVER));
        uint32_t row_bg = selected ? sel_bg
                        : node == t->hovered ? uui_state_bg(bg, UUI_STATE_HOVER) : bg;
        // The drop target, in the accent -- the same outline the
        // fileview draws, so a drag reads the same over both.
        if (node == t->drop_node) uui_focus_ring(s, t->x, ry, t->w - bar, rh);

        if (uui_tree_is_parent(t, node)) {
            draw_expander(s, t->x + UUI_TREE_PAD_X +
                              t->nodes[node].depth * UUI_TREE_INDENT + 4,
                          ry + rh / 2, !uui_tree_is_collapsed(t, node),
                          selected && strong ? sel_fg : muted);
        } else if (t->nodes[node].depth > 0) {
            // A guide tick for a child row, so depth reads at a glance
            // without an expander to anchor it.
            ugfx_fill_rect(s, t->x + UUI_TREE_PAD_X +
                               t->nodes[node].depth * UUI_TREE_INDENT + 3,
                           ry + rh / 2, 3, 1, guide);
        }

        // ugfx_blit_alpha(), never ugfx_blit(): an icon's corners are
        // transparent (docs/conventions/gui.md).
        int tx = row_text_x(t, node);
        int isz = ugfx_char_h(), ix = tx - icon_gutter(t), iy = ry + (rh - isz) / 2;
        if (t->nodes[node].icon) {
            const struct uimg *ic = icon_get(t->nodes[node].icon, isz);
            if (ic && ix + ic->w <= t->x + t->w - bar)
                ugfx_blit_alpha(s, ix, ry + (rh - ic->h) / 2, ic->w, ic->h, ic->px, ic->w);
        }
        // The badge over the icon's lower-right corner, overhanging by a
        // few pixels into the gutter's gap as Windows' does.
        if (t->nodes[node].badge) {
            int bsz = isz * 2 / 3 > 8 ? isz * 2 / 3 : 8;
            const struct uimg *b = icon_get(t->nodes[node].badge, bsz);
            int bx = ix + isz - bsz + 3, by = iy + isz - bsz + 2;
            if (b && bx + b->w <= t->x + t->w - bar)
                ugfx_blit_alpha(s, bx, by, b->w, b->h, b->px, b->w);
        }

        // The note and meter at the right edge -- each only where the
        // whole label still fits: the name first, the note, the meter.
        int right = row_right(t);
        int ty = ry + (rh - ugfx_char_h()) / 2;
        int need = tx + ugfx_text_width(nd->label) + 6;
        // The action first, always: it is a control, the note is not.
        if (nd->action) {
            int sz = action_size(t), ax = right - sz, ay = ry + (rh - sz) / 2;
            if (node == t->action_hot || node == t->action_press)
                uui_fill_round_rect(s, ax, ay, sz, sz, uui_control_radius(0),
                                    uui_state_bg(row_bg, node == t->action_press ? UUI_STATE_PRESSED
                                                                               : UUI_STATE_HOVER));
            const struct uimg *g = icon_get(nd->action, ugfx_char_h() - 2);
            if (g) {
                uint32_t ink;
                if (!uui_glyph_ink(nd->action, fg, row_bg, &ink)) ink = selected && strong ? sel_fg : fg;
                ugfx_blit_tinted(s, ax + (sz - g->w) / 2, ay + (sz - g->h) / 2, g->w, g->h,
                                 g->px, g->w, ink);
            }
            right = ax - 4;
        }
        if (nd->note) {
            int nw = ugfx_text_width(nd->note);
            if (right - nw >= need) {
                right -= nw;
                ugfx_draw_string(s, right, ty, nd->note, selected && strong ? sel_fg : muted,
                                 row_bg);
                right -= 6;
            }
        }
        if (nd->meter_on && meters) {
            int mw = meter_w(), mh = 3;
            if (right - mw >= need) {
                right -= mw;
                int my = ry + (rh - mh) / 2;
                ugfx_fill_rect(s, right, my, mw, mh, ugfx_blend(row_bg, UTHEME_BORDER, 70));
                int pm = nd->meter_pm < 0 ? 0 : nd->meter_pm > 1000 ? 1000 : nd->meter_pm;
                int fill = mw * pm / 1000;
                if (fill < 1 && pm > 0) fill = 1;
                ugfx_fill_rect(s, right, my, fill, mh, nd->meter_color);
                right -= 6;
            }
        }
        // CLIPPED, always: a label longer than the pane must not run
        // into the page beside it. gfx_draw_string does not clip, and
        // that has caused the identical overlap bug twice already.
        int avail = right - tx;
        if (avail > 0)
            ugfx_draw_string_clipped(s, tx, ty, avail, nd->label,
                                      node == t->drag_node ? muted : selected ? sel_fg : fg, row_bg);
        // The filter's matching run, marked over the label in the colour
        // Notepad marks a find hit in.
        int at = t->filter[0] ? find_ci(nd->label, t->filter) : -1;
        if (at >= 0 && avail > 0) {
            char run[96];
            int hn = at < (int)sizeof run - 1 ? at : (int)sizeof run - 1;
            memcpy(run, nd->label, (size_t)hn);
            run[hn] = '\0';
            int mx = tx + ugfx_text_width(run);
            int mn = (int)strlen(t->filter);
            if (mn > (int)sizeof run - 1) mn = (int)sizeof run - 1;
            memcpy(run, nd->label + at, (size_t)mn);
            run[mn] = '\0';
            int mw = ugfx_text_width(run);
            if (mx + mw > tx + avail) mw = tx + avail - mx;
            if (mw > 0) {
                ugfx_fill_rect(s, mx, ty, mw, ugfx_char_h(), FILTER_HIT_BG);
                ugfx_draw_string_clipped(s, mx, ty, mw, run, UTHEME_TEXT, FILTER_HIT_BG);
            }
        }
    }
    // THE GAP a drag would land in: a line in the accent with a dot at
    // its start, Explorer's and Dolphin's insertion mark.
    if (t->ins_y >= 0) {
        int lx = t->x + 8, lw = t->w - bar - 16, ly = t->ins_y - 1;
        ugfx_fill_rect(s, lx, ly, lw, 2, UTHEME_ACCENT);
        uui_fill_round_rect(s, lx - 3, ly - 2, 6, 6, 3, UTHEME_ACCENT);
    }
    ugfx_clip_restore(s, &saved);

    if (bar) {
        // In PIXELS, so the thumb glides with the rows: the row ratios
        // scaled by rh land the thumb on the same pixels the row-unit
        // hit test computes.
        struct uui_sbar sb = t->sb;
        uui_sbar_place(&sb, t->x + t->w - bar, t->y, t->h);
        uui_sbar_set(&sb, total * rh, vis * rh, t->top * rh - disp);
        uui_sbar_draw(&sb, s, bg, fg);
    }

    // On the selected row, or round the box when it is collapsed away
    // or scrolled off -- see uui_listbox.c for why an indicator that can
    // vanish is not one.
    if (t->focused) {
        // The strong style draws NO ring on its row: the accent fill is
        // already the mark of where focus is (Windows' and Plasma's rows).
        if (sel_ry >= 0 && (strong || rounded)) { /* the fill or edge is the indicator */ }
        else if (sel_ry >= 0) uui_focus_ring(s, t->x, sel_ry, t->w - bar, rh);
        else             uui_focus_ring(s, t->x, t->y, t->w, t->h);
    }
}

// --- input ------------------------------------------------------------

int uui_tree_hit(const struct uui_tree *t, int cx, int cy) {
    if (cx < t->x || cx >= t->x + t->w || cy < t->y || cy >= t->y + t->h) return -1;
    if (uui_tree_scrollbar_visible(t) && cx >= tree_bar_x(t)) return -1;
    // Mid-glide the rows sit `disp` px from their resting place; floor
    // division, since a row above `top` shows through a positive one.
    int rh = uui_tree_row_h(t);
    int rel = cy - t->y - t->anim.disp;
    int r = rel >= 0 ? rel / rh : -((-rel + rh - 1) / rh);
    return uui_tree_node_at_row(t, t->top + r);
}

int uui_tree_hit_expander(const struct uui_tree *t, int cx, int cy) {
    int node = uui_tree_hit(t, cx, cy);
    if (node < 0 || !uui_tree_is_parent(t, node)) return 0;
    int ex = t->x + UUI_TREE_PAD_X + t->nodes[node].depth * UUI_TREE_INDENT;
    return cx >= ex && cx < ex + UUI_TREE_EXP_W;
}

int uui_tree_hover(struct uui_tree *t, int cx, int cy) {
    int n = uui_tree_hit(t, cx, cy);
    if (is_header(t, n)) n = -1;
    if (n == t->hovered) return 0;
    t->hovered = n;
    return 1;
}

int uui_tree_click(struct uui_tree *t, int cx, int cy) {
    int node = uui_tree_hit(t, cx, cy);
    if (node < 0 || is_header(t, node)) return 0;
    // THE EXPANDER TOGGLES WITHOUT NAVIGATING. Clicking the triangle to
    // see what is inside a section is not the same gesture as choosing
    // that section, and a tree that conflated them would change the
    // page every time somebody explored.
    if (uui_tree_hit_expander(t, cx, cy))
        return uui_tree_set_collapsed(t, node, !uui_tree_is_collapsed(t, node));
    if (t->selected == node) return 0;
    t->selected = node;
    return 1;
}

int uui_tree_wheel(struct uui_tree *t, int notches) {
    int before = t->top;
    uui_scrollanim_arm(&t->anim);
    t->top -= notches;
    tree_clamp(t);
    return t->top != before;
}

int uui_tree_press(struct uui_tree *t, int cx, int cy) {
    if (cx < t->x || cx >= t->x + t->w || cy < t->y || cy >= t->y + t->h) return 0;
    int r = uui_sbar_press(tree_sbar(t), cx, cy);
    if (t->sb.grab >= 0) uui_scrollanim_cancel(&t->anim); // a drag draws where the thumb is, at once
    if (r & UUI_SBAR_MOVED) {
        uui_scrollanim_arm(&t->anim);   // a page glides
        t->top = t->sb.top;
    }
    if (r & UUI_SBAR_TOOK) return 1;
    // A row's action button arms, and selects nothing: Eject on a stick
    // is not a visit to it.
    t->action_press = action_at(t, cx, cy);
    if (t->action_press >= 0) { t->press_node = -1; return 1; }
    t->pre_press_sel = t->selected;
    t->press_node = uui_tree_hit(t, cx, cy);
    uui_tree_click(t, cx, cy);
    // NON-ZERO ON ANY HIT, even when nothing changed: the router takes
    // its pointer grab only when press does, so returning 0 for a
    // re-click on the selected row would silently cost that one row its
    // release. uui_radio_list shipped exactly that bug.
    return 1;
}

int uui_tree_drag(struct uui_tree *t, int cx, int cy) {
    if (t->sb.grab < 0) return 0;
    int r = uui_sbar_motion(tree_sbar(t), cx, cy, 1);
    if (r & UUI_SBAR_MOVED) t->top = t->sb.top;
    return (r & UUI_SBAR_MOVED) != 0;
}

void uui_tree_drag_end(struct uui_tree *t) { uui_sbar_release(&t->sb); }

// Scrolls the view so the selection is on screen. Called after any key
// that moves it -- a selection that moved off the top is indistinguishable
// from a key that did nothing.
static void reveal(struct uui_tree *t) {
    int row = row_of_node(t, t->selected);
    if (row < 0) return;
    int vis = uui_tree_visible_rows(t);
    if (row < t->top) t->top = row;
    else if (row >= t->top + vis) t->top = row - vis + 1;
    tree_clamp(t);
}

int uui_tree_key(struct uui_tree *t, int key) {
    if (t->count <= 0) return 0;
    uui_scrollanim_arm(&t->anim); // a key that scrolls the view glides it
    int row = row_of_node(t, t->selected);
    int total = uui_tree_visible_count(t);

    // A HEADING IS NOT A STOP: every move steps over one.
    switch (key) {
    case KEY_ARROW_UP:
        for (int r = row - 1; r >= 0; r--) {
            int n = uui_tree_node_at_row(t, r);
            if (!is_header(t, n)) { t->selected = n; reveal(t); return 1; }
        }
        return 0;
    case KEY_ARROW_DOWN:
        for (int r = row + 1; row >= 0 && r < total; r++) {
            int n = uui_tree_node_at_row(t, r);
            if (!is_header(t, n)) { t->selected = n; reveal(t); return 1; }
        }
        return 0;
    case KEY_HOME:
        for (int r = 0; r < total; r++) {
            int n = uui_tree_node_at_row(t, r);
            if (!is_header(t, n)) { t->selected = n; reveal(t); return 1; }
        }
        return 0;
    case KEY_END:
        for (int r = total - 1; r >= 0; r--) {
            int n = uui_tree_node_at_row(t, r);
            if (!is_header(t, n)) { t->selected = n; reveal(t); return 1; }
        }
        return 0;
    case KEY_ARROW_LEFT:
        // Collapse; or, if already collapsed (or a leaf), step OUT to
        // the parent. The two-step behaviour every desktop tree has --
        // Left on a closed node meaning "nothing" is the version that
        // feels broken.
        if (uui_tree_is_parent(t, t->selected) && !uui_tree_is_collapsed(t, t->selected))
            return uui_tree_set_collapsed(t, t->selected, 1);
        for (int i = t->selected - 1; i >= 0; i--) {
            if (t->nodes[i].depth < t->nodes[t->selected].depth) {
                t->selected = i; reveal(t); return 1;
            }
        }
        return 0;
    case KEY_ARROW_RIGHT:
        if (!uui_tree_is_parent(t, t->selected)) return 0;
        if (uui_tree_is_collapsed(t, t->selected))
            return uui_tree_set_collapsed(t, t->selected, 0);
        t->selected = uui_tree_node_at_row(t, row + 1); // into the first child
        reveal(t);
        return 1;
    default:
        return 0;
    }
}

// --- the ops tables ---------------------------------------------------

static void tree_draw_op(struct ugfx_surface *s, const void *w) {
    uui_tree_draw(s, (const struct uui_tree *)w);
}
// THE WHOLE CONTROL, scrollbar strip included -- uui_tree_hit() answers
// "which NODE", which is a different question, and the router gates
// press and wheel on this slot (uui_route.c). Answering the node
// question here left the bar undraggable. A boolean either way, so node
// 0 is not the falsey trap uui_listbox and uui_table both hit.
static int tree_hit_op(const void *w, int cx, int cy) {
    const struct uui_tree *t = (const struct uui_tree *)w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}
static int tree_press_op(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    return uui_tree_press(w, cx, cy);
}
static int tree_motion_op(void *w, int cx, int cy, unsigned buttons) {
    struct uui_tree *t = w;
    if (t->sb.grab >= 0) return uui_tree_drag(t, cx, cy);
    int r = uui_sbar_motion(tree_sbar(t), cx, cy, buttons);   // the bar's hover
    if (r & UUI_SBAR_TOOK) {
        if (t->hovered != -1) { t->hovered = -1; r |= UUI_SBAR_REDRAW; }
        return (r & UUI_SBAR_REDRAW) != 0;
    }
    int hot = buttons ? t->action_hot : action_at(t, cx, cy), moved = hot != t->action_hot;
    t->action_hot = hot;
    return uui_tree_hover(t, cx, cy) | moved | ((r & UUI_SBAR_REDRAW) != 0);
}
static int tree_release_op(void *w, int cx, int cy) {
    struct uui_tree *t = w;
    // Fires only over the button that was pressed.
    if (t->action_press >= 0 && action_at(t, cx, cy) == t->action_press) t->acted = t->action_press;
    t->action_press = -1;
    uui_tree_drag_end(t);
    return 1;
}
static int tree_wheel_op(void *w, int notches) { return uui_tree_wheel(w, notches); }
// Alt+arrow is the WINDOW's -- Back and Forward in every browser, file
// manager and help viewer -- so a tree with focus must not eat it as a
// collapse.
static int tree_key_op(void *w, int key, unsigned mods) {
    if (mods & KEY_MOD_ALT) return 0;
    return uui_tree_key(w, key);
}
static void tree_natural_op(const void *w, int *out_w, int *out_h) {
    uui_tree_natural_size((const struct uui_tree *)w, out_w, out_h);
}
static void tree_set_geometry_op(void *w, int x, int y, int rw, int rh) {
    struct uui_tree *t = w;
    t->x = x; t->y = y; t->w = rw; t->h = rh;
    tree_clamp(t);
}

static void tree_bounds_op(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_tree *t = w;
    if (x) *x = t->x;
    if (y) *y = t->y;
    if (out_w) *out_w = t->w;
    if (out_h) *out_h = t->h;
}

const struct uui_widget_ops uui_tree_focus_ops = {
    .key = tree_key_op,
};

// A tree with no nodes has nothing to navigate, so it is skipped in the
// tab order rather than being a stop that does nothing -- the same call
// uui_listbox_ops and uui_slider_ops make. It was UNDECLARED until
// tools/check_widget_ops.py grew a rule pairing `key` with this one:
// the ring's default is "accepts", so nothing behaved differently, but
// a default is not a statement.
static void tree_set_focused_op(void *w, int focused) {
    ((struct uui_tree *)w)->focused = focused;
}

static int tree_accepts_focus_op(const void *w) {
    return ((const struct uui_tree *)w)->count > 0;
}

static void tree_describe_op(const void *w, const struct uui_describe *d) {
    const struct uui_tree *t = (const struct uui_tree *)w;
    uui_describe_int(d, "row_h", uui_tree_row_h(t));
    uui_describe_int(d, "rows", uui_tree_visible_count(t));
    uui_describe_int(d, "selected", t->selected);
}

// --- a drag source: its own rows, reordered -----------------------------
//
// Only with uui_tree_set_reorder(), and only a row the app lets move. The
// press that became a drag selected the row: the selection goes back, so
// picking a row up is not choosing it.
static int tree_drag_start_op(void *w, int cx, int cy, struct uui_drag *d) {
    (void)cx; (void)cy;
    struct uui_tree *t = (struct uui_tree *)w;
    int n = t->press_node;
    if (!t->can_drag || !t->can_insert || n < 0 || n >= t->count || is_header(t, n) ||
        !t->can_drag(t->reorder_ctx, id_of(t, n))) return 0;
    t->selected = t->pre_press_sel;
    t->drag_node = n;
    snprintf(t->drag_label, sizeof t->drag_label, "%s", t->nodes[n].label ? t->nodes[n].label : "");
    d->kind = UUI_DRAG_ROW;
    d->count = 1;
    d->label = t->drag_label;
    d->dir = 0;
    return 1;
}

static void tree_drag_end_op(void *w, int dropped) {
    (void)dropped;
    struct uui_tree *t = (struct uui_tree *)w;
    t->drag_node = -1;
    t->ins_y = -1;
    uui_sbar_release(&t->sb);
}

// The ghost: the row's icon and label under the pointer.
static void tree_drag_draw_op(struct ugfx_surface *s, const void *w, const struct uui_drag *d) {
    const struct uui_tree *t = (const struct uui_tree *)w;
    int n = t->drag_node, px = ugfx_char_h();
    if (n < 0 || n >= t->count) return;
    const struct uimg *ic = t->nodes[n].icon ? icon_get(t->nodes[n].icon, px) : 0;
    int tw = ugfx_text_width(t->drag_label);
    int gx = d->x + 12, gy = d->y + 8, gw = px + 10 + tw + 8, gh = px + 8;
    uint32_t sel_bg = UUI_COLOR(t->sel_bg, UTHEME_SELECTION), sel_fg = UUI_COLOR(t->sel_fg, UTHEME_TEXT);
    uui_fill_round_rect(s, gx, gy, gw, gh, uui_select_radius(0), sel_bg);
    if (ic) ugfx_blit_alpha(s, gx + 4, gy + 4, ic->w, ic->h, ic->px, ic->w);
    ugfx_draw_string(s, gx + px + 10, gy + (gh - ugfx_char_h()) / 2, t->drag_label, sel_fg, sel_bg);
}

// --- a drop target (ui/uui_route.h's third rule) ------------------------
//
// Every node accepts files: a tree's rows are all places (a folder tree
// lists only directories), and the app decides at the drop whether the
// node is somewhere they can go. With reordering on, the GAPS take part
// too: a row dragged in this tree only ever lands in one, and a file
// lands in one when it is near a row's edge rather than its middle.

// The gap at (cx, cy): `edge` 0 picks the nearer one for any point, 1
// only for one within a quarter row of it. Sets ins_* and returns 1 when
// the app takes it.
static int find_gap(struct uui_tree *t, int cx, int cy, int from, int edge) {
    (void)cx;
    int rh = uui_tree_row_h(t), total = uui_tree_visible_count(t);
    if (rh <= 0 || cy < t->y) return 0;
    int rel = cy - t->y, r = t->top + rel / rh, frac = rel % rh;
    if (r >= total) { r = total; frac = 0; }   // below the last row: the end
    int gap = frac < rh / 2 ? r : r + 1;
    if (edge && frac >= rh / 4 && frac < rh - rh / 4 && r < total) return 0;
    int before = gap < total ? uui_tree_node_at_row(t, gap) : -1;
    int after = gap > 0 ? uui_tree_node_at_row(t, gap - 1) : -1;
    if (!t->can_insert(t->reorder_ctx, from < 0 ? -1 : id_of(t, from), id_of(t, after), id_of(t, before)))
        return 0;
    t->ins_y = t->y + (gap - t->top) * rh;
    t->ins_after = after;
    t->ins_before = before;
    return 1;
}

static int tree_drag_over_op(void *w, int cx, int cy, const struct uui_drag *d) {
    struct uui_tree *t = (struct uui_tree *)w;
    t->ins_y = -1;
    t->drop_node = -1;
    if (cx == UUI_NOWHERE) return 0;
    int reorder = t->can_insert != 0;
    if (d->kind == UUI_DRAG_ROW)
        return d->source == w && reorder && find_gap(t, cx, cy, t->drag_node, 0);
    if (d->kind != UUI_DRAG_FILES) return 0;
    if (reorder && find_gap(t, cx, cy, -1, 1)) return 1;
    int node = uui_tree_hit(t, cx, cy);
    if (is_header(t, node)) node = -1;
    t->drop_node = node;
    return node >= 0;
}

static int tree_drop_op(void *w, int cx, int cy, const struct uui_drag *d) {
    (void)cx; (void)cy;
    struct uui_tree *t = (struct uui_tree *)w;
    t->dropped_insert = 0;
    if (t->ins_y >= 0) {
        t->dropped_insert = 1;
        t->dropped_from = d->kind == UUI_DRAG_ROW ? id_of(t, t->drag_node) : -1;
        t->dropped_after = id_of(t, t->ins_after);
        t->dropped_before = id_of(t, t->ins_before);
        t->ins_y = -1;
        return 1;
    }
    if (t->drop_node < 0) return 0;
    t->dropped_node = t->drop_node;
    t->drop_node = -1;
    return 1;
}

const struct uui_widget_ops uui_tree_ops = {
    .draw = tree_draw_op,
    .bounds = tree_bounds_op,
    .accepts_focus = tree_accepts_focus_op,
    .set_focused   = tree_set_focused_op,
    .hit = tree_hit_op,
    .press = tree_press_op,
    .motion = tree_motion_op,
    .release = tree_release_op,
    .wheel = tree_wheel_op,
    .key = tree_key_op,
    .natural_size = tree_natural_op,
    .set_geometry = tree_set_geometry_op,
    .describe     = tree_describe_op,
    .drag_over    = tree_drag_over_op,
    .drop         = tree_drop_op,
    .drag_start   = tree_drag_start_op,
    .drag_end     = tree_drag_end_op,
    .drag_draw    = tree_drag_draw_op,
};
