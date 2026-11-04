// wave16.c -- MMSYSTEM's waveOut: a program's own buffers, played in turn
// through SaunaOS's sound daemon (media/usnd.h's push stream).
//
// THE BYTES ARE COPIED AT waveOutWrite, decoded to the daemon's format
// (stereo 16-bit at USND_RATE, PCM resampled, MS and IMA ADPCM through
// media/uadpcm.h as Windows' ACM mapper took them): the main thread feeds
// the daemon from w16's memory, never the program's, so a block freed
// while it plays is the program's mistake, not a read after free.
//
// A BUFFER IS DONE WHEN THE PLAYBACK HAS PASSED ITS END, and what the
// program sees of that -- WHDR_DONE in its header, MM_WOM_DONE to its
// window, its callback function -- happens on the TASK thread
// (w16_wave_service): after every Windows call, and in the message wait,
// which the main thread's tick wakes. With no sound daemon the device is
// SILENT: a clock plays the buffers, so a program that will not run
// without a wave device (DK's encyclopaedias) runs anyway.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "media/uadpcm.h"
#include "media/usnd.h"
#include "common/w16prefs.h"

#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_ALLOCATED   4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER    6
#define MMSYSERR_NOMEM       7
#define WAVERR_BADFORMAT     32
#define WAVERR_STILLPLAYING  33
#define WAVERR_UNPREPARED    34

#define WAVE_FORMAT_QUERY 0x0001
#define CALLBACK_TYPEMASK 0x00070000u
#define CALLBACK_WINDOW   0x00010000u
#define CALLBACK_TASK     0x00020000u
#define CALLBACK_FUNCTION 0x00030000u

#define WHDR_DONE     0x01
#define WHDR_PREPARED 0x02
#define WHDR_INQUEUE  0x10

#define MM_WOM_OPEN  0x3BB
#define MM_WOM_CLOSE 0x3BC
#define MM_WOM_DONE  0x3BD

#define HWAVEOUT 1                               // the one device

struct wbuf {
    uint32_t hdr;                                // the program's WAVEHDR, a far pointer
    int16_t *pcm;                                // its sound, stereo at USND_RATE: ours
    long frames;
    uint64_t end;                                // where it ends in the device's frames
    int done;                                    // played: owed to the program
};

static struct {
    pthread_mutex_t lock;
    int open, paused, silent;
    int tag, channels, bits;                     // the format: 1 PCM, or ADPCM's
    uint32_t rate;
    struct uadpcm adpcm;
    uint32_t flags, callback, instance;          // waveOutOpen's
    int32_t last[2];                             // the resampler: the source frame before...
    uint32_t phase, step;                        // ...and how far past it, in 1/65536ths
    struct wbuf *q;                              // queued, oldest first
    int nq, qcap;
    int fed, fed_at;                             // the daemon has buffers [0, fed) and fed_at frames of q[fed]
    uint64_t queued, pushed;                     // frames written, frames handed to the daemon
    uint64_t clock_base, clock_ms;               // the silent device: frames played as of clock_ms (0: stopped)
    int due;                                     // buffers done and not yet told
} g_wo = { .lock = PTHREAD_MUTEX_INITIALIZER };

// --- converting the program's bytes ----------------------------------------------------

static int grow(int16_t **pcm, long *cap, long need) {
    if (need <= *cap) return 1;
    long c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    int16_t *p = realloc(*pcm, (size_t)c * 4);
    if (!p) return 0;
    *pcm = p;
    *cap = c;
    return 1;
}

// One source frame through the resampler (linear, its phase carried from buffer to buffer), as
// many output frames as fall before it.
static int feed(int16_t l, int16_t r, int16_t **pcm, long *n, long *cap) {
    while (g_wo.phase < 0x10000) {
        if (!grow(pcm, cap, *n + 1)) return 0;
        int32_t f = (int32_t)g_wo.phase;
        (*pcm)[2 * *n] = (int16_t)(g_wo.last[0] + (((int32_t)l - g_wo.last[0]) * f >> 16));
        (*pcm)[2 * *n + 1] = (int16_t)(g_wo.last[1] + (((int32_t)r - g_wo.last[1]) * f >> 16));
        (*n)++;
        g_wo.phase += g_wo.step;
    }
    g_wo.phase -= 0x10000;
    g_wo.last[0] = l;
    g_wo.last[1] = r;
    return 1;
}

