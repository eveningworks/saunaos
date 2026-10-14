// A font, chosen -- see ui/uui_fontpick.h.
#include "ui/uui_fontpick.h"
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

#define FONT_DIR "/usr/share/fonts"
enum { ID_FAMILY, ID_SIZE, ID_STYLE };
enum { CODE_BOLD = 1, CODE_ITALIC };

// The families worth offering, in this order, when their file is there.
static const struct { const char *label, *file; } KNOWN[] = {
    { "Liberation Sans", "liberation-sans" }, { "Liberation Serif", "liberation-serif" },
    { "DejaVu Sans Mono", "dejavu-sans-mono" }, { "Vera Mono", "vera-mono" },
};

static int exists(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0 && !st.is_dir;
}

static const struct uui_toolbar_item STYLE[] = {
    { 0, "Bold",   CODE_BOLD,   "B", 0, 0, 0 },
    { 0, "Italic", CODE_ITALIC, "I", 0, 0, 0 },
};

// The toolbar asks by code alone: one picker's latches at a time, the
// arrangement uui_filedialog's view buttons use.
static const struct uui_fontpick *g_flags_for;
static unsigned style_flags(int code) {
    if (!g_flags_for) return 0;
    if (code == CODE_BOLD) return g_flags_for->bold ? UUI_MI_CHECKED : 0;
    if (code == CODE_ITALIC) return g_flags_for->italic ? UUI_MI_CHECKED : 0;
    return 0;
}

void uui_fontpick_init(struct uui_fontpick *p, int id_base, const char *family, int px,
                       int bold, int italic) {
    memset(p, 0, sizeof *p);
    p->id_base = id_base;
    p->bold = bold;
    p->italic = italic;
    int sel = 0;
    for (unsigned i = 0; i < sizeof KNOWN / sizeof KNOWN[0] && p->nfam < UUI_FONTPICK_FAMILIES; i++) {
        char path[96];
        snprintf(path, sizeof path, FONT_DIR "/%s.ttf", KNOWN[i].file);
        if (!exists(path)) continue;
        if (family && !strcmp(family, KNOWN[i].file)) sel = p->nfam;
        p->labels[p->nfam] = KNOWN[i].label;
        p->files[p->nfam] = KNOWN[i].file;
        p->nfam++;
    }
    uui_dropdown_init(&p->family, 0, 0, 0, 0, p->labels, p->nfam);
    if (p->nfam) uui_dropdown_set_selected(&p->family, sel);
    uui_spinbox_init(&p->size, px, 6, 200, 1, "px");
    uui_toolbar_init(&p->style, STYLE, 2);
    p->style.item_flags = style_flags;
    p->style.compact = 1;
    p->style.bg = UUI_ROLE(field_bg);   // on whatever card holds it, not a toolbar's grey
    p->style.accent_latch = 1;
    g_flags_for = p;
    p->it[0] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &p->family, .id = id_base + ID_FAMILY,
                                  .name = "fontfamily" };
    p->it[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &p->size, .id = id_base + ID_SIZE,
                                  .name = "fontsize" };
    p->it[2] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &p->style, .id = id_base + ID_STYLE,
                                  .name = "fontstyle" };
    p->row = (struct uui_layout){ .dir = UUI_ROW, .items = p->it, .count = 3, .margin = 1 };
}

struct uui_item uui_fontpick_item(struct uui_fontpick *p) {
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &p->row };
}

int uui_fontpick_focusables(struct uui_fontpick *p, struct uui_focusable *out, int max) {
    if (max < 2) return 0;
    out[0] = (struct uui_focusable){ &p->family, &uui_dropdown_ops };
    out[1] = (struct uui_focusable){ &p->size, &uui_spinbox_ops };
    return 2;
}

int uui_fontpick_px(const struct uui_fontpick *p) { return uui_spinbox_value(&p->size); }

int uui_fontpick_on_widget(struct uui_fontpick *p, int id) {
    int k = id - p->id_base;
    if (k < 0 || k >= UUI_FONTPICK_IDS) return 0;
    g_flags_for = p;
    if (k == ID_STYLE) {
        int code = uui_toolbar_take_code(&p->style);
        if (code == CODE_BOLD) { p->bold = !p->bold; return 1; }
        if (code == CODE_ITALIC) { p->italic = !p->italic; return 1; }
        return 0;
    }
    if (k == ID_SIZE) uui_spinbox_commit(&p->size);
    return 1;   // a family or a size: the caller re-loads, cheaply skipping no change
}

int uui_fontpick_path(const struct uui_fontpick *p, char *out, int cap, int *embolden) {
    int f = uui_dropdown_selected(&p->family);
    if (f < 0 || f >= p->nfam) return 0;
    // The styled face's own file when there is one; otherwise the plain
    // face, emboldened by the rasterizer for Bold (an italic cannot be
    // faked, so it falls back to upright).
    const char *suffix = p->bold && p->italic ? "-bold-italic" : p->bold ? "-bold" : p->italic ? "-italic" : "";
    snprintf(out, (size_t)cap, FONT_DIR "/%s%s.ttf", p->files[f], suffix);
    if (exists(out)) { if (embolden) *embolden = 0; return 1; }
    if (p->bold && p->italic) {
        snprintf(out, (size_t)cap, FONT_DIR "/%s-italic.ttf", p->files[f]);
        if (exists(out)) { if (embolden) *embolden = 1; return 1; }
    }
    snprintf(out, (size_t)cap, FONT_DIR "/%s.ttf", p->files[f]);
    if (embolden) *embolden = p->bold;
    return exists(out);
}
