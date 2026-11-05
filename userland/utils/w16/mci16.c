// mci16.c -- MMSYSTEM's Media Control Interface: mciSendCommand, mciSendString and
// mciGetErrorString, answered by w16 itself with an AVIVideo device over media/uvid_play.h.
//
// WINE'S SHAPE, NOT 3.1'S: Windows ran a driver per device type (MCIAVI.DRV, MCIWAVE.DRV) that
// MMSYSTEM loaded from SYSTEM.INI's [mci]; w16 answers the commands in C, as Wine's winmm does with
// its own mciavi, and never runs the driver a program ships (DK's encyclopaedias bring MCIAVI.DRV).
// One device type so far: AVIVideo, chosen by `type avivideo` or an element ending in .avi -- WIN.INI's
// [mci extensions] avi=AVIVideo.
//
// THE PICTURES ARE DRAWN ON THE TASK THREAD (w16_mci_service), where the program's own drawing
// happens, into the window MCI_WINDOW named or the device's own; the main thread's tick only wakes
// the task while a device plays. The sound is uvid_play's streaming voice, beside waveOut's pushed
// one, and the clock the pictures follow.
//
// MCIAVI'S CONVENTIONS kept: the time format starts as FRAMES; MCI_PUT and MCI_WHERE rectangles are
// an origin and an EXTENT (rc.right and rc.bottom a width and height); the default destination is the
// video's own size at the window's 0, 0.
#include "w16_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media/uvid.h"
#include "media/uvid_play.h"
#include "ui/ugfx.h"

#define MM_MCINOTIFY 0x03B9
#define MCI_NOTIFY_SUCCESSFUL 1
#define MCI_NOTIFY_SUPERSEDED 2
#define MCI_NOTIFY_ABORTED    4
#define MCI_NOTIFY_FAILURE    8

#define MCI_OPEN       0x0803
#define MCI_CLOSE      0x0804
#define MCI_PLAY       0x0806
#define MCI_SEEK       0x0807
#define MCI_STOP       0x0808
#define MCI_PAUSE      0x0809
#define MCI_INFO       0x080A
#define MCI_GETDEVCAPS 0x080B
#define MCI_SET        0x080D
#define MCI_STEP       0x080E
#define MCI_BREAK      0x0811
#define MCI_STATUS     0x0814
#define MCI_CUE        0x0830
#define MCI_REALIZE    0x0840
#define MCI_WINDOW     0x0841
#define MCI_PUT        0x0842
#define MCI_WHERE      0x0843
#define MCI_UPDATE     0x0854
#define MCI_RESUME     0x0855
#define MCI_SETAUDIO   0x0873   // digital video's (digitalv.h), which MCIAVI took too
#define MCI_SETVIDEO   0x0876
#define MCI_ALL_DEVICE_ID 0xFFFF

#define MCI_NOTIFY 0x00000001u
#define MCI_WAIT   0x00000002u
#define MCI_FROM   0x00000004u
#define MCI_TO     0x00000008u
#define MCI_OPEN_SHAREABLE 0x00000100u
#define MCI_OPEN_ELEMENT   0x00000200u
#define MCI_OPEN_ALIAS     0x00000400u
#define MCI_OPEN_TYPE_ID   0x00001000u
#define MCI_OPEN_TYPE      0x00002000u
#define MCI_SEEK_TO_START  0x00000100u
#define MCI_SEEK_TO_END    0x00000200u
#define MCI_STATUS_ITEM    0x00000100u
#define MCI_STATUS_START   0x00000200u
#define MCI_SET_TIME_FORMAT 0x00000400u
#define MCI_SET_AUDIO      0x00000800u
#define MCI_SET_VIDEO      0x00001000u
#define MCI_SET_ON         0x00002000u
#define MCI_SET_OFF        0x00004000u
#define MCI_INFO_PRODUCT   0x00000100u
#define MCI_INFO_FILE      0x00000200u
#define MCI_GETDEVCAPS_ITEM 0x00000100u
#define MCI_ANIM_OPEN_WS     0x00010000u
#define MCI_ANIM_OPEN_PARENT 0x00020000u
#define MCI_ANIM_STEP_REVERSE 0x00010000u
#define MCI_ANIM_STEP_FRAMES  0x00020000u
#define MCI_ANIM_INFO_TEXT   0x00010000u
#define MCI_ANIM_WINDOW_HWND  0x00010000u
#define MCI_ANIM_WINDOW_STATE 0x00040000u
#define MCI_ANIM_WINDOW_TEXT  0x00080000u
#define MCI_ANIM_RECT         0x00010000u
#define MCI_ANIM_PUT_SOURCE   0x00020000u
#define MCI_ANIM_PUT_DESTINATION 0x00040000u
#define MCI_ANIM_UPDATE_HDC   0x00020000u

#define MCI_STATUS_LENGTH     1
#define MCI_STATUS_POSITION   2
#define MCI_STATUS_NUMBER_OF_TRACKS 3
#define MCI_STATUS_MODE       4
#define MCI_STATUS_MEDIA_PRESENT 5
#define MCI_STATUS_TIME_FORMAT 6
#define MCI_STATUS_READY      7
#define MCI_STATUS_CURRENT_TRACK 8
#define MCI_ANIM_STATUS_SPEED   0x4001
#define MCI_ANIM_STATUS_FORWARD 0x4002
#define MCI_ANIM_STATUS_HWND    0x4003
#define MCI_ANIM_STATUS_HPAL    0x4004
#define MCI_ANIM_STATUS_STRETCH 0x4005

#define MCI_GETDEVCAPS_CAN_RECORD 1
#define MCI_GETDEVCAPS_HAS_AUDIO  2
#define MCI_GETDEVCAPS_HAS_VIDEO  3
#define MCI_GETDEVCAPS_DEVICE_TYPE 4
#define MCI_GETDEVCAPS_USES_FILES 5
#define MCI_GETDEVCAPS_COMPOUND_DEVICE 6
#define MCI_GETDEVCAPS_CAN_EJECT  7
#define MCI_GETDEVCAPS_CAN_PLAY   8
#define MCI_GETDEVCAPS_CAN_SAVE   9
#define MCI_ANIM_GETDEVCAPS_CAN_REVERSE 0x4001
#define MCI_ANIM_GETDEVCAPS_FAST_RATE   0x4002
#define MCI_ANIM_GETDEVCAPS_SLOW_RATE   0x4003
#define MCI_ANIM_GETDEVCAPS_NORMAL_RATE 0x4004
#define MCI_ANIM_GETDEVCAPS_PALETTES    0x4006
#define MCI_ANIM_GETDEVCAPS_CAN_STRETCH 0x4007
#define MCI_ANIM_GETDEVCAPS_MAX_WINDOWS 0x4008
#define MCI_DEVTYPE_DIGITAL_VIDEO 520

#define MCI_STRING_OFFSET 512
#define MCI_MODE_STOP  (MCI_STRING_OFFSET + 13)
#define MCI_MODE_PLAY  (MCI_STRING_OFFSET + 14)
#define MCI_MODE_PAUSE (MCI_STRING_OFFSET + 17)
#define MCI_FORMAT_MILLISECONDS 0
#define MCI_FORMAT_FRAMES 3

