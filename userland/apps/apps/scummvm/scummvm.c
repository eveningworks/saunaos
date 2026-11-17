// ScummVM's front (docs/sdl-design.md, stage 4): what the Start menu's
// ScummVM runs. With a free game here it hands over to /usr/bin/scummvm,
// ScummVM's own launcher, at once, with no window of its own; with none,
// or as `scummvm --games` (Free Adventure Games), it shows the card the
// four freeware games download from (ui/uui_datacard.h, mockup G2) and
// starts ScummVM when they are in.
//
// THE GAMES ARE REGISTERED BY SCUMMVM ITSELF: `scummvm --add --path=DIR`
// detects a game and writes it to scummvm.ini, so this never writes
// ScummVM's file. A game is marked offered (`added_<id>` in GAMES_CONF)
// once ScummVM's list NAMES it, so one the person then removes from that
// list stays removed -- and a folder ScummVM did not take is asked again
// at the next start rather than hidden for good (marking every attempt
// is how a failed add once kept a game out of the list for ever). That
// runs a ScummVM per new game, seconds each, so it happens after the
// card's window has closed, never inside its event loop.
#include "base/uconf.h"
#include "base/udatafetch.h"
#include "base/ulog.h"
#include "ui/uapp.h"
#include "ui/uui_datacard.h"
#include "uhttp.h"
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SCUMMVM    "/usr/bin/scummvm"
#define GAMES_DIR  "/usr/share/games"
#define GAMES_CONF "/etc/scummvm-games.conf"   // `mirror=`, a stand-in download server

extern char **environ;

// The freeware games ScummVM's site offers, each pinned by the .sha256
// published beside it on downloads.scummvm.org (2026-11-17). Every
// licence is the same freeware text: share it unchanged and free, no fee
// for the game itself -- a notice to read, not an agreement.
#define EXTRAS "https://downloads.scummvm.org/frs/extras/"
#define SHARE_NOTICE                                                                         \
    "You may share this game for free, unchanged, with its licence and notices; a "         \
    "reasonable copying fee is allowed, but not a fee for the game itself.\n"                \
    "It comes from ScummVM's own site, where its makers released it as freeware. "           \
    "Its SHA-256 is checked before ScummVM sees it."

static const struct udata_part BASS[] = { {
    EXTRAS "Beneath%20a%20Steel%20Sky/bass-cd-1.2.zip",
    "53209b9400eab6fd7fa71518b2f357c8de75cfeaa5ba57024575ab79cc974593",
    69377781, UDATA_TREE, "bass-cd-1.2/",
} };
static const struct udata_part FOTAQ[] = { {
    EXTRAS "Flight%20of%20the%20Amazon%20Queen/FOTAQ_Talkie-original.zip",
    "a298e68243f18a741d4816ef636a5a77a1593816fb2c9e23a09124c35a95dfec",
    112625523, UDATA_TREE, "",
} };
static const struct udata_part LURE[] = { {
    EXTRAS "Lure%20of%20the%20Temptress/lure-1.1.zip",
    "f3178245a1483da1168c3a11e70b65d33c389f1f5df63d4f3a356886c1890108",
    5678861, UDATA_TREE, "lure/",
} };
// The game, then its music add-on (Ogg Vorbis), whose tracks go beside it.
static const struct udata_part DRASCULA[] = {
    { EXTRAS "Drascula_%20The%20Vampire%20Strikes%20Back/drascula-1.0.zip",
      "b731f6cb5a22ba8b4c3b3362f570b9a10a67b6cb0b395394b19a94b36e4e42de",
      32842993, UDATA_TREE, "" },
    { EXTRAS "Drascula_%20The%20Vampire%20Strikes%20Back/drascula-audio-2.0.zip",
      "7e6afba36eed13dd02e0360119e9a6a8d0e7b334ddc11d7c46ab7faceb8fe401",
      36531704, UDATA_TREE, "audio/" },
};

