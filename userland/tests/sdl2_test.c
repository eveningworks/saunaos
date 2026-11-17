// SDL2 programs on SaunaOS through sdl2-compat (docs/sdl-design.md).
//
// /lib/libSDL2-2.0.so.0 is sdl2-compat: SDL2's API, carried out by
// SDL3, which it dlopen()s by name ("libSDL3.so.0") and binds symbol by
// symbol. So this test is mostly about that LOAD -- a symbol missing
// from libSDL3, or a dlopen that ld-toy refuses, fails here and nowhere
// else -- and about the two libraries exporting the SAME NAMES: without
// -Bsymbolic on libSDL3, SDL3's calls to itself landed in sdl2-compat's
// SDL2 functions, and video init failed with no error set. Then one
// SDL2-shaped round trip each for video, threads and timers, with SDL's
// dummy video driver.
#include <string.h>
#include <SDL2/SDL.h>

#include "base/utest.h"

static int thread_double(void *arg) {
    return 2 * *(int *)arg;
}

int main(void) {
    utest_begin("sdl2_test", "SDL2's API through sdl2-compat over SDL3", 0);
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        utest_checkf(0, "SDL_Init: %s", SDL_GetError());
        return utest_end();
    }
    utest_check(1, "SDL_Init: sdl2-compat found libSDL3.so.0 and bound it");

    SDL_version v;
    SDL_GetVersion(&v);
    utest_checkf(v.major == 2 && v.minor >= 30, "SDL_GetVersion() says SDL2 (%d.%d.%d)",
                 v.major, v.minor, v.patch);
    utest_checkf(strcmp(SDL_GetPlatform(), "SaunaOS") == 0,
                 "SDL_GetPlatform() is SDL3's answer, \"SaunaOS\" (got \"%s\")", SDL_GetPlatform());

    // SDL2's window surface: fill a rectangle and read the pixel back.
    SDL_Window *w = SDL_CreateWindow("sdl2_test", 0, 0, 40, 30, 0);
    SDL_Surface *s = w ? SDL_GetWindowSurface(w) : NULL;
    if (!s) {
        utest_checkf(0, "a window and its surface: %s", SDL_GetError());
    } else {
        SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 0, 0));
        SDL_Rect r = { 5, 5, 10, 10 };
        SDL_FillRect(s, &r, SDL_MapRGB(s->format, 0, 200, 0));
        Uint8 rr, gg, bb;
        Uint32 in, out;
        SDL_LockSurface(s);
        memcpy(&in, (Uint8 *)s->pixels + 8 * s->pitch + 8 * s->format->BytesPerPixel, 4);
        memcpy(&out, (Uint8 *)s->pixels + 25 * s->pitch + 30 * s->format->BytesPerPixel, 4);
        SDL_UnlockSurface(s);
        SDL_GetRGB(in, s->format, &rr, &gg, &bb);
        utest_checkf(rr == 0 && gg == 200 && bb == 0, "the filled rect reads back green (%u,%u,%u)",
                     rr, gg, bb);
        SDL_GetRGB(out, s->format, &rr, &gg, &bb);
        utest_checkf(rr == 0 && gg == 0 && bb == 0, "outside it is still black (%u,%u,%u)", rr, gg, bb);
        utest_check(SDL_UpdateWindowSurface(w) == 0, "SDL_UpdateWindowSurface");
    }
    if (w) SDL_DestroyWindow(w);

    int n = 21, got = -1;
    SDL_Thread *t = SDL_CreateThread(thread_double, "double", &n);
    if (t) SDL_WaitThread(t, &got);
    utest_checkf(got == 42, "an SDL2 thread's result comes back (%d)", got);

    Uint32 t0 = SDL_GetTicks();
    SDL_Delay(25);
    Uint32 dt = SDL_GetTicks() - t0;
    utest_checkf(dt >= 25 && dt < 2000, "SDL_Delay(25) took %u ms", dt);

    SDL_Quit();
    return utest_end();
}