#define MCIERR_BASE 256
#define MCIERR_INVALID_DEVICE_ID    (MCIERR_BASE + 1)
#define MCIERR_UNRECOGNIZED_KEYWORD (MCIERR_BASE + 3)
#define MCIERR_UNRECOGNIZED_COMMAND (MCIERR_BASE + 5)
#define MCIERR_INVALID_DEVICE_NAME  (MCIERR_BASE + 7)
#define MCIERR_OUT_OF_MEMORY        (MCIERR_BASE + 8)
#define MCIERR_PARAM_OVERFLOW       (MCIERR_BASE + 12)
#define MCIERR_MISSING_STRING_ARGUMENT (MCIERR_BASE + 13)
#define MCIERR_BAD_INTEGER          (MCIERR_BASE + 14)
#define MCIERR_MISSING_PARAMETER    (MCIERR_BASE + 17)
#define MCIERR_UNSUPPORTED_FUNCTION (MCIERR_BASE + 18)
#define MCIERR_FILE_NOT_FOUND       (MCIERR_BASE + 19)
#define MCIERR_OUTOFRANGE           (MCIERR_BASE + 26)
#define MCIERR_DEVICE_TYPE_REQUIRED (MCIERR_BASE + 31)
#define MCIERR_DUPLICATE_ALIAS      (MCIERR_BASE + 33)
#define MCIERR_BAD_CONSTANT         (MCIERR_BASE + 34)
#define MCIERR_MISSING_DEVICE_NAME  (MCIERR_BASE + 36)
#define MCIERR_BAD_TIME_FORMAT      (MCIERR_BASE + 37)
#define MCIERR_INVALID_FILE         (MCIERR_BASE + 40)
#define MCIERR_NULL_PARAMETER_BLOCK (MCIERR_BASE + 41)
#define MCIERR_FILENAME_REQUIRED    (MCIERR_BASE + 48)
#define MCIERR_EXTRA_CHARACTERS     (MCIERR_BASE + 49)
#define MCIERR_DEVICE_NOT_INSTALLED (MCIERR_BASE + 50)

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };

struct mcidev {
    uint16_t id;                 // 0: a free slot
    char *alias;                 // what mciSendString names it by: its alias, else its element as given
    char *element;               // the element's DOS name, as given
    struct uvid_play *play;
    struct uvid_info info;
    uint32_t frames;             // its length in frames
    unsigned time_format;
    int state;
    uint32_t to_ms;              // MCI_TO: where this play stops; 0 the end
    uint16_t notify_hwnd;        // MCI_NOTIFY's window, owed MM_MCINOTIFY; 0 none
    uint16_t hwnd;               // the window drawn into: MCI_WINDOW's, else `own`
    uint16_t own;                // the device's own window, 0 until one is made
    int16_t src[4], dst[4];      // MCI_PUT's, as origin and extent
    int video_off;               // MCI_SET_VIDEO with MCI_SET_OFF
    uint32_t drawn;              // the frame's uvid_play_serial() last drawn
    uint32_t await_serial;       // a seek's: the serial it was asked at, until the frame changes
    uint64_t await_ms;           // ...and when, so a frame that never comes stops being waited for
    int awaiting;
};

static struct mcidev *g_dev;     // grown as devices open
static int g_ndev;
static int g_playing;            // devices playing: the main thread's tick wakes the task for them
static int g_awaiting;           // devices stopped on a seek whose frame is not drawn yet: the same
static int g_due;                // set by that tick, cleared by the service (both under `lock`)
static int g_wnd_class = -1;     // the devices' own windows' class, made on first need

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...) {
    if (!g_w16_trace) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("  mci: ", g_w16_trace);
    vfprintf(g_w16_trace, fmt, ap);
    fputc('\n', g_w16_trace);
    fflush(g_w16_trace);
    va_end(ap);
}

// --- the program's memory ---------------------------------------------------------

static uint8_t *far_ptr(struct w16 *w, uint32_t far, uint32_t len, int write) {
    return far >> 16 ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, len, write) : 0;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// A string the program passed, allocated; NULL when it is not readable.
static char *far_str(struct w16 *w, uint32_t far) {
    if (!(far >> 16)) return 0;
    char buf[512];                               // MCI's own bound: a command line is at most 256
    if (cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, buf, sizeof buf) < 0) return 0;
    return strdup(buf);
}

// A string into the program's buffer, cut to fit: MCIERR_PARAM_OVERFLOW when it did not.
static unsigned put_str(struct w16 *w, uint32_t far, unsigned cap, const char *s) {
    if (!(far >> 16) || !cap) return 0;
    uint8_t *p = far_ptr(w, far, cap, 1);
    if (!p) return 0;
    size_t n = strlen(s);
    unsigned fit = n < cap ? (unsigned)n : cap - 1;
    memcpy(p, s, fit);
    p[fit] = 0;
    return n < cap ? 0 : MCIERR_PARAM_OVERFLOW;
}

// --- devices ------------------------------------------------------------------------

static struct mcidev *dev_of(uint16_t id) {
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].id && g_dev[i].id == id) return &g_dev[i];
    return 0;
}

static struct mcidev *dev_named(const char *name) {
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].id && g_dev[i].alias && !strcasecmp(g_dev[i].alias, name)) return &g_dev[i];
    return 0;
}

static struct mcidev *dev_new(void) {
    uint16_t id = 1;
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].id >= id) id = (uint16_t)(g_dev[i].id + 1);
    for (int i = 0; i < g_ndev; i++)
        if (!g_dev[i].id) { memset(&g_dev[i], 0, sizeof g_dev[i]); g_dev[i].id = id; return &g_dev[i]; }
    struct mcidev *g = realloc(g_dev, sizeof *g * (size_t)(g_ndev + 1));
    if (!g) return 0;
    g_dev = g;
    memset(&g_dev[g_ndev], 0, sizeof g_dev[0]);
    g_dev[g_ndev].id = id;
    return &g_dev[g_ndev++];
}

static uint32_t ms_of_frame(const struct mcidev *d, uint32_t f) {
    if (!d->info.fps_num) return f;
    return (uint32_t)((uint64_t)f * 1000 * d->info.fps_den / d->info.fps_num);
}

static uint32_t frame_of_ms(const struct mcidev *d, uint32_t ms) {
    if (!d->info.fps_num) return ms;
    return (uint32_t)((uint64_t)ms * d->info.fps_num / ((uint64_t)1000 * d->info.fps_den));
}

// A position in the device's time format, to and from milliseconds.
static uint32_t to_ms(const struct mcidev *d, uint32_t v) { return d->time_format == MCI_FORMAT_FRAMES ? ms_of_frame(d, v) : v; }
static uint32_t from_ms(const struct mcidev *d, uint32_t ms) { return d->time_format == MCI_FORMAT_FRAMES ? frame_of_ms(d, ms) : ms; }
static uint32_t length(const struct mcidev *d) { return d->time_format == MCI_FORMAT_FRAMES ? d->frames : d->info.ms; }

static void set_playing(struct w16 *w, struct mcidev *d, int st) {
    int was = d->state == ST_PLAYING;
    d->state = st;
    if (d->play) uvid_play_set_paused(d->play, st != ST_PLAYING);
    pthread_mutex_lock(&w->lock);
    g_playing += (st == ST_PLAYING) - was;
    pthread_mutex_unlock(&w->lock);
}

// A STOPPED DEVICE SHOWS THE FRAME IT WAS MOVED TO, as MCIAVI did: the decoder makes it in its own
// time, so the task is woken until it has (or two seconds have gone).
static void await_frame(struct w16 *w, struct mcidev *d) {
    pthread_mutex_lock(&w->lock);
    if (!d->awaiting) g_awaiting++;
    d->awaiting = 1;
    d->await_serial = uvid_play_serial(d->play);
    d->await_ms = w16_now_ms();
    pthread_mutex_unlock(&w->lock);
}

static void awaited(struct w16 *w, struct mcidev *d) {
    pthread_mutex_lock(&w->lock);
    if (d->awaiting) g_awaiting--;
    d->awaiting = 0;
    pthread_mutex_unlock(&w->lock);
}

// MM_MCINOTIFY for a command asked with MCI_NOTIFY, to the window in its dwCallback.
static void notify(struct w16 *w, uint16_t hwnd, unsigned how, uint16_t id) {
    if (hwnd) w16_post_msg(w, hwnd, MM_MCINOTIFY, (uint16_t)how, id);
}