static int feed_frames(const int16_t *s, size_t frames, int ch, int16_t **pcm, long *n, long *cap) {
    for (size_t i = 0; i < frames; i++)
        if (!feed(s[i * ch], s[i * ch + ch - 1], pcm, n, cap)) return 0;
    return 1;
}

// `len` bytes of the open format as the device's frames, allocated; 0 out of memory.
static int16_t *convert(const uint8_t *b, uint32_t len, long *frames) {
    int16_t *pcm = 0, s[2];
    long n = 0, cap = 0;
    int ch = g_wo.channels;
    if (g_wo.tag == 1) {
        unsigned bytes = (unsigned)(g_wo.bits / 8 * ch);
        for (uint32_t at = 0; at + bytes <= len; at += bytes) {
            for (int c = 0; c < ch; c++)
                s[c] = g_wo.bits == 8 ? (int16_t)((b[at + c] - 128) << 8)
                                      : (int16_t)(b[at + 2 * c] | b[at + 2 * c + 1] << 8);
            if (!feed_frames(s, 1, ch, &pcm, &n, &cap)) { free(pcm); return 0; }
        }
    } else {
        int16_t *blk = malloc((size_t)g_wo.adpcm.frames_per_block * (size_t)ch * 2);
        if (!blk) return 0;
        for (uint32_t at = 0; at < len; at += g_wo.adpcm.block_align) {
            size_t got = uadpcm_block(&g_wo.adpcm, b + at, len - at, blk);
            if (!feed_frames(blk, got, ch, &pcm, &n, &cap)) { free(pcm); free(blk); return 0; }
        }
        free(blk);
    }
    *frames = n;
    return pcm ? pcm : calloc(1, 4);             // an empty buffer is still a buffer to return
}

// --- the device's clock, on the main thread -------------------------------------------

static uint64_t played(void) {
    if (g_wo.silent) {
        uint64_t p = g_wo.clock_base;
        if (g_wo.clock_ms) p += (w16_now_ms() - g_wo.clock_ms) * USND_RATE / 1000;
        return p < g_wo.queued ? p : g_wo.queued;
    }
    long in_daemon = USND_PUSH_FRAMES - usnd_push_space();
    return g_wo.pushed > (uint64_t)in_daemon ? g_wo.pushed - (uint64_t)in_daemon : 0;
}

void w16_wave_tick(struct w16 *w) {
    pthread_mutex_lock(&g_wo.lock);
    if (!g_wo.open || g_wo.paused) { pthread_mutex_unlock(&g_wo.lock); return; }
    if (!g_wo.silent)
        while (g_wo.fed < g_wo.nq) {
            struct wbuf *b = &g_wo.q[g_wo.fed];
            long take = b->frames - g_wo.fed_at, room = usnd_push_space();
            if (take > room) take = room;
            long got = take > 0 ? usnd_push(b->pcm + 2 * g_wo.fed_at, take) : 0;
            g_wo.fed_at += (int)got;
            g_wo.pushed += (uint64_t)got;
            if (g_wo.fed_at < b->frames) break;
            g_wo.fed++;
            g_wo.fed_at = 0;
        }
    uint64_t now = played();
    if (g_wo.silent && now >= g_wo.queued) {     // idle: the clock waits for the next buffer
        g_wo.clock_base = g_wo.queued;
        g_wo.clock_ms = w16_now_ms();
    }
    int told = 0;
    for (int i = 0; i < g_wo.nq; i++)
        if (!g_wo.q[i].done && g_wo.q[i].end <= now) { g_wo.q[i].done = 1; g_wo.due++; told = 1; }
    pthread_mutex_unlock(&g_wo.lock);
    if (told) {                                  // a program waiting for a message wakes to hear it
        pthread_mutex_lock(&w->lock);
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->lock);
    }
}

