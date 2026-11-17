/* SaunaOS's SDL video driver: the device, the display and the event
 * pump. See SDL_saunaos_video.h. */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PRIVATE

#include "SDL_saunaos_video.h"
#include "events/SDL_events_c.h"
#include "events/SDL_clipboardevents_c.h"
#include "media/uclip.h"
#include "query_abi.h"
#include "rt/sys.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"

struct uapp *SAUNAOS_app;

/* The app SDL's windows belong to: no toplevel, so each SDL window is a
 * uapp window with a taskbar button of its own. */
static struct uapp_desc g_desc;

/* --- the clipboard: text, through media/uclip.h ------------------------
 * The system clipboard is one selection of three kinds (files, text, a
 * picture); SDL hears only its text, as SDL's Windows driver hears only
 * CF_UNICODETEXT. No primary selection: SaunaOS has none. */

static struct uclip g_clip;   /* 64 KiB: never on the stack (uclip.h) */
static unsigned g_clip_serial;

static bool SAUNAOS_SetClipboardText(SDL_VideoDevice *_this, const char *text)
{
    (void)_this;
    size_t n = SDL_strlen(text);
    if (n == 0) {
        return uclip_clear() ? true : SDL_SetError("The clipboard service refused");
    }
    if (n > UCLIP_TEXT_MAX || !uclip_set_text(text, (int)n)) {
        return SDL_SetError("The clipboard holds at most %d bytes of text", UCLIP_TEXT_MAX);
    }
    g_clip_serial = uclip_peek_serial();   /* our own change is not an update */
    return true;
}

static char *SAUNAOS_GetClipboardText(SDL_VideoDevice *_this)
{
    (void)_this;
    int len = 0;
    const char *t = NULL;
    if (uclip_load(&g_clip) && uclip_op(&g_clip) != UCLIP_NONE) {
        t = uclip_text(&g_clip, &len);
    }
    char *out = (char *)SDL_malloc((size_t)len + 1);
    if (out) {
        if (t) {
            SDL_memcpy(out, t, (size_t)len);
        }
        out[len] = '\0';
    }
    return out;
}

static bool SAUNAOS_HasClipboardText(SDL_VideoDevice *_this)
{
    (void)_this;
    int len = 0;
    return uclip_load(&g_clip) && uclip_op(&g_clip) != UCLIP_NONE &&
           uclip_text(&g_clip, &len) && len > 0;
}

/* Another program's copy is SDL_EVENT_CLIPBOARD_UPDATE: one shared-memory
 * read per pump. */
static void check_clipboard(void)
{
    unsigned serial = uclip_peek_serial();
    if (serial != g_clip_serial) {
        g_clip_serial = serial;
        SDL_SendClipboardUpdate(false, NULL, 0);
    }
}

float SAUNAOS_Scale(void)
{
    int pct = ugfx_scale_pct();
    return pct > 0 ? pct / 100.0f : 1.0f;
}

/* A wake-up from another thread (SAUNAOS_SendWakeupEvent): returning
 * from the wait is all it is for. */
static int on_user(struct uapp *a, int a0, int a1)
{
    (void)a; (void)a0; (void)a1;
    return 0;
}

static bool SAUNAOS_VideoInit(SDL_VideoDevice *_this)
{
    (void)_this;
    g_desc.title = "SDL";
    /* The program's locale is its own: SDL_Init must not change it. */
    g_desc.flags = UAPP_NO_TOPLEVEL | UAPP_NO_SETLOCALE;
    g_desc.on_user = on_user;
    if (!uapp_open(&SAUNAOS_app, &g_desc)) {
        return SDL_SetError("SaunaOS: no desktop to open a window on");
    }

    /* The desktop's mode, in points (macOS's shape): a 1280x720 screen
     * at 125% is a 1024x576 display of pixel density 1.25. */
    int w = 1024, h = 768;
    float hz = 60.0f;
    struct query_display q;
    if (sys_query_record(QUERY_DISPLAY, 0, &q, sizeof q) >= (int)sizeof q && q.width && q.height) {
        w = (int)q.width;
        h = (int)q.height;
        if (q.refresh_mhz) {
            hz = q.refresh_mhz / 1000.0f;
        }
    }
    float scale = SAUNAOS_Scale();
    SDL_DisplayMode mode;
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_XRGB8888;
    mode.w = (int)SDL_lroundf(w / scale);
    mode.h = (int)SDL_lroundf(h / scale);
    mode.pixel_density = scale;
    mode.refresh_rate = hz;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        return false;
    }
    SAUNAOS_InitInput();
    g_clip_serial = uclip_peek_serial();
    return true;
}

