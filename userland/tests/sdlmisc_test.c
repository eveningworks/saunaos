// What an SDL3 port reaches for beyond a window and sound, for
// tools/tests/gui/sdl_stage3_test.py (docs/sdl-design.md, stage 3). Not
// self-checking: one mode per run, each logging what SDL answered
// ("sdlmisc[ <tag>]: ...") for the test to judge.
//
//   pref                 SDL_GetPrefPath("SaunaOS Tests", "sdlmisc"), and a
//                        file written and read back there
//   clipset <text>       SDL_SetClipboardText, then reads it back
//   clipwait             a window; logs "clip update <text>" on every
//                        SDL_EVENT_CLIPBOARD_UPDATE
//   url <url>            SDL_OpenURL
//   msgbox               a window, then four message boxes over it;
//                        after each, "answer <buttonID>" and what the
//                        clipboard holds
//   alone                a message box before SDL_Init(VIDEO)
//   rel                  a window; R toggles relative mode, motion logged
//                        as "rel <dx> <dy>" (summed, points) and
//                        "abs <x> <y>"
//   tag=<T>              told apart from a second copy
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"

static const char *g_tag = "";

static void say(const char *fmt, ...) {
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n] = '\n';
    line[n + 1] = '\0';
    sys_eprint("sdlmisc");
    if (*g_tag) {
        sys_eprint(" ");
        sys_eprint(g_tag);
    }
    sys_eprint(": ");
    sys_eprint(line);
}

static void paint(SDL_Window *w, Uint8 r, Uint8 g, Uint8 b) {
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (!s) return;
    SDL_FillSurfaceRect(s, NULL, SDL_MapRGB(SDL_GetPixelFormatDetails(s->format), NULL, r, g, b));
    SDL_UpdateWindowSurface(w);
}

static SDL_Window *window(const char *title) {
    SDL_Window *w = SDL_CreateWindow(title, 200, 120, 0);
    if (!w) {
        say("window failed: %s", SDL_GetError());
        return NULL;
    }
    paint(w, 40, 40, 40);
    return w;
}

static int pref(void) {
    char *p = SDL_GetPrefPath("SaunaOS Tests", "sdlmisc");
    if (!p) {
        say("pref failed: %s", SDL_GetError());
        return 1;
    }
    char file[512];
    snprintf(file, sizeof file, "%ssave.txt", p);
    FILE *f = fopen(file, "w");
    int wrote = f && fputs("level 3\n", f) >= 0;
    if (f) fclose(f);
    char back[32] = "";
    f = fopen(file, "r");
    if (f) {
        if (!fgets(back, sizeof back, f)) back[0] = '\0';
        fclose(f);
    }
    back[strcspn(back, "\n")] = '\0';
    say("pref %s wrote %d read '%s'", p, wrote, back);
    SDL_free(p);
    return 0;
}

static int clipset(const char *text) {
    if (!SDL_SetClipboardText(text)) {
        say("clipset failed: %s", SDL_GetError());
        return 1;
    }
    char *got = SDL_GetClipboardText();
    say("clipset has %d got '%s'", SDL_HasClipboardText() ? 1 : 0, got ? got : "(null)");
    SDL_free(got);
    return 0;
}

static int clipwait(void) {
    SDL_Window *w = window("SDL misc clip");
    if (!w) return 1;
    say("ready");
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) continue;
        if (e.type == SDL_EVENT_CLIPBOARD_UPDATE) {
            char *t = SDL_GetClipboardText();
            say("clip update %s", t ? t : "(null)");
            SDL_free(t);
        } else if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            return 0;
        }
    }
}

static int msgbox(void) {
    SDL_Window *w = window("SDL misc box");
    if (!w) return 1;
    say("ready");
    static const SDL_MessageBoxButtonData three[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 11, "Retry" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 22, "Quit" },
        { 0, 33, "Help" },
    };
    static const SDL_MessageBoxButtonData one[] = { { 0, 44, "Fine" } };
    struct { Uint32 flags; const char *title, *msg; const SDL_MessageBoxButtonData *b; int n; } boxes[] = {
        { SDL_MESSAGEBOX_ERROR, "Game data",
          "No game data was found in /home/user/Games/sky. Copy the game's files into that folder and try again.",
          three, 3 },
        { SDL_MESSAGEBOX_WARNING, "Second", "Esc answers Quit.", three, 3 },
        { SDL_MESSAGEBOX_INFORMATION, "Third", "No escape button: closing it answers -1.", one, 1 },
        { 0, "Fourth", "Return answers Retry.", three, 3 },
    };
    for (int i = 0; i < 4; i++) {
        SDL_MessageBoxData d = { boxes[i].flags, w, boxes[i].title, boxes[i].msg, boxes[i].n, boxes[i].b, NULL };
        int id = -2;
        bool ok = SDL_ShowMessageBox(&d, &id);
        char *clip = SDL_GetClipboardText();
        say("answer %d ok %d clip '%s'", id, ok ? 1 : 0, clip ? clip : "");
        SDL_free(clip);
    }
    SDL_DestroyWindow(w);
    return 0;
}

static int alone(void) {
    int id = -2;
    static const SDL_MessageBoxButtonData b[] = { { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 7, "OK" } };
    SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, NULL, "Before video", "No SDL window exists yet.", 1, b, NULL };
    bool ok = SDL_ShowMessageBox(&d, &id);
    say("alone answer %d ok %d error '%s'", id, ok ? 1 : 0, ok ? "" : SDL_GetError());
    return 0;
}

static int rel(void) {
    SDL_Window *w = window("SDL misc rel");
    if (!w) return 1;
    SDL_StartTextInput(w);
    say("ready");
    float sx = 0, sy = 0;
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 300)) {
            if (sx != 0 || sy != 0) {   // a quiet spell ends one run of motion
                say("rel %d %d", (int)SDL_lroundf(sx), (int)SDL_lroundf(sy));
                sx = sy = 0;
            }
            continue;
        }
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_R) {
                bool on = !SDL_GetWindowRelativeMouseMode(w);
                bool ok = SDL_SetWindowRelativeMouseMode(w, on);
                say("relative %s ok %d", on ? "on" : "off", ok ? 1 : 0);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (SDL_GetWindowRelativeMouseMode(w)) {
                sx += e.motion.xrel;
                sy += e.motion.yrel;
            } else {
                say("abs %d %d", (int)e.motion.x, (int)e.motion.y);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            say("button %d down", e.button.button);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            say("focus %s", e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "gained" : "lost");
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return 0;
        default:
            break;
        }
    }
}

int main(int argc, char **argv) {
    const char *mode = "";
    const char *arg = "";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "tag=", 4)) g_tag = argv[i] + 4;
        else if (!*mode) mode = argv[i];
        else arg = argv[i];
    }
    if (!strcmp(mode, "alone")) return alone();
    if (!strcmp(mode, "pref")) return pref();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        say("init failed: %s", SDL_GetError());
        return 1;
    }
    int rc = 1;
    if (!strcmp(mode, "clipset")) rc = clipset(arg);
    else if (!strcmp(mode, "clipwait")) rc = clipwait();
    else if (!strcmp(mode, "url")) {
        bool ok = SDL_OpenURL(arg);
        say("url %s ok %d error '%s'", arg, ok ? 1 : 0, ok ? "" : SDL_GetError());
        rc = 0;
    } else if (!strcmp(mode, "msgbox")) rc = msgbox();
    else if (!strcmp(mode, "rel")) rc = rel();
    else say("unknown mode '%s'", mode);
    SDL_Quit();
    say("exit %d", rc);
    return rc;
}
