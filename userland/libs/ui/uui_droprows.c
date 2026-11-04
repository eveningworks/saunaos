// A dropdown's fixed rows, the person's own rows and an action row;
// see ui/uui_droprows.h.
#include "ui/uui_droprows.h"
#include <string.h>

static int row_of(const struct uui_droprows *r, int kind, int index) {
    switch (kind) {
    case UUI_DROPROW_FIXED: return index >= 0 && index < r->nfixed ? index : -1;
    case UUI_DROPROW_OWN:   return index >= 0 && index < r->nown ? r->nfixed + index : -1;
    case UUI_DROPROW_MORE:  return r->more ? r->nfixed + r->nown : -1;
    default:                return -1;
    }
}

static void rebuild(struct uui_droprows *r) {
    int n = 0;
    for (int i = 0; i < r->nfixed; i++) r->items[n++] = r->fixed[i];
    for (int i = 0; i < r->nown; i++) r->items[n++] = r->own[i];
    if (r->more) r->items[n++] = r->more;
    r->rows = n;
    uui_dropdown_set_items(r->dd, r->items, n);
    uui_droprows_reselect(r);
}

void uui_droprows_init(struct uui_droprows *r, struct uui_dropdown *dd, int max_own,
                       const char *more) {
    memset(r, 0, sizeof *r);
    r->dd = dd;
    r->max_own = max_own < 0 ? 0 : max_own > UUI_DROPROWS_OWN ? UUI_DROPROWS_OWN : max_own;
    r->more = more;
    r->sel_kind = UUI_DROPROW_NONE;
    rebuild(r);
}

void uui_droprows_set_fixed(struct uui_droprows *r, const char *const *labels, int n) {
    if (n > UUI_DROPROWS_FIXED) n = UUI_DROPROWS_FIXED;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) r->fixed[i] = labels[i];
    r->nfixed = n;
    rebuild(r);
}

int uui_droprows_find_own(const struct uui_droprows *r, const char *label) {
    for (int i = 0; i < r->nown; i++) if (!strcmp(r->own[i], label)) return i;
    return -1;
}

const char *uui_droprows_own(const struct uui_droprows *r, int index) {
    return index >= 0 && index < r->nown ? r->own[index] : NULL;
}

int uui_droprows_add_own(struct uui_droprows *r, const char *label) {
    if (!label || !label[0] || strlen(label) >= UUI_DROPROWS_LABEL || !r->max_own) return 0;
    int at = uui_droprows_find_own(r, label);
    if (at < 0) at = r->nown < r->max_own ? r->nown++ : r->nown - 1;   // the oldest goes
    memmove(r->own[1], r->own[0], sizeof r->own[0] * (unsigned)at);
    strlcpy(r->own[0], label, sizeof r->own[0]);
    r->sel_kind = UUI_DROPROW_OWN;
    r->sel_index = 0;
    rebuild(r);
    return 1;
}

void uui_droprows_remove_own(struct uui_droprows *r, int index) {
    if (index < 0 || index >= r->nown) return;
    memmove(r->own[index], r->own[index + 1], sizeof r->own[0] * (unsigned)(r->nown - index - 1));
    r->nown--;
    if (r->sel_kind == UUI_DROPROW_OWN) {
        if (r->sel_index == index) r->sel_kind = UUI_DROPROW_NONE;
        else if (r->sel_index > index) r->sel_index--;
    }
    rebuild(r);
}

void uui_droprows_select(struct uui_droprows *r, int kind, int index) {
    if (kind == UUI_DROPROW_MORE || row_of(r, kind, index) < 0) return;
    r->sel_kind = kind;
    r->sel_index = index;
    uui_dropdown_set_selected(r->dd, row_of(r, kind, index));
}

void uui_droprows_reselect(struct uui_droprows *r) {
    uui_dropdown_set_selected(r->dd, row_of(r, r->sel_kind, r->sel_index));
}

int uui_droprows_kind(const struct uui_droprows *r, int row, int *index) {
    int kind = UUI_DROPROW_NONE, i = -1;
    if (row >= 0 && row < r->nfixed) kind = UUI_DROPROW_FIXED, i = row;
    else if (row >= r->nfixed && row < r->nfixed + r->nown) kind = UUI_DROPROW_OWN, i = row - r->nfixed;
    else if (r->more && row == r->nfixed + r->nown) kind = UUI_DROPROW_MORE, i = 0;
    if (index) *index = i;
    return kind;
}

int uui_droprows_picked(const struct uui_droprows *r, int *index) {
    return uui_droprows_kind(r, uui_dropdown_selected(r->dd), index);
}