// A play's pending notification answered: by its end, or by what overtook it.
static void settle(struct w16 *w, struct mcidev *d, unsigned how) {
    if (!d->notify_hwnd) return;
    notify(w, d->notify_hwnd, how, d->id);
    d->notify_hwnd = 0;
}

// --- drawing --------------------------------------------------------------------------

// The device's current picture into its window at its destination, scaled from its source.
static void draw(struct w16 *w, struct mcidev *d) {
    const struct uvid_frame *f = d->play ? uvid_play_frame(d->play) : 0;
    struct w16_wnd *x = d->hwnd ? w16_wnd_get(w, d->hwnd) : 0;
    if (!f || !x || d->video_off) return;
    int dw = d->dst[2], dh = d->dst[3];
    if (dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096) return;
    int sx = d->src[0], sy = d->src[1], sw = d->src[2] > 0 ? d->src[2] : f->w, sh = d->src[3] > 0 ? d->src[3] : f->h;
    uint32_t *px = malloc((size_t)dw * dh * 4);
    if (!px) return;
    uvid_frame_draw(f, sx, sy, sw, sh, px, dw, dw, dh, UVID_FAST);
    struct ugfx_surface s;
    int ox, oy;
    if (w16_wnd_paint(w, x, &s, &ox, &oy)) {
        ugfx_blit(&s, ox + d->dst[0], oy + d->dst[1], dw, dh, px, dw);
        w16_wnd_paint_done(w);
    }
    free(px);
    d->drawn = uvid_play_serial(d->play);
}

// The devices' own windows: black, the picture laid on (MCIAVI's "AVIWnd").
static void paint_own(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    (void)w;
    ugfx_fill_rect(s, ox, oy, x->cw, x->ch, 0xFF000000u);
}

static uint32_t own_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        uint16_t h = x->hwnd;
        w16_ctl_paint(w, x, paint_own);
        for (int i = 0; i < g_ndev; i++)
            if (g_dev[i].id && g_dev[i].hwnd == h) draw(w, &g_dev[i]);
        return 0;
    }
    return w16_def_window_proc(w, x, msg, wp, lp);
}

// The device's own window, as MCIAVI made one: a child of `parent` with `style` (MCI_ANIM_OPEN_PARENT,
// MCI_ANIM_OPEN_WS), else an overlapped window titled with the element -- hidden until shown or played.
static uint16_t make_own(struct w16 *w, struct mcidev *d, uint16_t parent, uint32_t style, int have_style) {
    if (g_wnd_class < 0) g_wnd_class = w16_class_add_host(w, "AVIWnd", own_proc, 0, 0);
    if (g_wnd_class < 0) return 0;
    const char *title = d->element ? d->element : "";
    const char *base = strrchr(title, '\\');
    title = base ? base + 1 : title;
    int vw = d->info.w > 0 ? d->info.w : 160, vh = d->info.h > 0 ? d->info.h : 120;
    struct w16_wnd *x;
    if (parent && (!have_style || (style & WS_CHILD))) {
        x = w16_wnd_create(w, g_wnd_class, title, (have_style ? style : WS_CHILD) & ~WS_VISIBLE, 0, 0, vw, vh,
                           parent, 0, 0, 0);
    } else {
        uint32_t st = have_style ? style : 0x00CF0000u;   // WS_OVERLAPPEDWINDOW
        int f = w16_border_of(st), cap = (st & 0x00C00000u) == 0x00C00000u ? W16_CAPTION : 0;
        x = w16_popup_new(w, g_wnd_class, title, st & ~WS_VISIBLE, 0, 0, vw + 2 * f, vh + 2 * f + cap,
                          parent, 0, 0, 0);
    }
    return x ? x->hwnd : 0;
}

// The window a device shows in, shown -- its own, made now if it has none (a play with no MCI_WINDOW).
static void show_own(struct w16 *w, struct mcidev *d) {
    if (!d->hwnd) d->hwnd = d->own = make_own(w, d, 0, 0, 0);
    struct w16_wnd *x = d->own && d->hwnd == d->own ? w16_wnd_get(w, d->own) : 0;
    if (x && !x->visible) w16_show_window(w, x, 5);   // SW_SHOW
}

// --- the commands -------------------------------------------------------------------

static void close_dev(struct w16 *w, struct mcidev *d) {
    settle(w, d, MCI_NOTIFY_ABORTED);
    awaited(w, d);
    if (d->state == ST_PLAYING) set_playing(w, d, ST_STOPPED);
    if (d->play) uvid_play_close(d->play);
    if (d->own) {
        struct w16_wnd *x = w16_wnd_get(w, d->own);
        if (x) w16_wnd_destroy(w, x);
    }
    free(d->alias);
    free(d->element);
    memset(d, 0, sizeof *d);
}

// Opens an AVIVideo device on `element` (a DOS name): 0, or an MCIERR_.
static unsigned open_avi(struct mcidev *d, const char *element) {
    if (!element || !element[0]) return MCIERR_FILENAME_REQUIRED;
    char *host = malloc(PATH_MAX);
    if (!host) return MCIERR_OUT_OF_MEMORY;
    if (w16_dos_path(element, host, PATH_MAX, 0, 0, 0) != 0) { free(host); return MCIERR_FILE_NOT_FOUND; }
    int r = uvid_play_open(&d->play, host, 0);
    trace("open %s (%s): %d %s", element, host, r, r < 0 ? uvid_last_error() : "");
    free(host);
    if (r == -ENOENT) return MCIERR_FILE_NOT_FOUND;
    if (r == -ENOMEM) return MCIERR_OUT_OF_MEMORY;
    if (r < 0) return MCIERR_INVALID_FILE;
    d->info = *uvid_play_info(d->play);
    d->frames = d->info.frames ? d->info.frames : d->info.fps_den ? frame_of_ms(d, d->info.ms) : d->info.ms;
    uvid_play_set_paused(d->play, 1);
    d->time_format = MCI_FORMAT_FRAMES;
    d->src[2] = (int16_t)d->info.w;
    d->src[3] = (int16_t)d->info.h;
    d->dst[2] = (int16_t)d->info.w;
    d->dst[3] = (int16_t)d->info.h;
    return 0;
}

static int is_avi(const char *type, const char *element) {
    if (type) return !strcasecmp(type, "avivideo");
    const char *dot = element ? strrchr(element, '.') : 0;
    return dot && !strcasecmp(dot, ".avi");
}

static unsigned seek_to(struct w16 *w, struct mcidev *d, uint32_t ms) {
    if (d->info.ms && ms > d->info.ms) return MCIERR_OUTOFRANGE;
    uvid_play_seek(d->play, ms);
    d->drawn = 0;
    await_frame(w, d);
    return 0;
}

static unsigned play(struct w16 *w, struct mcidev *d, uint32_t flags, uint32_t from, uint32_t to, uint16_t cb) {
    if ((flags & MCI_FROM) && to_ms(d, from) > d->info.ms) return MCIERR_OUTOFRANGE;
    if ((flags & MCI_TO) && to_ms(d, to) > d->info.ms) return MCIERR_OUTOFRANGE;
    settle(w, d, MCI_NOTIFY_SUPERSEDED);
    if (flags & MCI_FROM) seek_to(w, d, to_ms(d, from));
    else if (uvid_play_ended(d->play) || uvid_play_position(d->play) >= d->info.ms) seek_to(w, d, 0);
    d->to_ms = flags & MCI_TO ? to_ms(d, to) : 0;
    if (flags & MCI_NOTIFY) d->notify_hwnd = cb;
    show_own(w, d);
    set_playing(w, d, ST_PLAYING);
    if (!(flags & MCI_WAIT)) return 0;
    while (d->id && d->state == ST_PLAYING && !w->cpu.fault) {   // MCI_WAIT: the pictures go on meanwhile
        w16_mci_service(w);
        w16_gdi_flush(w);
        struct timespec ts = { 0, 10 * 1000000L };
        nanosleep(&ts, 0);
    }
    return 0;
}

