// A tone through SDL2's audio CALLBACK -- ScummVM's shape -- by way of
// sdl2-compat, for tools/tests/sweep/sdl_audio_test.py: 1.5 s of 750 Hz,
// mono S16 at 22.05 kHz, in buffers bigger than usnd's whole queue. A different pitch from sdltone_test's, so a
// recording cannot be the other program's. Lines: "sdltone2: driver
// <name>", then "sdltone2: played" or "sdltone2: failed: <why>".
#include <SDL2/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include "rt/sys.h"

#define RATE 22050
#define HZ 750.0f

static void say(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n] = '\n';
    line[n + 1] = '\0';
    sys_eprint("sdltone2: ");
    sys_eprint(line);
}

static SDL_atomic_t g_done;
static int g_pos, g_left = RATE * 3 / 2;

// The audio thread's: g_pos and g_left are its alone.
static void fill(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    Sint16 *out = (Sint16 *)stream;
    for (int i = 0; i < len / 2; i++, g_pos++) {
        out[i] = g_left > 0 ? (Sint16)(16000.0f * SDL_sinf(2.0f * 3.14159265f * HZ * (float)g_pos / RATE)) : 0;
        if (g_left > 0 && --g_left == 0) SDL_AtomicSet(&g_done, 1);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        say("failed: init: %s", SDL_GetError());
        return 1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 16384;   // twice usnd's queue: the driver must push it in pieces
    want.callback = fill;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        say("failed: open: %s", SDL_GetError());
        return 1;
    }
    say("driver %s", SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(dev, 0);
    while (!SDL_AtomicGet(&g_done)) SDL_Delay(20);
    SDL_Delay(200);   // the callback's last buffer, still ahead of the card
    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    say("played");
    return 0;
}
