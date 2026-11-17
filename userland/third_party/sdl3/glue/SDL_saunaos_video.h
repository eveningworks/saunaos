/* SaunaOS's SDL video driver (SDL_VIDEO_DRIVER_PRIVATE): each SDL window
 * is a uapp window of a no-toplevel app, driven through uapp's step API
 * (uapp_open/uapp_pump/uapp_close). Shared by SDL_saunaos_video.c (the
 * device, the display, the event pump), SDL_saunaos_window.c (windows
 * and their framebuffer) and SDL_saunaos_events.c (input).
 *
 * COORDINATES ARE POINTS, macOS's model: an SDL window of 640x400 is a
 * uapp window of 640x400 x display.scale pixels. A window that did not
 * ask for SDL_WINDOW_HIGH_PIXEL_DENSITY draws 640x400 and is scaled
 * sharp-bilinear (base/upixscale.h); one that did draws the pixels. */
#ifndef SDL_saunaos_video_h_
#define SDL_saunaos_video_h_

#include "SDL_internal.h"
#include "video/SDL_sysvideo.h"
#include "base/upixscale.h"

struct uapp;
struct uapp_window;

struct SDL_WindowData {
    SDL_Window *window;
    struct uapp_window *uw;   /* NULL while the SDL window is hidden */
    Uint32 *fb;               /* SDL's framebuffer, fb_w x fb_h, XRGB8888 */
    int fb_w, fb_h;
    struct upixscale scaler;  /* fb -> the window's pixels, when they differ */
    unsigned buttons;         /* WIN_MOUSE_BTN_* held, to turn masks into edges */
};

extern struct uapp *SAUNAOS_app;

/* display.scale as a factor (1.25 at 125%). */
float SAUNAOS_Scale(void);

/* uapp_pump(), and SDL_EVENT_QUIT once if that says the app is over
 * (the desktop went away). What uapp_pump() returned. */
int SAUNAOS_Pump(int timeout_ms);

/* Is a SAUNAOS_Pump() under way: an SDL event watcher runs inside one. */
bool SAUNAOS_InPump(void);

/* SDL_saunaos_messagebox.c: the device's hook, and the bootstrap's, which
 * needs no driver up. */
bool SAUNAOS_ShowMessageBox(SDL_VideoDevice *_this, const SDL_MessageBoxData *data, int *buttonID);
bool SAUNAOS_ShowMessageBoxAlone(const SDL_MessageBoxData *data, int *buttonID);

/* The size SDL draws a window at: its pixels when it asked for them,
 * else its size in points. */
void SAUNAOS_GetWindowSizeInPixels(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h);

/* SDL_saunaos_window.c */
void SAUNAOS_SetWindowHooks(SDL_VideoDevice *device);

/* SDL_saunaos_events.c: the keyboard and mouse SDL is told about, the
 * cursor hooks, and the input callbacks of a window's description. */
void SAUNAOS_InitInput(void);
/* The WIN_CURSOR_* SDL's current cursor is, WIN_CURSOR_NONE when hidden. */
int SAUNAOS_CursorShape(void);
struct uapp_window_desc;
void SAUNAOS_SetInputCallbacks(struct uapp_window_desc *desc);
/* A window closing or hiding: its pointer lock goes with it. */
void SAUNAOS_ForgetWindow(SDL_Window *window);

#endif