static unsigned status(struct mcidev *d, uint32_t item, uint32_t *out) {
    switch (item) {
    case MCI_STATUS_LENGTH:          *out = length(d); return 0;
    case MCI_STATUS_POSITION:        *out = from_ms(d, uvid_play_position(d->play)); return 0;
    case MCI_STATUS_NUMBER_OF_TRACKS: case MCI_STATUS_CURRENT_TRACK: *out = 1; return 0;
    case MCI_STATUS_MODE:
        *out = d->state == ST_PLAYING ? MCI_MODE_PLAY : d->state == ST_PAUSED ? MCI_MODE_PAUSE : MCI_MODE_STOP;
        return 0;
    case MCI_STATUS_MEDIA_PRESENT: case MCI_STATUS_READY: case MCI_ANIM_STATUS_FORWARD: *out = 1; return 0;
    case MCI_STATUS_TIME_FORMAT:     *out = d->time_format; return 0;
    case MCI_ANIM_STATUS_SPEED:      *out = 1000; return 0;
    case MCI_ANIM_STATUS_HWND:       *out = d->hwnd; return 0;
    case MCI_ANIM_STATUS_HPAL: case MCI_ANIM_STATUS_STRETCH: *out = 0; return 0;
    default:                         return MCIERR_UNSUPPORTED_FUNCTION;
    }
}

static unsigned devcaps(uint32_t item, uint32_t *out) {
    switch (item) {
    case MCI_GETDEVCAPS_HAS_AUDIO: case MCI_GETDEVCAPS_HAS_VIDEO: case MCI_GETDEVCAPS_USES_FILES:
    case MCI_GETDEVCAPS_COMPOUND_DEVICE: case MCI_GETDEVCAPS_CAN_PLAY: case MCI_ANIM_GETDEVCAPS_CAN_STRETCH:
        *out = 1; return 0;
    case MCI_GETDEVCAPS_CAN_RECORD: case MCI_GETDEVCAPS_CAN_EJECT: case MCI_GETDEVCAPS_CAN_SAVE:
    case MCI_ANIM_GETDEVCAPS_CAN_REVERSE: case MCI_ANIM_GETDEVCAPS_PALETTES:
        *out = 0; return 0;
    case MCI_GETDEVCAPS_DEVICE_TYPE: *out = MCI_DEVTYPE_DIGITAL_VIDEO; return 0;
    case MCI_ANIM_GETDEVCAPS_FAST_RATE: case MCI_ANIM_GETDEVCAPS_SLOW_RATE: case MCI_ANIM_GETDEVCAPS_NORMAL_RATE:
        *out = 1000; return 0;
    case MCI_ANIM_GETDEVCAPS_MAX_WINDOWS: *out = 1; return 0;
    default: return MCIERR_UNSUPPORTED_FUNCTION;
    }
}

// MCI_WINDOW: the window it draws in (0 its own again), its own shown or hidden, its own's caption.
static unsigned set_window(struct w16 *w, struct mcidev *d, uint32_t flags, uint16_t hwnd, uint16_t show, const char *text) {
    if (flags & MCI_ANIM_WINDOW_HWND) {
        if (hwnd && !w16_wnd_get(w, hwnd)) return MCIERR_UNSUPPORTED_FUNCTION;   // MCIERR_NO_WINDOW in 3.1's
        d->hwnd = hwnd ? hwnd : d->own;
        d->drawn = 0;
    }
    if (!d->own && (flags & (MCI_ANIM_WINDOW_STATE | MCI_ANIM_WINDOW_TEXT)) && !d->hwnd) d->hwnd = d->own = make_own(w, d, 0, 0, 0);
    struct w16_wnd *x = d->own ? w16_wnd_get(w, d->own) : 0;
    if ((flags & MCI_ANIM_WINDOW_STATE) && x) w16_show_window(w, x, show);
    if ((flags & MCI_ANIM_WINDOW_TEXT) && x && text) {   // its caption, as WM_SETTEXT would set it
        char *t = strdup(text);
        if (t) {
            pthread_mutex_lock(&w->lock);
            free(x->text);
            x->text = t;
            pthread_mutex_unlock(&w->lock);
            if (x->kind != W16_K_CHILD) w16_task_post_arg(w, W16_POST_TITLE, x->hwnd);
        }
    }
    return 0;
}

// MCI_STOP and MCI_PAUSE both leave the picture where it is: MCIAVI paused for either.
static unsigned stop(struct w16 *w, struct mcidev *d, int paused) {
    settle(w, d, MCI_NOTIFY_ABORTED);
    set_playing(w, d, paused ? ST_PAUSED : ST_STOPPED);
    return 0;
}

// One command to one device (mciSendCommand's, and every mciSendString command after parsing).
// `p` the program's parameter block; `cb` MCI_NOTIFY's window.
static unsigned command(struct w16 *w, struct mcidev *d, uint16_t msg, uint32_t flags, uint8_t *p, uint32_t plen, uint16_t cb) {
    unsigned err = 0;
    uint32_t v = 0;
    switch (msg) {
    case MCI_CLOSE:
        close_dev(w, d);
        break;
    case MCI_PLAY:
        trace("play flags %08x from %u to %u (format %u)", flags, p && plen >= 8 ? rd32(p + 4) : 0, p && plen >= 12 ? rd32(p + 8) : 0,
              d->time_format);
        err = play(w, d, flags, p && plen >= 8 ? rd32(p + 4) : 0, p && plen >= 12 ? rd32(p + 8) : 0, cb);
        if (!err && (flags & MCI_WAIT) && (flags & MCI_NOTIFY)) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, d->id);
        return err;                              // the notification is the play's end, not now
    case MCI_SEEK:
        settle(w, d, MCI_NOTIFY_ABORTED);
        if (d->state == ST_PLAYING) set_playing(w, d, ST_STOPPED);
        if (flags & MCI_SEEK_TO_START) err = seek_to(w, d, 0);
        else if (flags & MCI_SEEK_TO_END) err = seek_to(w, d, d->info.ms);
        else if ((flags & MCI_TO) && p && plen >= 8) err = seek_to(w, d, to_ms(d, rd32(p + 4)));
        else err = MCIERR_MISSING_PARAMETER;
        break;
    case MCI_STOP:   err = stop(w, d, 0); break;
    case MCI_PAUSE:  err = stop(w, d, 1); break;
    case MCI_RESUME:
        if (d->state == ST_PAUSED) set_playing(w, d, ST_PLAYING);
        break;
    case MCI_STEP: {
        uint32_t n = (flags & MCI_ANIM_STEP_FRAMES) && p && plen >= 8 ? rd32(p + 4) : 1;
        uint32_t at = frame_of_ms(d, uvid_play_position(d->play));
        at = flags & MCI_ANIM_STEP_REVERSE ? (at > n ? at - n : 0) : at + n;
        if (at > d->frames) at = d->frames;
        if (d->state == ST_PLAYING) set_playing(w, d, ST_PAUSED);
        err = seek_to(w, d, ms_of_frame(d, at));
        break;
    }
    case MCI_STATUS:
        if (!(flags & MCI_STATUS_ITEM) || !p || plen < 12) return MCIERR_MISSING_PARAMETER;
        err = flags & MCI_STATUS_START ? (v = 0, 0) : status(d, rd32(p + 8), &v);
        if (!err) wr32(p + 4, v);
        trace("status %u: %u (%u ms)", rd32(p + 8), v, uvid_play_position(d->play));
        break;
    case MCI_GETDEVCAPS:
        if (!(flags & MCI_GETDEVCAPS_ITEM) || !p || plen < 12) return MCIERR_MISSING_PARAMETER;
        err = devcaps(rd32(p + 8), &v);
        if (!err) wr32(p + 4, v);
        break;
    case MCI_SET:
        if ((flags & MCI_SET_TIME_FORMAT) && p && plen >= 8) {
            uint32_t tf = rd32(p + 4);
            if (tf != MCI_FORMAT_MILLISECONDS && tf != MCI_FORMAT_FRAMES) return MCIERR_BAD_TIME_FORMAT;
            d->time_format = tf;
            trace("time format %u", tf);
        }
        if (flags & MCI_SET_VIDEO) d->video_off = (flags & MCI_SET_OFF) != 0;
        break;
    case MCI_WINDOW:
        if (!p || plen < 14) return MCIERR_NULL_PARAMETER_BLOCK;
        {
            char *text = flags & MCI_ANIM_WINDOW_TEXT ? far_str(w, rd32(p + 10)) : 0;
            err = set_window(w, d, flags, rd16(p + 4), rd16(p + 8), text);
            free(text);
        }
        break;
    case MCI_PUT:
        if (!p || plen < 12) return MCIERR_NULL_PARAMETER_BLOCK;
        if (flags & MCI_ANIM_RECT) {
            int16_t r[4] = { (int16_t)rd16(p + 4), (int16_t)rd16(p + 6), (int16_t)rd16(p + 8), (int16_t)rd16(p + 10) };
            if (flags & MCI_ANIM_PUT_SOURCE) memcpy(d->src, r, sizeof r);
            else memcpy(d->dst, r, sizeof r);
            d->drawn = 0;
        }
        break;
    case MCI_WHERE:
        if (!p || plen < 12) return MCIERR_NULL_PARAMETER_BLOCK;
        {
            const int16_t *r = flags & MCI_ANIM_PUT_SOURCE ? d->src : d->dst;
            for (int i = 0; i < 4; i++) wr16(p + 4 + 2 * i, (uint16_t)r[i]);
        }
        break;
    case MCI_UPDATE:
        d->drawn = 0;
        draw(w, d);
        break;
    case MCI_INFO:
        if (!p || plen < 10) return MCIERR_NULL_PARAMETER_BLOCK;
        {
            const char *s = flags & MCI_INFO_FILE ? (d->element ? d->element : "")
                          : flags & MCI_ANIM_INFO_TEXT ? (d->element ? d->element : "")
                          : "Video for Windows";
            err = put_str(w, rd32(p + 4), (unsigned)rd32(p + 8), s);
        }
        break;
    case MCI_SETVIDEO: case MCI_SETAUDIO:
        // A palette, a quality, a volume: nothing to change on a 24-bit picture and SaunaOS's own mixer.
        trace("set%s item %#x value %u", msg == MCI_SETVIDEO ? "video" : "audio", p && plen >= 12 ? rd32(p + 4) : 0,
              p && plen >= 12 ? rd32(p + 8) : 0);
        break;
    case MCI_CUE: case MCI_REALIZE: case MCI_BREAK:
        break;
    default:
        return MCIERR_UNRECOGNIZED_COMMAND;
    }
    if (!err && (flags & MCI_NOTIFY) && d->id) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, d->id);
    return err;
}

