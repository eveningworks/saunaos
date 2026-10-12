// Theme files -- see ui/utheme_file.h and docs/theme-design.md.
#include "ui/utheme_file.h"
#include "lib/uconf.h"
#include "lib/usetting.h"
#include "knum.h"
#include "rt/sys.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROLE(k, l, g, f) { k, l, g, (uint16_t)offsetof(struct utheme, f) }
const struct utheme_role utheme_roles[] = {
    ROLE("titlebar",               "Title bar",             "Window frame", titlebar),
    ROLE("titlebar_text",          "Title text",            "Window frame", titlebar_text),
    ROLE("titlebar_inactive",      "Title bar, inactive",   "Window frame", titlebar_inactive),
    ROLE("titlebar_inactive_text", "Title text, inactive",  "Window frame", titlebar_inactive_text),
    ROLE("caption_bg",             "Caption buttons",       "Window frame", caption_bg),
    ROLE("border",                 "Window and menu frame", "Window frame", border),
    ROLE("window_bg",              "Window ground",         "Pages and controls", window_bg),
    ROLE("panel_bg",               "Page",                  "Pages and controls", panel_bg),
    ROLE("control_bg",             "Control face",          "Pages and controls", control_bg),
    ROLE("field_bg",               "Field",                 "Pages and controls", field_bg),
    ROLE("bar_bg",                 "Menu and status bars",  "Pages and controls", bar_bg),
    ROLE("chrome",                 "Command bars",          "Pages and controls", chrome),
    ROLE("tab_rest",               "Resting tab",           "Pages and controls", tab_rest),
    ROLE("outline",                "Control outline",       "Pages and controls", outline),
    ROLE("separator",              "Separator",             "Pages and controls", separator),
    ROLE("text",                   "Text",                  "Text and selection", text),
    ROLE("text_muted",             "Muted text",            "Text and selection", text_muted),
    ROLE("accent",                 "Accent",                "Text and selection", accent),
    ROLE("accent_text",            "Text on accent",        "Text and selection", accent_text),
    ROLE("selection_bg",           "Selection",             "Text and selection", selection_bg),
    ROLE("find_hit",               "Search match",          "Text and selection", find_hit),
    ROLE("find_hit_current",       "Current match",         "Text and selection", find_hit_current),
    ROLE("tooltip_bg",             "Tooltip",               "Text and selection", tooltip_bg),
    ROLE("tooltip_text",           "Tooltip text",          "Text and selection", tooltip_text),
    ROLE("action_nav",             "Navigate",              "Command colours", action[UTHEME_ACT_NAV]),
    ROLE("action_view",            "View",                  "Command colours", action[UTHEME_ACT_VIEW]),
    ROLE("action_create",          "Create",                "Command colours", action[UTHEME_ACT_CREATE]),
    ROLE("action_edit",            "Edit",                  "Command colours", action[UTHEME_ACT_EDIT]),
    ROLE("action_danger",          "Danger",                "Command colours", action[UTHEME_ACT_DANGER]),
    ROLE("action_arrange",         "Arrange",               "Command colours", action[UTHEME_ACT_ARRANGE]),
    ROLE("action_media",           "Media",                 "Command colours", action[UTHEME_ACT_MEDIA]),
    ROLE("danger_bg",              "Danger button",         "Command colours", danger_bg),
    ROLE("severity_error",         "Error",                 "Command colours", severity[UTHEME_SEV_ERROR]),
    ROLE("severity_warning",       "Warning",               "Command colours", severity[UTHEME_SEV_WARNING]),
};
const int utheme_role_count = sizeof utheme_roles / sizeof utheme_roles[0];

uint32_t *utheme_role_ptr(struct utheme *t, int role) {
    if (role < 0 || role >= utheme_role_count) return 0;
    return (uint32_t *)((char *)t + utheme_roles[role].offset);
}

const char *const utheme_bundle_names[UTHEME_BUNDLE_COUNT] = {
    "desktop.corners", "desktop.taskbar_theme", "system.cursor_theme",
    "desktop.wallpaper_type", "desktop.wallpaper", "desktop.background_colour",
    "desktop.transparency", "desktop.shadows",
};

int utheme_parse_colour(const char *s, uint32_t *out) {
    if (!s) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        int d = k_hex_digit(s[i]);
        if (d < 0) return 0;
        v = (v << 4) | (uint32_t)d;
    }
    if (s[6]) return 0;
    *out = v;
    return 1;
}

