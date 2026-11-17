/* SaunaOS's SDL video driver: windows and their framebuffer. See
 * SDL_saunaos_video.h. */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PRIVATE

#include "SDL_saunaos_video.h"
#include "events/SDL_windowevents_c.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "win_proto.h"
#include "events/SDL_keyboard_c.h"

static int to_px(int points)
{
    int px = (int)SDL_lroundf(points * SAUNAOS_Scale());
    return px > 0 ? px : 1;
}

void SAUNAOS_GetWindowSizeInPixels(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    if (window->flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) {
        /* The window's real size while it is open: points times the
         * scale does not round-trip (1002 px -> 802 points -> 1003 px),
         * and a framebuffer a pixel off is resampled every frame. */
        if (data && data->uw) {
            *w = uapp_window_width(data->uw);
            *h = uapp_window_height(data->uw);
        } else {
            *w = to_px(window->w);
            *h = to_px(window->h);
        }
    } else {
        *w = window->w;
        *h = window->h;
    }
}

/* SDL's picture into the window: copied when the sizes agree, scaled
 * sharp-bilinear when they do not (a window that drew in points). */
static void draw(struct uapp_window *uw, struct uapp_draw *d)
{
    SDL_WindowData *data = (SDL_WindowData *)uapp_window_state(uw);
    struct ugfx_surface *s = uapp_surface(d);
    if (!data->fb) {
        ugfx_fill_rect(s, 0, 0, s->w, s->h, 0);
        return;
    }
    if (data->fb_w == s->w && data->fb_h == s->h) {
        SDL_memcpy(s->pixels, data->fb, (size_t)s->w * s->h * 4);
    } else if (upixscale_prepare(&data->scaler, data->fb_w, data->fb_h, s->w, s->h)) {
        upixscale_run(&data->scaler, data->fb, data->fb_w, s->pixels, s->w);
    }
}

/* The uapp window for an SDL window being shown. */
static bool open_uw(SDL_WindowData *data)
{
    SDL_Window *window = data->window;
    struct uapp_window_desc desc;
    SDL_zero(desc);
    desc.title = window->title ? window->title : "";
    desc.w = to_px(window->w);
    desc.h = to_px(window->h);
    if (window->flags & SDL_WINDOW_RESIZABLE) {
        desc.flags |= UAPP_WIN_RESIZABLE;
        desc.min_w = window->min_w > 0 ? to_px(window->min_w) : 1;
        desc.min_h = window->min_h > 0 ? to_px(window->min_h) : 1;
    }
    if (window->flags & SDL_WINDOW_BORDERLESS) {
        desc.flags |= UAPP_WIN_NO_DECOR;
    }
    desc.state = data;
    desc.on_draw = draw;
    SAUNAOS_SetInputCallbacks(&desc);
    data->uw = uapp_window_open(SAUNAOS_app, &desc);
    if (!data->uw) {
        return SDL_SetError("SaunaOS: the desktop refused a window");
    }
    uapp_window_set_cursor(data->uw, SAUNAOS_CursorShape());
    /* Frontmost the moment it opens, so it has the keyboard. */
    SDL_SetKeyboardFocus(window);
    return true;
}

static bool SAUNAOS_CreateSDLWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props)
{
    (void)_this; (void)props;
    SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return false;
    }
    data->window = window;
    window->internal = data;
    if (!(window->flags & SDL_WINDOW_HIDDEN) && !open_uw(data)) {
        window->internal = NULL;
        SDL_free(data);
        return false;
    }
    return true;
}

static void SAUNAOS_ShowWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    if (data && !data->uw) {
        open_uw(data);
    }
}

static void SAUNAOS_HideWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    if (data && data->uw) {
        uapp_window_close(data->uw);
        data->uw = NULL;
    }
}

static void SAUNAOS_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *data = window->internal;
    if (!data) {
        return;
    }
    SAUNAOS_HideWindow(_this, window);
    upixscale_free(&data->scaler);
    SDL_free(data->fb);
    SDL_free(data);
    window->internal = NULL;
}

static void SAUNAOS_SetWindowTitle(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    if (data && data->uw) {
        uapp_window_set_title(data->uw, window->title ? window->title : "");
    }
}

/* The program's own resize: uapp reports none back, so SDL hears it here. */
static void SAUNAOS_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    int w = window->pending.w, h = window->pending.h;
    if (data && data->uw) {
        uapp_window_resize(data->uw, to_px(w), to_px(h));
    }
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, w, h);
}

/* Fullscreen on the desktop's own mode, the only one: no mode changes. */
static SDL_FullscreenResult SAUNAOS_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window,
                                                        SDL_VideoDisplay *display, SDL_FullscreenOp op)
{
    (void)_this; (void)display;
    SDL_WindowData *data = window->internal;
    if (!data || !data->uw) {
        return SDL_FULLSCREEN_FAILED;
    }
    uapp_window_set_fullscreen(data->uw, op == SDL_FULLSCREEN_OP_ENTER);
    return SDL_FULLSCREEN_SUCCEEDED;
}

static bool SAUNAOS_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                            SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_WindowData *data = window->internal;
    int w, h;
    SAUNAOS_GetWindowSizeInPixels(_this, window, &w, &h);
    SDL_free(data->fb);
    data->fb = (Uint32 *)SDL_calloc((size_t)w * h, 4);
    if (!data->fb) {
        data->fb_w = data->fb_h = 0;
        return false;
    }
    data->fb_w = w;
    data->fb_h = h;
    *format = SDL_PIXELFORMAT_XRGB8888;
    *pixels = data->fb;
    *pitch = w * 4;
    return true;
}

/* A frame is done: paint it now rather than at the program's next pump,
 * which a game busy with its next frame may be a while reaching. From an
 * event watcher, inside a pump, SAUNAOS_Pump() declines and that pump
 * paints it. */
static bool SAUNAOS_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                            const SDL_Rect *rects, int numrects)
{
    (void)_this; (void)rects; (void)numrects;
    SDL_WindowData *data = window->internal;
    if (data && data->uw) {
        uapp_window_redraw(data->uw);
        SAUNAOS_Pump(0);
    }
    return true;
}

/* Asked at each press instead (SDL_saunaos_events.c's hit_test()). */
static bool SAUNAOS_SetWindowHitTest(SDL_Window *window, bool enabled)
{
    (void)window; (void)enabled;
    return true;
}

static void SAUNAOS_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    if (data) {
        SDL_free(data->fb);
        data->fb = NULL;
        data->fb_w = data->fb_h = 0;
    }
}

void SAUNAOS_SetWindowHooks(SDL_VideoDevice *device)
{
    device->CreateSDLWindow = SAUNAOS_CreateSDLWindow;
    device->ShowWindow = SAUNAOS_ShowWindow;
    device->HideWindow = SAUNAOS_HideWindow;
    device->DestroyWindow = SAUNAOS_DestroyWindow;
    device->SetWindowTitle = SAUNAOS_SetWindowTitle;
    device->SetWindowSize = SAUNAOS_SetWindowSize;
    device->SetWindowFullscreen = SAUNAOS_SetWindowFullscreen;
    device->CreateWindowFramebuffer = SAUNAOS_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SAUNAOS_UpdateWindowFramebuffer;
    device->SetWindowHitTest = SAUNAOS_SetWindowHitTest;
    device->DestroyWindowFramebuffer = SAUNAOS_DestroyWindowFramebuffer;
}

#endif /* SDL_VIDEO_DRIVER_PRIVATE */