// MCI_OPEN: a new device for the type and element in the open block; its id written back.
static unsigned open_cmd(struct w16 *w, uint32_t flags, uint8_t *p, uint32_t plen, uint16_t *out_id) {
    if (!p || plen < 20) return MCIERR_NULL_PARAMETER_BLOCK;
    char *type = 0, *element = flags & MCI_OPEN_ELEMENT ? far_str(w, rd32(p + 12)) : 0;
    char *alias = flags & MCI_OPEN_ALIAS ? far_str(w, rd32(p + 16)) : 0;
    if ((flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID)) type = far_str(w, rd32(p + 8));
    if (type) {                                  // "avivideo!C:\FILE.AVI": a type and its element in one
        char *bang = strchr(type, '!');
        if (bang && !element) { element = strdup(bang + 1); *bang = 0; }
    }
    trace("MCI_OPEN flags %08x type %s element %s alias %s", flags, type ? type : "-", element ? element : "-",
          alias ? alias : "-");
    unsigned err = 0;
    struct mcidev *d = 0;
    if (!type && !element) err = MCIERR_DEVICE_TYPE_REQUIRED;
    else if (!is_avi(type, element)) err = MCIERR_DEVICE_NOT_INSTALLED;
    else if (alias && dev_named(alias)) err = MCIERR_DUPLICATE_ALIAS;
    else if (!(d = dev_new())) err = MCIERR_OUT_OF_MEMORY;
    else if ((err = open_avi(d, element)) == 0) {
        d->element = element;
        element = 0;
        d->alias = strdup(alias ? alias : d->element);
        if (flags & MCI_ANIM_OPEN_PARENT || flags & MCI_ANIM_OPEN_WS) {
            uint16_t parent = flags & MCI_ANIM_OPEN_PARENT && plen >= 26 ? rd16(p + 24) : 0;
            uint32_t style = flags & MCI_ANIM_OPEN_WS && plen >= 24 ? rd32(p + 20) : 0;
            d->hwnd = d->own = make_own(w, d, parent, style, (flags & MCI_ANIM_OPEN_WS) != 0);
        }
        wr16(p + 4, d->id);
        *out_id = d->id;
    }
    if (err && d) close_dev(w, d);
    free(type);
    free(element);
    free(alias);
    return err;
}

// The parameter block's size for each command: what may be read and written of it.
static uint32_t block_len(uint16_t msg) {
    switch (msg) {
    case MCI_OPEN:   return 26;                  // MCI_ANIM_OPEN_PARMS
    case MCI_SETVIDEO: case MCI_SETAUDIO: return 12;
    case MCI_PLAY:   return 12;
    case MCI_SEEK: case MCI_STEP: case MCI_SET: return msg == MCI_SET ? 12 : 8;
    case MCI_STATUS: return 16;
    case MCI_GETDEVCAPS: return 12;
    case MCI_INFO:   return 12;
    case MCI_WINDOW: return 14;
    case MCI_PUT: case MCI_WHERE: return 12;
    case MCI_UPDATE: return 14;
    default:         return 4;                   // MCI_GENERIC_PARMS: dwCallback
    }
}

static unsigned send_command(struct w16 *w, uint16_t id, uint16_t msg, uint32_t flags, uint32_t far) {
    uint32_t plen = block_len(msg);
    uint8_t *p = far ? far_ptr(w, far, plen, 1) : 0;
    if (far && !p) {                             // a shorter block at the end of a segment: its own fields only
        for (plen = 4; plen > 0 && !p; plen = plen > 4 ? plen - 2 : 0) p = far_ptr(w, far, plen, 1);
        if (!p) plen = 0;
    }
    uint16_t cb = p && plen >= 4 ? (uint16_t)rd32(p) : 0;
    trace("command %04x to %u flags %08x", msg, id, flags);
    if (msg == MCI_OPEN) {
        uint16_t got = 0;
        unsigned err = open_cmd(w, flags, p, plen, &got);
        if (!err && (flags & MCI_NOTIFY)) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, got);
        return err;
    }
    if (id == MCI_ALL_DEVICE_ID) {
        if (msg != MCI_CLOSE && msg != MCI_STOP && msg != MCI_PAUSE) return MCIERR_UNRECOGNIZED_COMMAND;
        for (int i = 0; i < g_ndev; i++)
            if (g_dev[i].id) command(w, &g_dev[i], msg, flags & ~MCI_NOTIFY, p, plen, 0);
        if (flags & MCI_NOTIFY) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, MCI_ALL_DEVICE_ID);
        return 0;
    }
    struct mcidev *d = dev_of(id);
    if (!d) return MCIERR_INVALID_DEVICE_ID;
    return command(w, d, msg, flags, p, plen, cb);
}

// --- the task's side: pictures and endings ----------------------------------------------

int w16_mci_due(void) {
    return g_due;
}