// --- telling the program, on the task thread ----------------------------------------------

int w16_wave_due(void) { return __atomic_load_n(&g_wo.due, __ATOMIC_RELAXED) != 0; }

static void tell(struct w16 *w, uint16_t msg, uint32_t p1) {
    uint32_t type = g_wo.flags & CALLBACK_TYPEMASK;
    if (type == CALLBACK_WINDOW || type == CALLBACK_TASK)
        w16_post_msg(w, type == CALLBACK_WINDOW ? (uint16_t)g_wo.callback : 0, msg, HWAVEOUT, p1);
    else if (type == CALLBACK_FUNCTION && g_wo.callback) {
        const uint16_t words[] = { HWAVEOUT, msg, (uint16_t)(g_wo.instance >> 16), (uint16_t)g_wo.instance,
                                   (uint16_t)(p1 >> 16), (uint16_t)p1, 0, 0 };
        w16_call_far(w, (uint16_t)(g_wo.callback >> 16), (uint16_t)g_wo.callback, words, 8);
    }
}

static void set_flags(struct w16 *w, uint32_t hdr, uint32_t set, uint32_t clear) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(hdr >> 16), (uint16_t)(hdr + 16), 4, 1);
    if (!p) { w->cpu.fault = 0; return; }        // a header gone: nothing to tell it
    uint32_t f = ((uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 | set) & ~clear;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(f >> 8 * i);
}

void w16_wave_service(struct w16 *w) {
    static int busy;                             // a callback that writes the next buffer comes back here
    if (busy || !w16_wave_due()) return;
    busy = 1;
    for (;;) {
        pthread_mutex_lock(&g_wo.lock);
        if (!g_wo.nq || !g_wo.q[0].done) { g_wo.due = 0; pthread_mutex_unlock(&g_wo.lock); break; }
        struct wbuf b = g_wo.q[0];               // in order, as Windows returned them
        memmove(g_wo.q, g_wo.q + 1, (size_t)--g_wo.nq * sizeof *g_wo.q);
        if (g_wo.fed > 0) g_wo.fed--;
        else g_wo.fed_at = 0;
        g_wo.due--;
        pthread_mutex_unlock(&g_wo.lock);
        free(b.pcm);
        set_flags(w, b.hdr, WHDR_DONE, WHDR_INQUEUE);
        tell(w, MM_WOM_DONE, b.hdr);
    }
    busy = 0;
}

// --- the calls ------------------------------------------------------------------------

// The format at `far` taken as the device's (PCM, MS or IMA ADPCM); 0, or WAVERR_BADFORMAT.
static int take_format(struct w16 *w, uint32_t far) {
    const uint8_t *f = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 16, 0);
    if (!f) return -1;
    int tag = f[0] | f[1] << 8, ch = f[2] | f[3] << 8, bits = f[14] | f[15] << 8;
    uint32_t rate = (uint32_t)(f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24);
    if (tag == 1) {
        if ((ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 1000 || rate > 192000) return WAVERR_BADFORMAT;
    } else if (tag == UADPCM_MS || tag == UADPCM_IMA) {
        const uint8_t *x = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 18, 0);
        unsigned extra = x ? (unsigned)(x[16] | x[17] << 8) : 0;
        x = x ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 18 + extra, 0) : 0;
        if (!x) return -1;
        if (uadpcm_init(&g_wo.adpcm, x, 18 + extra) < 0) return WAVERR_BADFORMAT;
    } else {
        return WAVERR_BADFORMAT;
    }
    g_wo.tag = tag;
    g_wo.channels = ch;
    g_wo.bits = bits;
    g_wo.rate = rate;
    return 0;
}

