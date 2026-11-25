// SDL3 on SaunaOS, stage 0 (docs/sdl-design.md): /lib/libSDL3.so.0
// loads, and the parts of it that need nothing from the desktop work.
//
// No window appears: SDL's "dummy" video driver keeps windows in memory
// and its "disk" audio driver writes what is played to a file. That is
// enough to prove the library over tolibc -- threads, semaphores,
// timers, the event queue, the software renderer and blitters, the
// audio thread and its conversion, and SaunaOS's glue modules (time and
// filesystem) -- before the drivers that reach the desktop exist.
//
// EVERY CHECK IS A ROUND TRIP: pixels read back from what was drawn,
// samples read back from the file the audio thread wrote, a value a
// thread computed. "SDL_Init returned true" alone would pass with every
// subsystem broken.
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "base/usetting.h"
#include "base/utest.h"

#define AUDIO_FILE "/tmp/sdl3_test.raw"
#define AUDIO_FRAMES 4800   // 0.1 s of 48 kHz stereo

static int thread_sum(void *arg) {
    int n = *(int *)arg, s = 0;
    for (int i = 1; i <= n; i++) s += i;
    return s;
}

static int signal_later(void *arg) {
    SDL_Delay(20);
    SDL_SignalSemaphore((SDL_Semaphore *)arg);
    return 0;
}

static Uint32 timer_events;
static Uint32 SDLCALL on_timer(void *user, SDL_TimerID id, Uint32 interval) {
    (void)user; (void)id; (void)interval;
    SDL_Event e;
    SDL_zero(e);
    e.type = timer_events;
    e.user.code = 42;
    SDL_PushEvent(&e);
    return 0;   // once
}

static void check_errors(void) {
    SDL_SetError("probe %d", 7);
    utest_checkf(strcmp(SDL_GetError(), "probe 7") == 0,
                 "SDL_SetError's message comes back from SDL_GetError (\"%s\")", SDL_GetError());
    SDL_ClearError();
}

