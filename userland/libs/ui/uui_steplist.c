// See uui_steplist.h.
#include <stdio.h>
#include <string.h>
#include "ui/uui_steplist.h"
#include "ui/utheme.h"

static void cell(void *ctx, int row, int col, char *out, int cap) {
    const struct uui_steplist *sl = ctx;
    out[0] = 0;
    if (row < 0 || row >= sl->count) return;
    const struct uui_step *s = &sl->steps[row];
    if (col == 0) snprintf(out, (size_t)cap, "%d", row + 1);
    else if (col == 1) snprintf(out, (size_t)cap, "%s", s->verb ? s->verb : "");
    else if (col == sl->col_text) snprintf(out, (size_t)cap, "%s", s->text ? s->text : "");
    else if (col == sl->col_remove) snprintf(out, (size_t)cap, "%s", s->state == UUI_STEP_WAITING ? "Remove" : "");
    else if (col == sl->col_state) {
        switch (s->state) {
        case UUI_STEP_RUNNING:
            if (s->permille >= 0) snprintf(out, (size_t)cap, "Running %d%%", s->permille / 10);
            else snprintf(out, (size_t)cap, "Running");
            break;
        case UUI_STEP_DONE:    snprintf(out, (size_t)cap, "Done"); break;
        case UUI_STEP_FAILED:  snprintf(out, (size_t)cap, "Failed"); break;
        case UUI_STEP_NOT_RUN: snprintf(out, (size_t)cap, "Not run"); break;
        default:               snprintf(out, (size_t)cap, "Waiting"); break;
        }
    }
}

// The word says the state; the dot is the glance.
static uint32_t dot(void *ctx, int row, int col) {
    const struct uui_steplist *sl = ctx;
    if (col != sl->col_state || row < 0 || row >= sl->count) return 0;
    switch (sl->steps[row].state) {
    case UUI_STEP_RUNNING: return UTHEME_ACCENT;
    case UUI_STEP_DONE:    return utheme_action(UTHEME_ACT_CREATE);
    case UUI_STEP_FAILED:  return utheme_action(UTHEME_ACT_DANGER);
    default:               return 0;
    }
}

static int meter(void *ctx, int row, int col) {
    const struct uui_steplist *sl = ctx;
    if (col != sl->col_text || row < 0 || row >= sl->count) return -1;
    const struct uui_step *s = &sl->steps[row];
    return s->state == UUI_STEP_RUNNING && s->permille >= 0 ? s->permille : -1;
}

static int fade(void *ctx, int row) {
    const struct uui_steplist *sl = ctx;
    return row >= 0 && row < sl->count && sl->steps[row].state == UUI_STEP_NOT_RUN;
}

void uui_steplist_init(struct uui_steplist *sl, int flags) {
    sl->flags = flags;
    int n = 0;
    sl->cols[n++] = (struct uui_table_column){ "#", 3, UUI_TALIGN_RIGHT, 0, 0 };
    sl->cols[n++] = (struct uui_table_column){ "Step", 8, UUI_TALIGN_LEFT, 0, 0 };
    sl->col_text = n;
    sl->cols[n++] = (struct uui_table_column){ "Details", 0, UUI_TALIGN_LEFT, 0, 0 };
    sl->col_state = sl->col_remove = -1;
    if (flags & UUI_STEPLIST_STATE) {
        sl->col_state = n;
        sl->cols[n++] = (struct uui_table_column){ "State", 13, UUI_TALIGN_LEFT, 0, 0 };
    }
    if (flags & UUI_STEPLIST_REMOVE) {
        sl->col_remove = n;
        sl->cols[n++] = (struct uui_table_column){ "", 9, UUI_TALIGN_LEFT, 0, UUI_TCOL_ACTION };
    }
    uui_table_init(&sl->table, 0, 0, 0, 0, sl->cols, n, cell, sl);
    uui_table_set_dot(&sl->table, dot);
    uui_table_set_meter(&sl->table, meter);
    uui_table_set_fade(&sl->table, fade);
}

void uui_steplist_set(struct uui_steplist *sl, const struct uui_step *steps, int count) {
    sl->steps = steps;
    sl->count = count;
    uui_table_set_rows(&sl->table, count);
}

int uui_steplist_take_remove(struct uui_steplist *sl) {
    int col, row = uui_table_take_action(&sl->table, &col);
    return row >= 0 && col == sl->col_remove ? row : -1;
}

int uui_steplist_height(const struct uui_steplist *sl, int rows) {
    return uui_table_header_h(&sl->table) + uui_table_row_h(&sl->table) * rows + 2;
}