static const struct udata_item GAMES[] = {
    { "bass", "Beneath a Steel Sky", "Revolution Software, 1994. CD version with speech.",
      "downloads.scummvm.org", BASS, 1, "Freeware (Revolution Software).", SHARE_NOTICE, 0,
      "beneath-a-steel-sky.txt", "bass-cd-1.2/readme.txt" },
    { "fotaq", "Flight of the Amazon Queen", "John Passfield and Steve Stamatiadis, 1995. With speech.",
      "downloads.scummvm.org", FOTAQ, 1, "Freeware (John Passfield, Steve Stamatiadis).",
      SHARE_NOTICE, 0, "flight-of-the-amazon-queen.txt", "readme.txt" },
    { "lure", "Lure of the Temptress", "Revolution Software, 1992.",
      "downloads.scummvm.org", LURE, 1, "Freeware (Revolution Software).", SHARE_NOTICE, 0,
      "lure-of-the-temptress.txt", "lure/LICENSE.txt" },
    { "drascula", "Drascula: The Vampire Strikes Back", "Alcachofa Soft, 1996. With its music.",
      "downloads.scummvm.org", DRASCULA, 2, "Freeware (Alcachofa Soft).", SHARE_NOTICE, 0,
      "drascula.txt", "readme.txt" },
};
#define GAME_COUNT ((int)(sizeof GAMES / sizeof GAMES[0]))

// --- handing over to ScummVM -------------------------------------------------

// Where ScummVM keeps scummvm.ini: its POSIX backend's own rule.
static char *ini_path(void) {
    const char *x = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *p = NULL;
    int n = x && *x ? asprintf(&p, "%s/scummvm/scummvm.ini", x)
                    : asprintf(&p, "%s/.config/scummvm/scummvm.ini", home && *home ? home : "");
    return n < 0 ? NULL : p;
}

// Is `dir` a game's path in scummvm.ini already (with or without a
// trailing '/')? Read line by line: the file is ScummVM's.
static int registered(const char *dir) {
    char *ini = ini_path();
    FILE *f = ini ? fopen(ini, "r") : NULL;
    free(ini);
    if (!f) return 0;
    char *line = NULL;
    size_t cap = 0, n = strlen(dir);
    int hit = 0;
    while (!hit && getline(&line, &cap, f) > 0) {
        if (strncmp(line, "path=", 5) || strncmp(line + 5, dir, n)) continue;
        const char *rest = line + 5 + n;
        rest += *rest == '/';
        hit = !*rest || *rest == '\n' || *rest == '\r';
    }
    free(line);
    fclose(f);
    return hit;
}

static int offered(const struct udata_item *g) {
    char key[48], v[8];
    snprintf(key, sizeof key, "added_%s", g->id);
    return uconf_get(GAMES_CONF, key, v, sizeof v) && !strcmp(v, "yes");
}

