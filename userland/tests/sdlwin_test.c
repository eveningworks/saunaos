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
// F toggles fullscreen. Arguments, any of:
//   tag=<T>     title "SDL window test <T>", lines "sdlwin <T>: ..." -- a
//               second window, told apart
//   borderless  no frame; the top 12 points drag it (SDL_SetWindowHitTest)
//   wait16      waits in 16 ms slices (a game's frame), not for ever
// An event watcher repaints on every key and its text, as a program
// drawing from one would -- from INSIDE the driver's pump.
// Lines, all on the kernel log ("sdlwin: ..."):
//   ready <w>x<h> pixels <pw>x<ph> scale <pct> driver <name>
//   number <1.5 as printf writes it> locale <LC_NUMERIC's name>
//   watch nested                   (the watcher ran inside itself)
//   key down|up sc=<scancode> <name>
//   text <utf8>
//   button <n> down|up <x> <y>     (points)
//   wheel <y>
//   mouse enter | mouse leave
//   resize <w> <h>                 (points)
//   close requested               (then it exits 0)
#include <SDL3/SDL.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"

static const char *g_tag = "";

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
    sys_eprint("sdlwin");
    if (*g_tag) {
        sys_eprint(" ");
        sys_eprint(g_tag);
    }
    sys_eprint(": ");
    sys_eprint(line);
}

static SDL_HitTestResult strip(SDL_Window *w, const SDL_Point *pt, void *data) {
    (void)w; (void)data;
    return pt->y < 12 ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

static int g_in_watch;

static bool watch(void *data, SDL_Event *e) {
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_TEXT_INPUT) return true;
    if (g_in_watch) say("watch nested");
    g_in_watch = 1;
    SDL_UpdateWindowSurface((SDL_Window *)data);
    g_in_watch = 0;
    return true;
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

int main(int argc, char **argv) {
    int borderless = 0, wait16 = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "tag=", 4)) g_tag = argv[i] + 4;
        else if (!strcmp(argv[i], "borderless")) borderless = 1;
        else if (!strcmp(argv[i], "wait16")) wait16 = 1;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        say("init failed: %s", SDL_GetError());
        return 1;
    }
    char title[48];
    snprintf(title, sizeof title, "SDL window test%s%s", *g_tag ? " " : "", g_tag);
    SDL_Window *w = SDL_CreateWindow(title, 160, 100, borderless ? SDL_WINDOW_BORDERLESS : 0);
    if (!w) {
        say("window failed: %s", SDL_GetError());
        return 1;
    }
    if (borderless && !SDL_SetWindowHitTest(w, strip, NULL)) say("hit test refused: %s", SDL_GetError());
    SDL_AddEventWatch(watch, w);
    SDL_StartTextInput(w);
    paint(w);
    int pw = 0, ph = 0, ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    say("ready %dx%d pixels %dx%d scale %d driver %s", ww, wh, pw, ph,
        (int)(SDL_GetWindowPixelDensity(w) * 100.0f + 0.5f), SDL_GetCurrentVideoDriver());
    // SDL_Init must not have changed it: a program starts in "C".
    say("number %.1f locale %s", 1.5, setlocale(LC_NUMERIC, NULL));

    for (;;) {
        SDL_Event e;
        if (wait16 ? !SDL_WaitEventTimeout(&e, 16) : !SDL_WaitEvent(&e)) continue;
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