int utheme_stem_ok(const char *stem) {
    size_t n = stem ? strlen(stem) : 0;
    if (n == 0 || n >= UTHEME_STEM_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = stem[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

static int stem_path(const char *dir, const char *stem, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s.theme", dir, stem);
    return n > 0 && (size_t)n < cap;
}

int utheme_path(const char *stem, char *out, size_t cap) {
    if (!utheme_stem_ok(stem)) return 0;
    if (stem_path(UTHEME_LOCAL_DIR, stem, out, cap) && access(out, F_OK) == 0) return 1;
    return stem_path(UTHEME_SHIPPED_DIR, stem, out, cap) && access(out, F_OK) == 0;
}

int utheme_is_shipped(const char *stem) {
    char p[96];
    return utheme_stem_ok(stem) && stem_path(UTHEME_SHIPPED_DIR, stem, p, sizeof p) &&
           access(p, F_OK) == 0;
}

static int word_of(const char *v, const char *const *words, int n, int *out) {
    for (int i = 0; i < n; i++) if (!strcmp(v, words[i])) { *out = i; return 1; }
    return 0;
}

static const char *const CAPTION_WORD[] = { "disc", "rect" };
static const char *const ALIGN_WORD[] = { "left", "center" };
#define PAD_MIN 4
#define PAD_MAX 20

int utheme_load_path(const char *path, struct utheme *out, struct utheme_bundle *b,
                     int *rejected) {
    utheme_default(out);
    if (b) memset(b, 0, sizeof *b);
    int bad = 0;
    // 4 KiB: on the heap, as every etc_config_buf in ring 3 is (lib/uconf.h).
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return 0;
    if (!uconf_load(path, buf)) { free(buf); return 0; }

    char v[SETTING_ABI_VALUE_MAX];
    if (etc_config_buf_get(buf, "Name", v, sizeof v) && v[0])
        strlcpy(out->name, v, sizeof out->name);
    for (int r = 0; r < utheme_role_count; r++) {
        if (!etc_config_buf_get(buf, utheme_roles[r].key, v, sizeof v)) continue;
        if (!utheme_parse_colour(v, utheme_role_ptr(out, r))) bad++;
    }
    // The "none" slots follow the text, as the default's do.
    out->action[UTHEME_ACT_NONE] = out->text;
    out->severity[UTHEME_SEV_NONE] = out->text;

    int w;
    if (etc_config_buf_get_in(buf, "frame", "caption_buttons", v, sizeof v)) {
        if (word_of(v, CAPTION_WORD, 2, &w)) out->caption_style = (uint8_t)w; else bad++;
    }
    if (etc_config_buf_get_in(buf, "frame", "title_align", v, sizeof v)) {
        if (word_of(v, ALIGN_WORD, 2, &w)) out->title_align = (uint8_t)w; else bad++;
    }
    if (etc_config_buf_get_in(buf, "frame", "titlebar_pad", v, sizeof v)) {
        uint32_t pad;
        if (k_parse_u32(v, &pad) && pad >= PAD_MIN && pad <= PAD_MAX) out->titlebar_pad = (uint8_t)pad;
        else bad++;
    }
    for (int i = 0; b && i < UTHEME_BUNDLE_COUNT; i++)
        etc_config_buf_get_in(buf, "apply", utheme_bundle_names[i], b->value[i], sizeof b->value[i]);
    free(buf);
    if (rejected) *rejected = bad;
    return 1;
}

int utheme_load(const char *stem, struct utheme *out, struct utheme_bundle *b) {
    char path[96];
    if (!utheme_path(stem, path, sizeof path)) { utheme_default(out); return 0; }
    return utheme_load_path(path, out, b, 0);
}

int utheme_save(const char *path, const struct utheme *t, const struct utheme_bundle *b,
                const char *description) {
    char tmp[112];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# toy-os theme -- docs/theme-design.md\nName=%s\n", t->name);
    if (description && description[0]) fprintf(f, "Description=%s\n", description);
    const char *group = "";
    struct utheme copy = *t;
    for (int r = 0; r < utheme_role_count; r++) {
        if (strcmp(group, utheme_roles[r].group)) {
            group = utheme_roles[r].group;
            fprintf(f, "\n# %s\n", group);
        }
        fprintf(f, "%s=%06X\n", utheme_roles[r].key, (unsigned)(*utheme_role_ptr(&copy, r) & 0xFFFFFF));
    }
    fprintf(f, "\n[frame]\ncaption_buttons=%s\ntitle_align=%s\ntitlebar_pad=%u\n",
            CAPTION_WORD[t->caption_style ? 1 : 0], ALIGN_WORD[t->title_align ? 1 : 0],
            (unsigned)t->titlebar_pad);
    int any = 0;
    for (int i = 0; b && i < UTHEME_BUNDLE_COUNT; i++) {
        if (!b->value[i][0]) continue;
        if (!any++) fprintf(f, "\n[apply]\n");
        fprintf(f, "%s=%s\n", utheme_bundle_names[i], b->value[i]);
    }
    if (fclose(f) != 0) { unlink(tmp); return 0; }
    // A rename does not replace a file here (lib/urecent.c).
    unlink(path);
    return sys_rename(tmp, path) == 0;
}

int utheme_apply(const char *stem) {
    struct utheme t;
    struct utheme_bundle b;
    if (!utheme_load(stem, &t, &b)) return -1;
    int n = 0;
    for (int i = 0; i < UTHEME_BUNDLE_COUNT; i++)
        if (b.value[i][0] && usetting_set(utheme_bundle_names[i], b.value[i]) > 0) n++;
    usetting_set("desktop.theme", stem);
    return n;
}