// Main thread, every tick: wake the task while anything plays.
void w16_mci_tick(struct w16 *w) {
    pthread_mutex_lock(&w->lock);
    if (g_playing || g_awaiting) {
        g_due = 1;
        pthread_cond_broadcast(&w->cond);
    }
    pthread_mutex_unlock(&w->lock);
}

// Task thread: each playing device's frame due now drawn; a play that reached its end or its MCI_TO
// stopped there and its notification sent.
// A NEW PICTURE IS SHOWN AT ONCE (w16_gdi_flush), not when the program next waits for a message: a
// program polling MCI_STATUS in a loop of its own (DK's encyclopaedias) would never see one.
void w16_mci_service(struct w16 *w) {
    pthread_mutex_lock(&w->lock);
    g_due = 0;
    pthread_mutex_unlock(&w->lock);
    int drew = 0;
    for (int i = 0; i < g_ndev; i++) {
        struct mcidev *d = &g_dev[i];
        if (!d->id || !d->play) continue;
        uvid_play_tick(d->play);
        if (uvid_play_serial(d->play) != d->drawn) { draw(w, d); drew = 1; }
        if (d->awaiting && (uvid_play_serial(d->play) != d->await_serial || w16_now_ms() - d->await_ms > 2000))
            awaited(w, d);
        if (d->state != ST_PLAYING) continue;
        uint32_t at = uvid_play_position(d->play);
        if (uvid_play_ended(d->play) || (d->to_ms && at >= d->to_ms) || (d->info.ms && at >= d->info.ms)) {
            trace("device %u: ended at %u ms (to %u)", d->id, at, d->to_ms);
            set_playing(w, d, ST_STOPPED);
            settle(w, d, MCI_NOTIFY_SUCCESSFUL);
        }
    }
    if (drew && !w->painting) w16_gdi_flush(w);   // never in the middle of the program's own paint
}

// --- mciSendString's commands -----------------------------------------------------------

// The words of a command line; a "quoted" word kept whole.
struct words { char *buf; char *v[48]; int n; };