// waveOutOpen(lphWaveOut, uDeviceID, lpFormat, dwCallback, dwInstance, dwFlags) -> UINT
// Windows: Opens a wave output device -- or, with WAVE_MAPPER, the one that plays the format, through
// a converter when the card cannot -- for PCM or a compressed format; the program's callback (a window,
// its task or a function) hears MM_WOM_OPEN, MM_WOM_DONE for each buffer played, and MM_WOM_CLOSE.
// WAVE_FORMAT_QUERY only asks whether the format would open.
// w16: The same, one device: PCM of 8 or 16 bits, mono or stereo, any rate, and Microsoft's and IMA's
// ADPCM, as the mapper played them, through SaunaOS's sound daemon -- or, with none running, a silent
// device whose buffers end when they would have. MMSYSERR_NODRIVER when the `sounds` setting is Off.
static int api_waveOutOpen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct cpu16 *c = &w->cpu;
    uint32_t out = w16_arg32(a);
    uint16_t dev = w16_arg16(a);
    uint32_t fmt = w16_arg32(a), cb = w16_arg32(a), inst = w16_arg32(a), flags = w16_arg32(a);
    c->r[R_DX] = 0;
    pthread_mutex_lock(&g_wo.lock);
    int r = g_w16_prefs.sounds == W16_SOUNDS_OFF ? MMSYSERR_NODRIVER
            : dev != 0 && dev != 0xFFFF         ? MMSYSERR_BADDEVICEID
            : g_wo.open && !(flags & WAVE_FORMAT_QUERY) ? MMSYSERR_ALLOCATED : 0;
    if (!r) r = take_format(w, fmt);
    pthread_mutex_unlock(&g_wo.lock);
    if (r < 0) return -1;
    if (r || (flags & WAVE_FORMAT_QUERY)) { c->r[R_AX] = (uint16_t)r; return 22; }
    if (out) {
        uint8_t *p = cpu16_ptr(c, (uint16_t)(out >> 16), (uint16_t)out, 2, 1);
        if (!p) return -1;
        p[0] = HWAVEOUT;
        p[1] = 0;
    }
    int silent = !w16_sound_ready(w) || usnd_push_open() != 0;
    pthread_mutex_lock(&g_wo.lock);
    g_wo.open = 1;
    g_wo.paused = 0;
    g_wo.silent = silent;
    g_wo.flags = flags;
    g_wo.callback = cb;
    g_wo.instance = inst;
    g_wo.step = (uint32_t)(((uint64_t)g_wo.rate << 16) / USND_RATE);
    g_wo.phase = 0;
    g_wo.last[0] = g_wo.last[1] = 0;
    g_wo.queued = g_wo.pushed = g_wo.clock_base = 0;
    g_wo.clock_ms = w16_now_ms();
    pthread_mutex_unlock(&g_wo.lock);
    tell(w, MM_WOM_OPEN, 0);
    c->r[R_AX] = 0;
    return 22;
}

// waveOutClose(hWaveOut) -> UINT
// Windows: Closes the device; WAVERR_STILLPLAYING while buffers are queued.
// w16: The same.
static int api_waveOutClose(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    pthread_mutex_lock(&g_wo.lock);
    int r = h != HWAVEOUT || !g_wo.open ? MMSYSERR_INVALHANDLE : g_wo.nq ? WAVERR_STILLPLAYING : 0;
    if (!r) {
        if (!g_wo.silent) usnd_push_close();
        g_wo.open = 0;
    }
    pthread_mutex_unlock(&g_wo.lock);
    if (!r) tell(w, MM_WOM_CLOSE, 0);
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = 0;
    return 2;
}

