// An SDL3 program in a SaunaOS window, for tools/tests/gui/sdl_window_test.py
// (docs/sdl-design.md, stage 1). Not self-checking: it draws a picture
// the test reads back off the screen and logs, one line each, what SDL
// told it -- the test drives the input and judges the lines.
//
// The picture, 160x100 points, drawn in POINTS (no high pixel density),
// so at display.scale 125% the driver scales it: red up to x=80, blue
// to x=121, green after. At 1.25x the first edge lands ON a pixel edge
// (80 -> 100) and the second inside a pixel (121 -> 151.25), so sharp
// bilinear blends the second and only the second: plain bilinear would
// blend both, nearest neither.
//
// F toggles fullscreen. Lines, all on the kernel log ("sdlwin: ..."):
//   ready <w>x<h> pixels <pw>x<ph> scale <pct> driver <name>
//   key down|up sc=<scancode> <name>
//   text <utf8>
//   button <n> down|up <x> <y>     (points)
//   wheel <y>
//   mouse enter | mouse leave
//   resize <w> <h>                 (points)
//   close requested               (then it exits 0)
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include "rt/sys.h"

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
    sys_eprint("sdlwin: ");
    sys_eprint(line);
}

static void paint(SDL_Window *w) {
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (!s) return;
    const SDL_PixelFormatDetails *f = SDL_GetPixelFormatDetails(s->format);
    SDL_Rect red = { 0, 0, 80, s->h }, blue = { 80, 0, 41, s->h }, green = { 121, 0, s->w - 121, s->h };
    SDL_FillSurfaceRect(s, &red, SDL_MapRGB(f, NULL, 255, 0, 0));
    SDL_FillSurfaceRect(s, &blue, SDL_MapRGB(f, NULL, 0, 0, 255));
    SDL_FillSurfaceRect(s, &green, SDL_MapRGB(f, NULL, 0, 255, 0));
    SDL_UpdateWindowSurface(w);
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        say("init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("SDL window test", 160, 100, 0);
    if (!w) {
        say("window failed: %s", SDL_GetError());
        return 1;
    }
    SDL_StartTextInput(w);
    paint(w);
    int pw = 0, ph = 0, ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    say("ready %dx%d pixels %dx%d scale %d driver %s", ww, wh, pw, ph,
        (int)(SDL_GetWindowPixelDensity(w) * 100.0f + 0.5f), SDL_GetCurrentVideoDriver());

    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) continue;
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            say("key %s sc=%d %s", e.type == SDL_EVENT_KEY_DOWN ? "down" : "up",
                (int)e.key.scancode, SDL_GetScancodeName(e.key.scancode));
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F)
                SDL_SetWindowFullscreen(w, !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN));
            break;
        case SDL_EVENT_TEXT_INPUT:
            say("text %s", e.text.text);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            say("button %d %s %d %d", e.button.button,
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up",
                (int)(e.button.x + 0.5f), (int)(e.button.y + 0.5f));
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            say("wheel %d", (int)e.wheel.y);
            break;
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            say("mouse %s", e.type == SDL_EVENT_WINDOW_MOUSE_ENTER ? "enter" : "leave");
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            say("resize %d %d", e.window.data1, e.window.data2);
            paint(w);
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            paint(w);
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_QUIT:
            say("close requested");
            SDL_DestroyWindow(w);
            SDL_Quit();
            return 0;
        default:
            break;
        }
    }
}
