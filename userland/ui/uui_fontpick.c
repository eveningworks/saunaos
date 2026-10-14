// A font, chosen -- see ui/uui_fontpick.h.
#include "ui/uui_fontpick.h"
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

#define FONT_DIR "/usr/share/fonts"
enum { ID_FAMILY, ID_SIZE, ID_STYLE };

// The families worth offering, in this order, when their file is there.
static const struct { const char *label, *file; } KNOWN[] = {
    { "Liberation Sans", "liberation-sans" }, { "Liberation Serif", "liberation-serif" },
    { "DejaVu Sans Mono", "dejavu-sans-mono" }, { "Vera Mono", "vera-mono" },
};

static int exists(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0 && !st.is_dir;
}

static const struct uui_toggle STYLE[] = {
    { 0, "Bold",          "tb-bold",      0 },
    { 0, "Italic",        "tb-italic",    0 },
    { 0, "Underline",     "tb-underline", 0 },
    { 0, "Strikethrough", "tb-strike",    0 },
};

static void sync_style(struct uui_fontpick *p) {
    p->style.on = (unsigned)(p->bold | p->italic << 1 | p->underline << 2 | p->strike << 3);
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
    uui_spinbox_init(&p->size, px, 6, UGFX_FONT_PX_MAX, 1, "px");   // what the rasterizer takes
    uui_togglebar_init(&p->style, STYLE, 4);
    sync_style(p);
    // ONE HEIGHT FOR THE ROW: each stretched to it, so the list, the
    // number and the toggles line up top and bottom.
    p->it[0] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &p->family, .id = id_base + ID_FAMILY,
                                  .flags = UUI_FILL_H, .name = "fontfamily" };
    p->it[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &p->size, .id = id_base + ID_SIZE,
                                  .flags = UUI_FILL_H, .name = "fontsize" };
    p->it[2] = (struct uui_item){ .ops = &uui_togglebar_ops, .widget = &p->style, .id = id_base + ID_STYLE,
                                  .flags = UUI_FILL_H, .name = "fontstyle" };
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
    if (k == ID_STYLE) {
        int i = uui_togglebar_take(&p->style);
        if (i == 0) p->bold = !p->bold;
        else if (i == 1) p->italic = !p->italic;
        else if (i == 2) p->underline = !p->underline;
        else if (i == 3) p->strike = !p->strike;
        else return 0;
        sync_style(p);
        return 1;
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