static void SAUNAOS_VideoQuit(SDL_VideoDevice *_this)
{
    (void)_this;
    if (SAUNAOS_app) {
        uapp_close(SAUNAOS_app);
        SAUNAOS_app = NULL;
    }
}

static int g_pump_depth;

bool SAUNAOS_InPump(void)
{
    return g_pump_depth > 0;
}

int SAUNAOS_Pump(int timeout_ms)
{
    static bool told;
    if (!SAUNAOS_app) {
        return -1;
    }
    /* NOT FROM INSIDE ONE: an SDL event watcher runs within uapp_pump()'s
     * dispatch, and a pump from there (SDL_UpdateWindowSurface,
     * SDL_PumpEvents) would hand SDL the next events in the middle of
     * this one. The outer pump paints what was marked. */
    if (g_pump_depth) {
        return 0;
    }
    g_pump_depth++;
    int handled = uapp_pump(SAUNAOS_app, timeout_ms);
    g_pump_depth--;
    check_clipboard();
    if (handled < 0 && !told) {
        told = true;
        SDL_SendQuit();
    }
    return handled;
}

static void SAUNAOS_PumpEvents(SDL_VideoDevice *_this)
{
    (void)_this;
    SAUNAOS_Pump(0);
}

/* 1 when something arrived (an event, a wake-up, the end), 0 when the
 * whole time passed. */
static int SAUNAOS_WaitEventTimeout(SDL_VideoDevice *_this, Sint64 timeoutNS)
{
    (void)_this;
    if (!SAUNAOS_app) {
        return -1;
    }
    int ms = timeoutNS < 0 ? -1 : (int)((timeoutNS + 999999) / 1000000);
    return SAUNAOS_Pump(ms) != 0 ? 1 : 0;
}

/* Any thread: uapp_post() is uapp's one call a worker may make. */
static void SAUNAOS_SendWakeupEvent(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this; (void)window;
    if (SAUNAOS_app) {
        uapp_post(SAUNAOS_app, 0, 0);
    }
}

static void SAUNAOS_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

/* No desktop -- a text terminal, a test with none running -- is no
 * driver, so SDL goes on to the next (X11's and Wayland's rule without a
 * display) rather than failing SDL_Init. */
static SDL_VideoDevice *SAUNAOS_CreateDevice(void)
{
    if (!uapp_has_desktop()) {
        return NULL;
    }
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    device->VideoInit = SAUNAOS_VideoInit;
    device->VideoQuit = SAUNAOS_VideoQuit;
    device->PumpEvents = SAUNAOS_PumpEvents;
    device->WaitEventTimeout = SAUNAOS_WaitEventTimeout;
    device->SendWakeupEvent = SAUNAOS_SendWakeupEvent;
    device->GetWindowSizeInPixels = SAUNAOS_GetWindowSizeInPixels;
    device->ShowMessageBox = SAUNAOS_ShowMessageBox;
    device->SetClipboardText = SAUNAOS_SetClipboardText;
    device->GetClipboardText = SAUNAOS_GetClipboardText;
    device->HasClipboardText = SAUNAOS_HasClipboardText;
    SAUNAOS_SetWindowHooks(device);
    device->free = SAUNAOS_DeleteDevice;
    return device;
}

VideoBootStrap PRIVATE_bootstrap = {
    "saunaos", "SaunaOS desktop",
    SAUNAOS_CreateDevice,
    SAUNAOS_ShowMessageBoxAlone,
    false
};

#endif /* SDL_VIDEO_DRIVER_PRIVATE */