static int split(struct words *ws, const char *line) {
    ws->n = 0;
    if (!(ws->buf = strdup(line))) return -1;
    for (char *p = ws->buf; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (ws->n == (int)(sizeof ws->v / sizeof ws->v[0])) return MCIERR_EXTRA_CHARACTERS;
        if (*p == '"') {
            ws->v[ws->n++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            ws->v[ws->n++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
        if (*p) *p++ = 0;
    }
    return 0;
}

static int is(const char *a, const char *b) { return a && !strcasecmp(a, b); }

static int number(const char *s, uint32_t *out) {
    char *end;
    if (!s || !*s) return -1;
    unsigned long v = strtoul(s, &end, 10);
    if (*end) return -1;
    *out = (uint32_t)v;
    return 0;
}

static const char *mode_name(const struct mcidev *d) {
    return d->state == ST_PLAYING ? "playing" : d->state == ST_PAUSED ? "paused" : "stopped";
}

// A command line, run: 0 or an MCIERR_, its answer (if any) into `ret`.
static unsigned send_string(struct w16 *w, const char *line, char *ret, size_t cap, uint16_t cb) {
    struct words ws;
    int r = split(&ws, line);
    unsigned err = r < 0 ? MCIERR_OUT_OF_MEMORY : (unsigned)r;
    ret[0] = 0;
    if (!err && ws.n < 1) err = MCIERR_UNRECOGNIZED_COMMAND;
    if (!err && ws.n < 2) err = MCIERR_MISSING_DEVICE_NAME;
    if (err) { free(ws.buf); return err; }
    const char *cmd = ws.v[0], *name = ws.v[1];
    uint32_t flags = 0;
    int i;
    for (i = 2; i < ws.n; i++) {                 // the common flags, wherever they stand
        if (is(ws.v[i], "notify")) flags |= MCI_NOTIFY;
        else if (is(ws.v[i], "wait")) flags |= MCI_WAIT;
        else continue;
        memmove(&ws.v[i], &ws.v[i + 1], sizeof ws.v[0] * (size_t)(ws.n - i - 1));
        ws.n--;
        i--;
    }
    if (is(cmd, "open")) {
        uint8_t blk[26] = { 0 };
        char *type = 0, *element = 0, *alias = 0;
        uint32_t style = 0;
        uint16_t parent = 0;
        char *bang = strchr(name, '!');
        if (bang) { *bang = 0; type = (char *)name; element = bang + 1; }
        else if (is(name, "avivideo")) type = (char *)name;
        else element = (char *)name;
        for (i = 2; i < ws.n && !err; i++) {
            if (is(ws.v[i], "type") && i + 1 < ws.n) type = ws.v[++i];
            else if (is(ws.v[i], "alias") && i + 1 < ws.n) alias = ws.v[++i];
            else if (is(ws.v[i], "shareable")) flags |= MCI_OPEN_SHAREABLE;
            else if (is(ws.v[i], "parent") && i + 1 < ws.n) {
                uint32_t v = 0;
                if (number(ws.v[++i], &v) < 0) err = MCIERR_BAD_INTEGER;
                parent = (uint16_t)v;
                flags |= MCI_ANIM_OPEN_PARENT;
            } else if (is(ws.v[i], "style") && i + 1 < ws.n) {
                const char *s = ws.v[++i];
                flags |= MCI_ANIM_OPEN_WS;
                if (is(s, "child")) style = WS_CHILD;
                else if (is(s, "popup")) style = 0x80000000u;
                else if (is(s, "overlapped")) style = 0x00CF0000u;
                else if (number(s, &style) < 0) err = MCIERR_BAD_INTEGER;
            } else err = MCIERR_UNRECOGNIZED_KEYWORD;
        }
        // Through MCI_OPEN's own path, with the strings in w16's scratch memory.
        uint16_t got = 0;
        if (!err) {
            uint16_t at = w16_sys_push(w, 600);
            if (w->cpu.fault) { free(ws.buf); return MCIERR_OUT_OF_MEMORY; }
            uint8_t *m = w->sysmem + at;
            uint32_t base = (uint32_t)w->sysdata << 16;
            uint16_t o = 32;
            if (type) { snprintf((char *)m + o, 160, "%s", type); wr32(blk + 8, base | (uint16_t)(at + o)); flags |= MCI_OPEN_TYPE; o += 160; }
            if (element) { snprintf((char *)m + o, 260, "%s", element); wr32(blk + 12, base | (uint16_t)(at + o)); flags |= MCI_OPEN_ELEMENT; o += 260; }
            if (alias) { snprintf((char *)m + o, 128, "%s", alias); wr32(blk + 16, base | (uint16_t)(at + o)); flags |= MCI_OPEN_ALIAS; }
            wr32(blk + 20, style);
            wr16(blk + 24, parent);
            memcpy(m, blk, sizeof blk);
            err = open_cmd(w, flags, m, sizeof blk, &got);
            w16_sys_pop(w, at);
        }
        if (!err) snprintf(ret, cap, "%u", got);
        if (!err && (flags & MCI_NOTIFY)) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, got);
        free(ws.buf);
        return err;
    }
    struct mcidev *d = is(name, "all") ? 0 : dev_named(name);
    if (!d && !is(name, "all")) { free(ws.buf); return MCIERR_INVALID_DEVICE_NAME; }
    if (!d) {                                    // "close all", "stop all"
        uint16_t msg = is(cmd, "close") ? MCI_CLOSE : is(cmd, "stop") ? MCI_STOP : is(cmd, "pause") ? MCI_PAUSE : 0;
        free(ws.buf);
        return msg ? send_command(w, MCI_ALL_DEVICE_ID, msg, flags & ~MCI_NOTIFY, 0) : MCIERR_UNRECOGNIZED_COMMAND;
    }
    uint8_t blk[16] = { 0 };
    wr32(blk, cb);
    uint16_t msg = 0;
    if (is(cmd, "play")) {
        msg = MCI_PLAY;
        for (i = 2; i < ws.n && !err; i++) {
            uint32_t v = 0;
            if ((is(ws.v[i], "from") || is(ws.v[i], "to")) && i + 1 < ws.n) {
                int from = is(ws.v[i], "from");
                if (number(ws.v[++i], &v) < 0) err = MCIERR_BAD_INTEGER;
                wr32(blk + (from ? 4 : 8), v);
                flags |= from ? MCI_FROM : MCI_TO;
            } else if (!is(ws.v[i], "fullscreen") && !is(ws.v[i], "window")) err = MCIERR_UNRECOGNIZED_KEYWORD;
        }
    } else if (is(cmd, "seek")) {
        msg = MCI_SEEK;
        if (ws.n >= 4 && is(ws.v[2], "to")) {
            uint32_t v = 0;
            if (is(ws.v[3], "start")) flags |= MCI_SEEK_TO_START;
            else if (is(ws.v[3], "end")) flags |= MCI_SEEK_TO_END;
            else if (number(ws.v[3], &v) == 0) { flags |= MCI_TO; wr32(blk + 4, v); }
            else err = MCIERR_BAD_INTEGER;
        } else err = MCIERR_MISSING_PARAMETER;
    } else if (is(cmd, "stop") || is(cmd, "pause") || is(cmd, "resume") || is(cmd, "close") || is(cmd, "cue")
               || is(cmd, "realize")) {
        msg = is(cmd, "stop") ? MCI_STOP : is(cmd, "pause") ? MCI_PAUSE : is(cmd, "resume") ? MCI_RESUME
            : is(cmd, "close") ? MCI_CLOSE : is(cmd, "cue") ? MCI_CUE : MCI_REALIZE;
    } else if (is(cmd, "step")) {
        msg = MCI_STEP;
        for (i = 2; i < ws.n && !err; i++) {
            uint32_t v = 0;
            if (is(ws.v[i], "reverse")) flags |= MCI_ANIM_STEP_REVERSE;
            else if (is(ws.v[i], "by") && i + 1 < ws.n && number(ws.v[++i], &v) == 0) { flags |= MCI_ANIM_STEP_FRAMES; wr32(blk + 4, v); }
            else err = MCIERR_UNRECOGNIZED_KEYWORD;
        }
    } else if (is(cmd, "status")) {
        uint32_t v = 0;
        const char *a = ws.n > 2 ? ws.v[2] : 0, *b = ws.n > 3 ? ws.v[3] : 0;
        if (is(a, "mode")) snprintf(ret, cap, "%s", mode_name(d));
        else if (is(a, "length")) snprintf(ret, cap, "%u", length(d));
        else if (is(a, "position")) snprintf(ret, cap, "%u", from_ms(d, uvid_play_position(d->play)));
        else if (is(a, "start")) snprintf(ret, cap, "0");
        else if (is(a, "ready") || (is(a, "media") && is(b, "present")) || is(a, "forward")) snprintf(ret, cap, "true");
        else if (is(a, "time") && is(b, "format")) snprintf(ret, cap, "%s", d->time_format == MCI_FORMAT_FRAMES ? "frames" : "milliseconds");
        else if (is(a, "window") && is(b, "handle")) snprintf(ret, cap, "%u", d->hwnd);
        else if ((is(a, "number") && is(b, "of")) || (is(a, "current") && is(b, "track"))) snprintf(ret, cap, "1");
        else if (is(a, "speed")) snprintf(ret, cap, "1000");
        else if (is(a, "stretch")) snprintf(ret, cap, "false");
        else err = a ? MCIERR_UNRECOGNIZED_KEYWORD : MCIERR_MISSING_PARAMETER;
        (void)v;
        free(ws.buf);
        if (!err && (flags & MCI_NOTIFY)) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, d->id);
        return err;
    } else if (is(cmd, "set")) {
        msg = MCI_SET;
        for (i = 2; i < ws.n && !err; i++) {
            if (is(ws.v[i], "time") && i + 2 < ws.n && is(ws.v[i + 1], "format")) {
                const char *f = ws.v[i + 2];
                i += 2;
                flags |= MCI_SET_TIME_FORMAT;
                if (is(f, "frames")) wr32(blk + 4, MCI_FORMAT_FRAMES);
                else if (is(f, "ms") || is(f, "milliseconds")) wr32(blk + 4, MCI_FORMAT_MILLISECONDS);
                else err = MCIERR_BAD_TIME_FORMAT;
            } else if ((is(ws.v[i], "video") || is(ws.v[i], "audio")) && i + 1 < ws.n) {
                int video = is(ws.v[i], "video");
                if (!video && i + 2 < ws.n && (is(ws.v[i + 1], "all") || is(ws.v[i + 1], "left") || is(ws.v[i + 1], "right"))) i++;
                const char *o = ws.v[++i];
                if (!is(o, "on") && !is(o, "off")) { err = MCIERR_BAD_CONSTANT; break; }
                if (video) flags |= MCI_SET_VIDEO | (is(o, "on") ? MCI_SET_ON : MCI_SET_OFF);
            } else err = MCIERR_UNRECOGNIZED_KEYWORD;
        }
    } else if (is(cmd, "window")) {
        uint16_t hwnd = 0, show = 0;
        const char *text = 0;
        for (i = 2; i < ws.n && !err; i++) {
            uint32_t v = 0;
            if (is(ws.v[i], "handle") && i + 1 < ws.n) {
                flags |= MCI_ANIM_WINDOW_HWND;
                if (is(ws.v[++i], "default")) hwnd = 0;
                else if (number(ws.v[i], &v) == 0) hwnd = (uint16_t)v;
                else err = MCIERR_BAD_INTEGER;
            } else if (is(ws.v[i], "state") && i + 1 < ws.n) {
                static const struct { const char *w; uint16_t sw; } st[] = {
                    { "hide", 0 }, { "show", 5 }, { "minimize", 6 }, { "maximize", 3 }, { "restore", 9 },
                    { "minimized", 2 }, { "maximized", 3 }, { "iconic", 6 }, { "no", 8 }, { "normal", 1 } };
                const char *s = ws.v[++i];
                unsigned k;
                for (k = 0; k < sizeof st / sizeof st[0] && !is(s, st[k].w); k++) {}
                if (k == sizeof st / sizeof st[0]) { err = MCIERR_BAD_CONSTANT; break; }
                if (is(s, "no") && i + 1 < ws.n) i++;   // "no action"
                flags |= MCI_ANIM_WINDOW_STATE;
                show = st[k].sw;
            } else if (is(ws.v[i], "text") && i + 1 < ws.n) {
                flags |= MCI_ANIM_WINDOW_TEXT;
                text = ws.v[++i];
            } else err = MCIERR_UNRECOGNIZED_KEYWORD;
        }
        if (!err) err = set_window(w, d, flags, hwnd, show, text);
        free(ws.buf);
        if (!err && (flags & MCI_NOTIFY)) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, d->id);
        return err;
    } else if (is(cmd, "put") || is(cmd, "where")) {
        int src = ws.n > 2 && is(ws.v[2], "source");
        if (ws.n < 3 || !(src || is(ws.v[2], "destination") || is(ws.v[2], "window"))) err = MCIERR_MISSING_PARAMETER;
        int16_t *r = src ? d->src : d->dst;
        if (!err && is(cmd, "where")) {
            snprintf(ret, cap, "%d %d %d %d", r[0], r[1], r[2], r[3]);
        } else if (!err && ws.n >= 8 && is(ws.v[3], "at")) {
            uint32_t v[4] = { 0 };
            for (int k = 0; k < 4 && !err; k++) if (number(ws.v[4 + k], &v[k]) < 0) err = MCIERR_BAD_INTEGER;
            if (!err) for (int k = 0; k < 4; k++) r[k] = (int16_t)v[k];
            d->drawn = 0;
        } else if (!err && ws.n > 3) err = MCIERR_UNRECOGNIZED_KEYWORD;
        free(ws.buf);
        if (!err && (flags & MCI_NOTIFY)) notify(w, cb, MCI_NOTIFY_SUCCESSFUL, d->id);
        return err;
    } else if (is(cmd, "update")) {
        msg = MCI_UPDATE;
    } else if (is(cmd, "info")) {
        const char *a = ws.n > 2 ? ws.v[2] : 0;
        if (is(a, "product")) snprintf(ret, cap, "Video for Windows");
        else if (is(a, "file") || is(a, "window")) snprintf(ret, cap, "%s", d->element ? d->element : "");
        else err = a ? MCIERR_UNRECOGNIZED_KEYWORD : MCIERR_MISSING_PARAMETER;
        free(ws.buf);
        return err;
    } else if (is(cmd, "capability")) {
        const char *a = ws.n > 2 ? ws.v[2] : 0, *b = ws.n > 3 ? ws.v[3] : 0;
        if (is(a, "device") && is(b, "type")) snprintf(ret, cap, "digitalvideo");
        else if ((is(a, "can") && (is(b, "play") || is(b, "stretch"))) || (is(a, "has") && (is(b, "audio") || is(b, "video")))
                 || (is(a, "uses") && is(b, "files")) || (is(a, "compound") && is(b, "device"))) snprintf(ret, cap, "true");
        else if (is(a, "can") || is(a, "uses")) snprintf(ret, cap, "false");
        else if (is(a, "windows")) snprintf(ret, cap, "1");
        else if (is(a, "fast") || is(a, "slow") || is(a, "normal")) snprintf(ret, cap, "1000");
        else err = a ? MCIERR_UNRECOGNIZED_KEYWORD : MCIERR_MISSING_PARAMETER;
        free(ws.buf);
        return err;
    } else {
        err = MCIERR_UNRECOGNIZED_COMMAND;
    }
    free(ws.buf);
    if (err) return err;
    return command(w, d, msg, flags, blk, sizeof blk, cb);
}

