#ifndef WM_THUMB_H
#define WM_THUMB_H

// A WINDOW'S THUMBNAIL: its client buffer box-averaged into a small
// image, held by `open_seq` and rescaled when the client presents --
// at most every WM_THUMB_RESCALE_TICKS, never per frame. Shared by the
// taskbar's peek card and the Alt+Tab switcher, which each keep their
// own entries (different boxes) and call wm_thumb_update() per frame.
//
// CONTENT ONLY, no chrome: the cards draw their own title row, and a
// scaled title bar is a stripe of illegible text. A kernel-space app
// window and a client that has not presented have no buffer -- the
// caller draws the app's icon instead (wm_thumb_can_show() says which).

#include <stdint.h>
#include "lib/uimg.h"

// A client presenting at 60 Hz must not cost a full rescale a frame.
#define WM_THUMB_RESCALE_TICKS 20

struct wm_thumb {
    uint32_t seq;          // struct window.open_seq -- never an index
    struct uimg img;       // px NULL until first scaled
    int dirty;
    uint64_t scaled_at;
};

// Does windows[idx] have a buffer to scale? A ring-3 client that has
// presented at least one frame.
int wm_thumb_can_show(int idx);

// Brings t up to date for a bw x bh box (fitted, aspect kept). Returns 1
// when its pixels changed -- the caller damages where it draws it.
int wm_thumb_update(struct wm_thumb *t, int bw, int bh, uint64_t now);

// Points t at a window, stale; frees any old pixels.
void wm_thumb_reset(struct wm_thumb *t, uint32_t seq);
void wm_thumb_free(struct wm_thumb *t);

// A client presented a frame (wm_client.c): every holder's thumbnail of
// it is stale. Fans out to each card that keeps thumbnails.
void wm_thumb_presented(uint32_t open_seq);

#endif // WM_THUMB_H