static void check_platform(void) {
    utest_checkf(strcmp(SDL_GetPlatform(), "SaunaOS") == 0,
                 "SDL_GetPlatform() is \"SaunaOS\" (got \"%s\")", SDL_GetPlatform());
    int v = SDL_GetVersion();
    utest_checkf(SDL_VERSIONNUM_MAJOR(v) == 3, "the linked library is SDL 3 (%d.%d.%d)",
                 SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
}

static void check_threads(void) {
    int n = 1000;
    SDL_Thread *t = SDL_CreateThread(thread_sum, "sum", &n);
    int got = -1;
    if (t) SDL_WaitThread(t, &got);
    utest_checkf(got == 500500, "a thread's result comes back through SDL_WaitThread (%d)", got);

    SDL_Semaphore *sem = SDL_CreateSemaphore(0);
    SDL_Thread *s = SDL_CreateThread(signal_later, "signal", sem);
    utest_check(sem && s && SDL_WaitSemaphoreTimeout(sem, 2000),
                "a semaphore signalled by another thread wakes the waiter");
    utest_check(!SDL_TryWaitSemaphore(sem), "...and is taken only once");
    if (s) SDL_WaitThread(s, NULL);
    SDL_DestroySemaphore(sem);
}

static void check_timers(void) {
    Uint64 t0 = SDL_GetTicks();
    SDL_Delay(30);
    Uint64 dt = SDL_GetTicks() - t0;
    utest_checkf(dt >= 30 && dt < 2000, "SDL_Delay(30) took %llu ms", (unsigned long long)dt);

    timer_events = SDL_RegisterEvents(1);
    utest_check(SDL_AddTimer(10, on_timer, NULL) != 0, "SDL_AddTimer");
    SDL_Event e;
    int seen = 0;
    while (!seen && SDL_WaitEventTimeout(&e, 2000)) {
        if (e.type == timer_events && e.user.code == 42) seen = 1;
    }
    utest_check(seen, "the timer thread's event reaches SDL_WaitEventTimeout");
}

// Clear to blue, fill a red square, read both back.
static void check_renderer(void) {
    SDL_Window *w = NULL;
    SDL_Renderer *r = NULL;
    if (!SDL_CreateWindowAndRenderer("sdl3_test", 64, 48, 0, &w, &r)) {
        utest_checkf(0, "SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return;
    }
    utest_checkf(strcmp(SDL_GetRendererName(r), "software") == 0,
                 "the renderer is SDL's software one (%s)", SDL_GetRendererName(r));
    SDL_SetRenderDrawColor(r, 0, 0, 255, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
    SDL_FRect sq = { 8, 8, 16, 16 };
    SDL_RenderFillRect(r, &sq);
    SDL_Surface *got = SDL_RenderReadPixels(r, NULL);
    SDL_Surface *rgba = got ? SDL_ConvertSurface(got, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (!rgba) {
        utest_checkf(0, "SDL_RenderReadPixels: %s", SDL_GetError());
    } else {
        const Uint8 *in = (const Uint8 *)rgba->pixels + 12 * rgba->pitch + 12 * 4;
        const Uint8 *out = (const Uint8 *)rgba->pixels + 40 * rgba->pitch + 50 * 4;
        utest_checkf(in[0] == 255 && in[1] == 0 && in[2] == 0,
                     "inside the square is red (%u,%u,%u)", in[0], in[1], in[2]);
        utest_checkf(out[0] == 0 && out[1] == 0 && out[2] == 255,
                     "outside it is the clear colour, blue (%u,%u,%u)", out[0], out[1], out[2]);
    }
    SDL_DestroySurface(rgba);
    SDL_DestroySurface(got);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
}

// The audio thread pulls a ramp through a stream and the disk driver
// writes it out; the ramp must be in the file, in order.
static void check_audio(void) {
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, 48000 };
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!st) {
        utest_checkf(0, "SDL_OpenAudioDeviceStream: %s", SDL_GetError());
        return;
    }
    static Sint16 ramp[AUDIO_FRAMES * 2];
    for (int i = 0; i < AUDIO_FRAMES * 2; i++) ramp[i] = (Sint16)(i * 7 - 30000);
    SDL_PutAudioStreamData(st, ramp, sizeof ramp);
    SDL_FlushAudioStream(st);
    SDL_ResumeAudioStreamDevice(st);
    for (int i = 0; i < 200 && SDL_GetAudioStreamQueued(st) > 0; i++) SDL_Delay(10);
    SDL_Delay(100);   // the device thread's last buffer, past the queue
    SDL_DestroyAudioStream(st);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);   // closes the device, and the file

    size_t len = 0;
    Uint8 *file = SDL_LoadFile(AUDIO_FILE, &len);
    int found = 0;
    for (size_t off = 0; file && off + sizeof ramp <= len; off += 2) {
        if (memcmp(file + off, ramp, sizeof ramp) == 0) { found = 1; break; }
    }
    utest_checkf(found, "the whole ramp is in what the audio thread wrote (%zu bytes)", len);
    SDL_free(file);
    remove(AUDIO_FILE);
}

static void check_glue(void) {
    const char *base = SDL_GetBasePath();
    utest_checkf(base && strcmp(base, "/tests/") == 0,
                 "SDL_GetBasePath() is this program's directory (%s)", base ? base : SDL_GetError());
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);   // SDL's cached copy
    utest_checkf(docs && strcmp(docs, "/home/user/Documents/") == 0,
                 "SDL_GetUserFolder(DOCUMENTS) is the personal folder (%s)", docs ? docs : SDL_GetError());

    SDL_Time now;
    SDL_DateTime utc;
    int ok = SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &utc, false);
    utest_checkf(ok && utc.year >= 2026 && utc.utc_offset == 0,
                 "the clock reads a plausible UTC year (%d)", ok ? utc.year : 0);

    // Local time in a zone that is NOT UTC, or a glue that ignored the
    // zone would pass: Tokyo is +9 all year (utz_test's choice). The
    // setting is put back whatever happened.
    char original[64] = "utc";
    usetting_get("system.timezone", original, sizeof original);
    if (usetting_set("system.timezone", "tokyo") != 0) {
        tzset();
        SDL_DateTime local;
        SDL_Time moment = SDL_SECONDS_TO_NS((SDL_Time)1768446000);   // 2026-01-15 03:00 UTC
        int got = SDL_TimeToDateTime(moment, &local, true);
        utest_checkf(got && local.hour == 12 && local.day == 15 && local.utc_offset == 9 * 3600,
                     "Tokyo's local time is 12:00 at +32400 s (%02d:00 at %d s)",
                     got ? local.hour : -1, got ? local.utc_offset : -1);
        usetting_set("system.timezone", original);
        tzset();
    } else {
        utest_check(0, "the timezone can be set to Tokyo for the local-time check");
    }
}

int main(void) {
    utest_begin("sdl3_test", "SDL3 over tolibc, with the dummy video and disk audio drivers", 0);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "disk");
    SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, AUDIO_FILE);

    check_platform();
    check_errors();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        utest_checkf(0, "SDL_Init: %s", SDL_GetError());
        return utest_end();
    }
    utest_checkf(strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0, "video driver %s",
                 SDL_GetCurrentVideoDriver());
    check_threads();
    check_timers();
    check_renderer();
    check_audio();
    check_glue();
    SDL_Quit();
    return utest_end();
}