static int run_wait(char *const argv[]) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) return -1;
    int st = 0;
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// The XDG base directories, SET when the environment lacks them and made
// when missing (a machine updated from before they were shipped).
// ScummVM makes only its own folder under them, and without them writes
// scummvm.ini into its working directory -- where this front never looks,
// so the game it just added was in a list nobody read. The defaults are
// /etc/environment's, for a ScummVM started with no environment at all
// (the kernel shell, the debug console's `sh`).
static void make_xdg_dirs(void) {
    static const char *const VARS[] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME" };
    static const char *const DEFAULTS[] = { "/home/.config", "/home/.local/share", "/home/.cache" };
    for (int i = 0; i < 3; i++) {
        const char *now = getenv(VARS[i]);
        if (!now || *now != '/') setenv(VARS[i], DEFAULTS[i], 1);
        const char *v = getenv(VARS[i]);
        char *p = v && *v == '/' ? strdup(v) : NULL;
        for (char *s = p ? p + 1 : NULL; s; s = strchr(s + 1, '/')) {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
        if (p) mkdir(p, 0755);
        free(p);
    }
}

// Every game here that ScummVM's list does not name yet, added by ScummVM
// -- once, unless the list itself is gone (a fresh /home).
static void register_games(void) {
    make_xdg_dirs();
    char *ini = ini_path();
    int have_ini = ini && access(ini, F_OK) == 0;
    free(ini);
    for (int i = 0; i < GAME_COUNT; i++) {
        char *dir = udata_path(GAMES_DIR, &GAMES[i]), *arg = NULL;
        if (dir && access(dir, F_OK) == 0 && (!have_ini || !offered(&GAMES[i])) && !registered(dir) &&
            asprintf(&arg, "--path=%s", dir) >= 0) {
            char *argv[] = { SCUMMVM, "--add", arg, NULL };
            int rc = run_wait(argv);
            if (registered(dir)) {
                char key[48];
                snprintf(key, sizeof key, "added_%s", GAMES[i].id);
                uconf_set(GAMES_CONF, key, "yes");
                ulogf("scummvm: added %s to ScummVM's list (exit %d)\n", GAMES[i].id, rc);
            } else {
                ulogf("scummvm: ScummVM did not list %s (exit %d); asked again next start\n",
                      GAMES[i].id, rc);
            }
        }
        free(arg);
        free(dir);
    }
}

static int any_game(void) {
    for (int i = 0; i < GAME_COUNT; i++)
        if (udata_present(GAMES_DIR, &GAMES[i])) return 1;
    return 0;
}

// --- the card ----------------------------------------------------------------

static struct uui_datacard g_card;
static int g_games_asked;          // `--games`: the card, whatever is here
static int g_launch;               // the card closed asking for ScummVM

static void start_scummvm(struct uapp *a) {
    g_launch = 1;
    uapp_quit(a, 0);
}

// Out of the event loop: the games registered, then ScummVM in this
// process's place. Returns only on failure.
static int launch(void) {
    register_games();
    ulogf("scummvm: starting ScummVM\n");
    char *args[] = { SCUMMVM, NULL };
    execv(SCUMMVM, args);
    fprintf(stderr, "scummvm: could not start %s\n", SCUMMVM);
    ulogf("scummvm: could not start %s\n", SCUMMVM);
    return 1;
}

static void card_done(struct uapp *a, const struct udata_item *last, void *ctx) {
    (void)last; (void)ctx;
    start_scummvm(a);
}

static void card_aux(struct uapp *a, void *ctx) {
    (void)ctx;
    start_scummvm(a);
}

static void card_cancel(struct uapp *a, void *ctx) {
    (void)ctx;
    uapp_quit(a, 0);
}

static const struct uui_datacard_desc CARD = {
    .log = "scummvm", .conf = GAMES_CONF, .dir = GAMES_DIR,
    .items = GAMES, .count = GAME_COUNT, .multi = 1,
    .title_empty = "ScummVM needs a game", .title_some = "Free games for ScummVM",
    .subtitle = "Choose the games to download. They go to " GAMES_DIR ".",
    .aux_label = "Open ScummVM",
    .noun = "game", .nouns = "games",
    .download = uhttp_download,
    .on_done = card_done, .on_aux = card_aux, .on_cancel = card_cancel,
};

static void on_open(struct uapp *a) {
    uui_datacard_open(&g_card, a, &CARD, NULL, g_games_asked);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    uui_datacard_draw(&g_card, uapp_surface(d));
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a;
    uui_datacard_press(&g_card, x, y, buttons);
}
static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a; (void)buttons;
    uui_datacard_motion(&g_card, x, y);
}
static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a;
    uui_datacard_release(&g_card, x, y, buttons);
}
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)a;
    uui_datacard_key(&g_card, key, mods);
}
static void on_phys_key(struct uapp *a, int keycode, int down, unsigned mods) {
    (void)a; (void)mods;
    uui_datacard_phys(&g_card, keycode, down);
}
static int on_user(struct uapp *a, int a0, int a1) {
    (void)a;
    return uui_datacard_user(&g_card, a0, a1);
}

int main(int argc, char **argv) {
    g_games_asked = argc > 1 && !strcmp(argv[1], "--games");
    if (access(SCUMMVM, X_OK) != 0) {
        fprintf(stderr, "scummvm: %s is not installed (the ScummVM component)\n", SCUMMVM);
        return 1;
    }
    if (!g_games_asked && any_game()) return launch();
    struct uapp_desc desc = {
        .title = "ScummVM",
        .app_id = "scummvm",
        .flags = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        .w = 640,
        .h = 480,
        .min_w = 400,
        .min_h = 300,
        .on_open = on_open,
        .on_draw = on_draw,
        .on_press = on_press,
        .on_motion = on_motion,
        .on_release = on_release,
        .on_key = on_key,
        .on_phys_key = on_phys_key,
        .on_user = on_user,
    };
    int rc = uapp_run(&desc);
    return g_launch ? launch() : rc;
}