// waveOutPrepareHeader(hWaveOut, lpWaveOutHdr, uSize) -> UINT
// waveOutUnprepareHeader(hWaveOut, lpWaveOutHdr, uSize) -> UINT
// Windows: Ready a WAVEHDR's buffer for waveOutWrite (its memory locked), and release it again --
// WAVERR_STILLPLAYING while it is queued.
// w16: The same in the header's flags; w16's memory never moves, so there is nothing to lock.
static int api_waveOutPrepareHeader(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t h = w16_arg16(a);
    uint32_t hdr = w16_arg32(a);
    w16_arg16(a);
    struct cpu16 *c = &w->cpu;
    const uint8_t *p = cpu16_ptr(c, (uint16_t)(hdr >> 16), (uint16_t)hdr, 32, 0);
    if (!p) return -1;
    c->r[R_DX] = 0;
    if (h != HWAVEOUT) { c->r[R_AX] = MMSYSERR_INVALHANDLE; return 8; }
    if (ordinal == 407 && (p[16] & WHDR_INQUEUE)) { c->r[R_AX] = WAVERR_STILLPLAYING; return 8; }
    set_flags(w, hdr, ordinal == 406 ? WHDR_PREPARED : 0, ordinal == 406 ? 0 : WHDR_PREPARED);
    c->r[R_AX] = 0;
    return 8;
}

// waveOutWrite(hWaveOut, lpWaveOutHdr, uSize) -> UINT
// Windows: Queues a prepared buffer to play after those before it; WHDR_DONE and MM_WOM_DONE when it
// has played.
// w16: The same: the buffer is copied now, in the device's format, so the program may reuse its
// memory as soon as the buffer is done -- and only then, as Windows asked. A buffer plays once:
// WHDR_BEGINLOOP's repeats are not played.
static int api_waveOutWrite(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t hdr = w16_arg32(a);
    w16_arg16(a);
    struct cpu16 *c = &w->cpu;
    const uint8_t *p = cpu16_ptr(c, (uint16_t)(hdr >> 16), (uint16_t)hdr, 32, 0);
    if (!p) return -1;
    c->r[R_DX] = 0;
    uint32_t data = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    uint32_t len = (uint32_t)(p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24);
    if (h != HWAVEOUT || !g_wo.open) { c->r[R_AX] = MMSYSERR_INVALHANDLE; return 8; }
    if (!(p[16] & WHDR_PREPARED)) { c->r[R_AX] = WAVERR_UNPREPARED; return 8; }
    const uint8_t *b = len ? cpu16_ptr(c, (uint16_t)(data >> 16), (uint16_t)data, len, 0) : 0;
    if (len && !b) return -1;
    pthread_mutex_lock(&g_wo.lock);
    long frames = 0;
    int16_t *pcm = convert(b, len, &frames);
    if (pcm && g_wo.nq == g_wo.qcap) {
        int cap = g_wo.qcap ? 2 * g_wo.qcap : 16;
        struct wbuf *q = realloc(g_wo.q, (size_t)cap * sizeof *q);
        if (q) { g_wo.q = q; g_wo.qcap = cap; }
    }
    int ok = pcm && g_wo.nq < g_wo.qcap;
    if (ok) {
        g_wo.queued += (uint64_t)frames;
        g_wo.q[g_wo.nq++] = (struct wbuf){ hdr, pcm, frames, g_wo.queued, 0 };
    } else {
        free(pcm);
    }
    pthread_mutex_unlock(&g_wo.lock);
    if (ok) set_flags(w, hdr, WHDR_INQUEUE, WHDR_DONE);
    c->r[R_AX] = ok ? 0 : MMSYSERR_NOMEM;
    return 8;
}

// waveOutPause(hWaveOut) -> UINT
// waveOutRestart(hWaveOut) -> UINT
// Windows: Stop playing where it is, and go on from there.
// w16: The same, give or take what the sound daemon already holds -- a sixth of a second.
static int api_waveOutPause(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t h = w16_arg16(a);
    pthread_mutex_lock(&g_wo.lock);
    int r = h != HWAVEOUT || !g_wo.open ? MMSYSERR_INVALHANDLE : 0;
    if (!r && ordinal == 409 && !g_wo.paused) {
        g_wo.clock_base = played();
        g_wo.clock_ms = 0;
        g_wo.paused = 1;
    } else if (!r && ordinal == 410 && g_wo.paused) {
        g_wo.clock_ms = w16_now_ms();
        g_wo.paused = 0;
    }
    pthread_mutex_unlock(&g_wo.lock);
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = 0;
    return 2;
}

