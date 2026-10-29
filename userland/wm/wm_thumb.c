// Window thumbnails -- see wm_thumb.h.
#include "wm_thumb.h"
#include "wm_internal.h"
#include "wm_peek.h"
#include "wm_switcher.h"

int wm_thumb_can_show(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    const struct window *w = &windows[idx];
    return wm_client_is_client_window(w) && w->client_buf &&
           w->client_w > 0 && w->client_h > 0 &&
           w->client_gen[w->client_front] != 0;
}

void wm_thumb_free(struct wm_thumb *t) {
    if (t->img.px) uimg_free(&t->img);
    t->img.px = 0;
    t->img.w = t->img.h = 0;
}

void wm_thumb_reset(struct wm_thumb *t, uint32_t seq) {
    wm_thumb_free(t);
    t->seq = seq;
    t->dirty = 1;
    t->scaled_at = 0;
}

// Box-averaged by uimg_scale(), which keeps a scaled-down window readable
// as a window rather than as nearest-neighbour noise.
int wm_thumb_update(struct wm_thumb *t, int bw, int bh, uint64_t now) {
    int idx = wm_window_by_seq(t->seq);
    if (!wm_thumb_can_show(idx) || bw <= 0 || bh <= 0) return 0;
    const struct window *w = &windows[idx];
    int tw, th;
    uimg_fit_size(w->client_w, w->client_h, bw, bh, UIMG_FIT_CONTAIN, &tw, &th);
    int resized = !t->img.px || t->img.w != tw || t->img.h != th;
    if (!resized && !t->dirty) return 0;
    if (!resized && now - t->scaled_at < WM_THUMB_RESCALE_TICKS) return 0;   // stays dirty
    struct uimg src = { w->client_w, w->client_h, w->client_buf, 0 };
    struct uimg out = { 0, 0, 0, 0 };
    if (uimg_scale(&src, tw, th, &out) != 0) return 0;
    if (t->img.px) uimg_free(&t->img);
    t->img = out;
    t->dirty = 0;
    t->scaled_at = now;
    return 1;
}

void wm_thumb_presented(uint32_t open_seq) {
    wm_peek_presented(open_seq);
    wm_switcher_presented(open_seq);
}
