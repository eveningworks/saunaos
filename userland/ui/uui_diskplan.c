// Before and after, as two partition maps (ui/uui_diskplan.h).
#include <stdio.h>
#include <string.h>
#include "ui/uui_diskplan.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "lib/human.h"
#include "query_abi.h"   // QUERY_PART_GPT

static const char *const LABELS[2] = { "Now", "After" };

void uui_diskplan_init(struct uui_diskplan *p) {
    memset(p, 0, sizeof *p);
    uui_segbar_init(&p->now);
    uui_segbar_init(&p->after);
    p->now.min_w = p->after.min_w = ugfx_char_w() * 6;
}

// The colours say what a partition IS -- Disks' (gui/apps/disks.c):
// firmware amber, a filesystem the accent's tint, the rest grey. Roles,
// so a theme switch repaints them (ui/uui_segbar.h).
void uui_diskplan_colour(struct uui_segbar_seg *s, int role, int strong) {
    if (role == UDISK_ROLE_ESP || role == UDISK_ROLE_BIOS) {
        s->fill = UUI_COLOR_UNSET;   // a wash of the edge
        s->edge = UUI_ROLE(action[UTHEME_ACT_ARRANGE]);
    } else if (strong) {
        s->fill = UUI_ROLE(selection_bg);
        s->edge = UUI_ROLE(accent);
    } else {
        s->fill = UUI_ROLE(panel_bg);
        s->edge = UUI_ROLE(outline);
    }
}

static void add_now(struct uui_diskplan *p, int *n, const struct udisk_part *part, uint64_t count) {
    if (*n >= UUI_SEGBAR_MAX) return;
    int i = (*n)++;
    struct uui_segbar_seg *s = &p->now_segs[i];
    memset(s, 0, sizeof *s);
    s->size = count;
    s->used_pct = -1;
    char size[24];
    human_size_iec(size, sizeof size, count * 512);
    p->now_corner[i][0] = 0;
    if (!part) {
        snprintf(p->now_title[i], sizeof p->now_title[i], "Free space");
        snprintf(p->now_detail[i], sizeof p->now_detail[i], "%s", size);
        s->empty = 1;
    } else {
        udisk_part_title(part, p->now_title[i], sizeof p->now_title[i]);
        snprintf(p->now_detail[i], sizeof p->now_detail[i], "%s%s%s", size, udisk_part_fs(part)[0] ? " " : "",
                 udisk_part_fs(part));
        snprintf(p->now_corner[i], sizeof p->now_corner[i], "%s", part->point);
        uui_diskplan_colour(s, part->role, part->fs[0] != 0);
        s->corner = p->now_corner[i][0] ? p->now_corner[i] : 0;
    }
    s->title = p->now_title[i];
    s->detail = p->now_detail[i];
}

void uui_diskplan_set(struct uui_diskplan *p, const struct udisk *disk,
                      const struct uui_diskplan_new *after, int n) {
    int count = 0;
    if (disk) {
        // In disk order, and every gap of a megabyte or more as free
        // space -- Disks' map, from the first usable sector to the last.
        static struct udisk d;   // a few KB: never on the stack
        d = *disk;
        udisk_sort(&d);
        uint64_t at = UDISK_MIB, end = d.kind == QUERY_PART_GPT ? d.sectors - UDISK_GPT_TAIL : d.sectors;
        if (d.nparts && !d.kind) at = 0;   // a whole-disk volume
        for (int i = 0; i < d.nparts; i++) {
            const struct udisk_part *q = &d.parts[i];
            if (q->start > at && q->start - at >= UDISK_MIB) add_now(p, &count, 0, q->start - at);
            add_now(p, &count, q, q->count);
            if (q->start + q->count > at) at = q->start + q->count;
        }
        if (end > at && end - at >= UDISK_MIB) add_now(p, &count, 0, end - at);
    }
    uui_segbar_set(&p->now, p->now_segs, count);

    if (n > UUI_DISKPLAN_NEW_MAX) n = UUI_DISKPLAN_NEW_MAX;
    for (int i = 0; i < n; i++) {
        struct uui_segbar_seg *s = &p->after_segs[i];
        memset(s, 0, sizeof *s);
        s->size = after[i].count;
        s->used_pct = -1;
        char size[24];
        human_size_iec(size, sizeof size, after[i].count * 512);
        snprintf(p->after_title[i], sizeof p->after_title[i], "%s", after[i].title ? after[i].title : "");
        snprintf(p->after_detail[i], sizeof p->after_detail[i], "%s%s%s", size,
                 after[i].detail ? " " : "", after[i].detail ? after[i].detail : "");
        uui_diskplan_colour(s, after[i].role, 1);
        s->title = p->after_title[i];
        s->detail = p->after_detail[i];
    }
    uui_segbar_set(&p->after, p->after_segs, n < 0 ? 0 : n);
}

// --- ops ------------------------------------------------------------------

static int row_h(void) { return ugfx_char_h() * 2 + ugfx_char_w() + 4; }
static int gap(void) { return ugfx_char_h() / 2; }
static int label_w(void) { return ugfx_text_width("After") + ugfx_char_w() * 2; }

static void dp_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = label_w() + ugfx_char_w() * 40;
    *oh = row_h() * 2 + gap();
}

static void dp_geometry(void *w, int x, int y, int width, int height) {
    struct uui_diskplan *p = w;
    (void)height;
    p->x = x; p->y = y; p->w = width; p->h = row_h() * 2 + gap();
    p->label_w = label_w();
    int bx = x + p->label_w, bw = width - p->label_w;
    uui_segbar_ops.set_geometry(&p->now, bx, y, bw, row_h());
    uui_segbar_ops.set_geometry(&p->after, bx, y + row_h() + gap(), bw, row_h());
}

static void dp_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_diskplan *p = w;
    *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}

static void dp_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_diskplan *p = w;
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    for (int r = 0; r < 2; r++) {
        int ry = p->y + r * (row_h() + gap()) + (row_h() - ugfx_char_h()) / 2;
        ugfx_draw_string_clipped(s, p->x, ry, p->label_w, LABELS[r], dim, UTHEME_WINDOW_BG);
    }
    if (p->now.count) uui_segbar_ops.draw(s, &p->now);
    else ugfx_draw_string_clipped(s, p->now.x, p->y + (row_h() - ugfx_char_h()) / 2, p->now.w,
                                  "No disk chosen", dim, UTHEME_WINDOW_BG);
    if (p->after.count) uui_segbar_ops.draw(s, &p->after);
}

const struct uui_widget_ops uui_diskplan_ops = {
    .natural_size = dp_natural,
    .set_geometry = dp_geometry,
    .bounds = dp_bounds,
    .draw = dp_draw,
};
