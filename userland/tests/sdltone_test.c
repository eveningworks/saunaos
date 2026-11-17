// A tone through SDL3's audio, for tools/tests/sweep/sdl_audio_test.py
// (docs/sdl-design.md, stage 2): 1.5 s of 1000 Hz, mono float at
// 44.1 kHz, pushed into an SDL_AudioStream -- SDL converts it to the
// device's stereo S16 at 48 kHz. The host records what the sound card
// emitted and measures the pitch; this only plays, and says so on the
// kernel log: "sdltone: driver <name>", then "sdltone: played" or
// "sdltone: failed: <why>". sdl2tone_test is the same through SDL2.
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include "rt/sys.h"

#define SECONDS 1.5f
#define HZ 1000.0f

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
    sys_eprint("sdltone: ");
    sys_eprint(line);
}

int main(void) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        say("failed: init: %s", SDL_GetError());
        return 1;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, 44100 };
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!st) {
        say("failed: open: %s", SDL_GetError());
        return 1;
    }
    say("driver %s", SDL_GetCurrentAudioDriver());
    int n = (int)(spec.freq * SECONDS);
    float *tone = SDL_malloc((size_t)n * sizeof *tone);
    if (!tone) return 1;
    for (int i = 0; i < n; i++) tone[i] = 0.5f * SDL_sinf(2.0f * SDL_PI_F * HZ * (float)i / (float)spec.freq);
    SDL_PutAudioStreamData(st, tone, n * (int)sizeof *tone);
    SDL_FlushAudioStream(st);
    SDL_ResumeAudioStreamDevice(st);
    while (SDL_GetAudioStreamQueued(st) > 0) SDL_Delay(20);
    SDL_DestroyAudioStream(st);   // closes the device: the driver drains what is queued
    SDL_free(tone);
    SDL_Quit();
    say("played");
    return 0;
}