// --- the calls --------------------------------------------------------------------------

// mciSendCommand(wDeviceID, wMessage, dwParam1, dwParam2) -> DWORD
// Windows: Sends a Media Control Interface command (MCI_OPEN, MCI_PLAY...) to a device: a CD, a
// sequencer, a wave file, a video.
// w16: The same for AVI video (the AVIVideo device, MCIAVI's commands: open, close, play, seek, stop,
// pause, resume, step, status, set, getdevcaps, info, window, put, where, update, cue, realize), its
// pictures from media/uvid and its sound through SaunaOS's; any other device is not installed.
// Why: Answered by w16, as Wine answers MCI with its own drivers: a program's own MCIAVI.DRV would need
// Video for Windows' DLLs and a driver task beside the program.
// Notice: A program that plays a .AVI plays it; one that asks for a CD, MIDI or a wave file through MCI
// hears MCIERR_DEVICE_NOT_INSTALLED, as on a 3.1 without that driver.
// Whole: The other devices -- waveaudio, sequencer, cdaudio -- not written yet.
static int api_mciSendCommand(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t id = w16_arg16(a), msg = w16_arg16(a);
    uint32_t flags = w16_arg32(a), far = w16_arg32(a);
    unsigned err = send_command(w, id, msg, flags, far);
    if (err) trace("command %04x: error %u", msg, err);
    w->cpu.r[R_AX] = (uint16_t)err;
    w->cpu.r[R_DX] = 0;
    return w->cpu.fault ? -1 : 12;
}

// mciSendString(lpstrCommand, lpstrReturnString, uReturnLength, hwndCallback) -> DWORD
// Windows: An MCI command as text ("play movie from 0 notify"), its answer as text.
// w16: The same for what mciSendCommand answers, the device named by its alias or its element, a
// new one opened with "open"; the answer "stopped", "playing", a number, "true".
// Why: As mciSendCommand.
// Notice: As mciSendCommand.
// Whole: As mciSendCommand; a command naming a device not open does not open it (3.1's auto-open).
static int api_mciSendString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t cmd = w16_arg32(a), out = w16_arg32(a);
    uint16_t cap = w16_arg16(a), cb = w16_arg16(a);
    char *line = far_str(w, cmd);
    char ret[256];
    unsigned err = line ? send_string(w, line, ret, sizeof ret, cb) : MCIERR_NULL_PARAMETER_BLOCK;
    trace("string \"%s\": %u \"%s\"", line ? line : "", err, err ? "" : ret);
    if (!err) err = put_str(w, out, cap, ret);
    free(line);
    w->cpu.r[R_AX] = (uint16_t)err;
    w->cpu.r[R_DX] = 0;
    return w->cpu.fault ? -1 : 12;
}

static const struct { unsigned err; const char *text; } ERRORS[] = {
    { MCIERR_INVALID_DEVICE_ID, "Invalid MCI device ID. Use the ID returned when opening the MCI device." },
    { MCIERR_UNRECOGNIZED_KEYWORD, "The driver cannot recognize the specified command parameter." },
    { MCIERR_UNRECOGNIZED_COMMAND, "The driver cannot recognize the specified command." },
    { MCIERR_INVALID_DEVICE_NAME, "The specified device is not open or is not recognized by MCI." },
    { MCIERR_OUT_OF_MEMORY, "There is not enough memory available for this task." },
    { MCIERR_PARAM_OVERFLOW, "The string returned by MCI is too long for its buffer." },
    { MCIERR_BAD_INTEGER, "The specified integer is invalid for this command." },
    { MCIERR_MISSING_PARAMETER, "The specified command requires a parameter." },
    { MCIERR_UNSUPPORTED_FUNCTION, "The MCI device you are using does not support the specified command." },
    { MCIERR_FILE_NOT_FOUND, "Cannot find the specified file. Make sure the path and filename are correct." },
    { MCIERR_OUTOFRANGE, "The specified parameter is out of range for the specified command." },
    { MCIERR_DEVICE_TYPE_REQUIRED, "The specified device cannot be found. Check the device name." },
    { MCIERR_DUPLICATE_ALIAS, "The specified alias is already in use in this application." },
    { MCIERR_BAD_CONSTANT, "The specified parameter value is unknown." },
    { MCIERR_MISSING_DEVICE_NAME, "No device name was specified." },
    { MCIERR_BAD_TIME_FORMAT, "The specified time format is invalid." },
    { MCIERR_INVALID_FILE, "The specified file cannot be played on the specified MCI device." },
    { MCIERR_NULL_PARAMETER_BLOCK, "A null parameter block was passed to MCI." },
    { MCIERR_FILENAME_REQUIRED, "A valid filename is required." },
    { MCIERR_EXTRA_CHARACTERS, "The command line has too many words." },
    { MCIERR_DEVICE_NOT_INSTALLED, "The specified device is not installed on the system." },
};

// mciGetErrorString(dwError, lpstrBuffer, uLength) -> BOOL
// Windows: The text of an MCI error, for the program to show.
// w16: The same, in English, for every error w16's MCI answers; FALSE for a code it does not know.
static int api_mciGetErrorString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t err = w16_arg32(a), out = w16_arg32(a);
    uint16_t cap = w16_arg16(a);
    const char *text = 0;
    for (unsigned i = 0; i < sizeof ERRORS / sizeof ERRORS[0]; i++)
        if (ERRORS[i].err == err) text = ERRORS[i].text;
    put_str(w, out, cap, text ? text : "");
    w->cpu.r[R_AX] = text != 0;
    return w->cpu.fault ? -1 : 10;
}

const struct w16_api g_mci_api[] = {
    { 701, api_mciSendCommand },
    { 702, api_mciSendString },
    { 706, api_mciGetErrorString },
};
const unsigned g_mci_api_n = sizeof g_mci_api / sizeof g_mci_api[0];