// waveOutReset(hWaveOut) -> UINT
// Windows: Stops playing and returns every queued buffer, WHDR_DONE, its callback told, the position
// back to 0.
// w16: The same; what the sound daemon holds is dropped with it.
static int api_waveOutReset(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    pthread_mutex_lock(&g_wo.lock);
    int r = h != HWAVEOUT || !g_wo.open ? MMSYSERR_INVALHANDLE : 0;
    if (!r) {
        for (int i = 0; i < g_wo.nq; i++)
            if (!g_wo.q[i].done) { g_wo.q[i].done = 1; g_wo.due++; }
        if (!g_wo.silent) { usnd_push_close(); usnd_push_open(); }
        g_wo.fed = g_wo.nq;
        g_wo.fed_at = 0;
        g_wo.queued = g_wo.pushed = g_wo.clock_base = 0;
        for (int i = 0; i < g_wo.nq; i++) g_wo.q[i].end = 0;
        g_wo.clock_ms = g_wo.paused ? 0 : w16_now_ms();
    }
    pthread_mutex_unlock(&g_wo.lock);
    if (!r) w16_wave_service(w);
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = 0;
    return 2;
}

// waveOutGetPosition(hWaveOut, lpmmt, uSize) -> UINT
// Windows: How far the device has played since it opened or was reset, in the MMTIME format asked
// for -- samples, bytes or milliseconds, the device's choice when it cannot.
// w16: In samples (TIME_SAMPLES), or milliseconds when asked for them.
static int api_waveOutGetPosition(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t mmt = w16_arg32(a);
    w16_arg16(a);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(mmt >> 16), (uint16_t)mmt, 6, 1);
    if (!p) return -1;
    pthread_mutex_lock(&g_wo.lock);
    int r = h != HWAVEOUT || !g_wo.open ? MMSYSERR_INVALHANDLE : 0;
    uint64_t at = r ? 0 : played() * g_wo.rate / USND_RATE;
    pthread_mutex_unlock(&g_wo.lock);
    int ms = p[0] == 1;                          // TIME_MS
    uint32_t v = (uint32_t)(ms ? at * 1000 / (g_wo.rate ? g_wo.rate : 1) : at);
    if (!r) {
        p[0] = ms ? 1 : 2;                       // TIME_SAMPLES otherwise
        p[1] = 0;
        for (int i = 0; i < 4; i++) p[2 + i] = (uint8_t)(v >> 8 * i);
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = 0;
    return 8;
}

// waveInOpen(lphWaveIn, uDeviceID, lpFormat, dwCallback, dwInstance, dwFlags) -> UINT
// Windows: Opens a wave input device -- a sound card's microphone or line in -- to record into the
// program's buffers.
// w16: Answers MMSYSERR_NODRIVER: there is no wave input device.
// Why: Recording through SaunaOS's sound daemon is not written; nothing a program run so far records.
// Notice: A program that offers recording says it cannot; one that only checks for a device runs on.
// Whole: The waveIn calls over a capture stream. On the roadmap: "w16: wave input".
static int api_waveInOpen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal; (void)a;
    w->cpu.r[R_AX] = MMSYSERR_NODRIVER;
    w->cpu.r[R_DX] = 0;
    return 22;
}

const struct w16_api g_wave_api[] = {
    { 404, api_waveOutOpen },
    { 405, api_waveOutClose },
    { 406, api_waveOutPrepareHeader },
    { 407, api_waveOutPrepareHeader },
    { 408, api_waveOutWrite },
    { 409, api_waveOutPause },
    { 410, api_waveOutPause },
    { 411, api_waveOutReset },
    { 412, api_waveOutGetPosition },
    { 504, api_waveInOpen },
};
const unsigned g_wave_api_n = sizeof g_wave_api / sizeof g_wave_api[0];
