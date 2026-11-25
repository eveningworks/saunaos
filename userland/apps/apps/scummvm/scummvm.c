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
// at the next start (up to TRIES_MAX) rather than hidden after one failed
// add, which is how a game once stayed out of the list for ever. That
// runs a ScummVM per new game, seconds each, so it happens after the
// card's window has closed, never inside its event loop.
//
// A SCUMMVM ALREADY OPEN WRITES ITS OWN LIST BACK -- at a setting change
// and at quit -- and one built before our config patch
// (userland/third_party/scummvm/patches/0002) wrote the added game away.
// So a game added while another ScummVM runs is not marked offered: the
// next start finds it listed and marks it then, or finds it gone and adds
// it again.
#include "base/uconf.h"
#include "base/udatafetch.h"
#include "base/ulog.h"
#include "ui/uapp.h"
#include "ui/uui_datacard.h"
#include "uhttp.h"
#include "rt/sys.h"
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "base/udirs.h"

#define SCUMMVM    "/usr/bin/scummvm"
// The games' data name (base/udatafetch.h): a download goes to the person's
// $XDG_DATA_HOME/games, and a game shipped in /usr/share/games is found too.
#define GAMES_DATA "games"
#define GAMES_CONF "scummvm-games.conf"   // `mirror=`, a stand-in download server

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

static void mark_offered(const struct udata_item *g) {
    char key[48];
    snprintf(key, sizeof key, "added_%s", g->id);
    uconf_set(GAMES_CONF, key, "yes");
}

// A folder ScummVM does not take is asked again at the next start -- but
// not for ever: each ask runs a ScummVM (seconds), so after TRIES_MAX the
// game is marked offered and left alone. 0 when this was the last try.
#define TRIES_MAX 3
static int another_try(const struct udata_item *g) {
    char key[48], v[8];
    snprintf(key, sizeof key, "tries_%s", g->id);
    int n = uconf_get(GAMES_CONF, key, v, sizeof v) ? atoi(v) : 0;
    snprintf(v, sizeof v, "%d", ++n);
    uconf_set(GAMES_CONF, key, v);
    if (n < TRIES_MAX) return 1;
    mark_offered(g);
    return 0;
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
// what init gives the desktop's account, for a ScummVM started with no
// environment at all (the kernel shell, the debug console's `sh`).
static void make_xdg_dirs(void) {
    static const char *const VARS[] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME" };
    static const char *const UNDER[] = { ".config", ".local/share", ".cache" };
    const char *home = udir_home();
    for (int i = 0; i < 3; i++) {
        const char *now = getenv(VARS[i]);
        char def[300];
        snprintf(def, sizeof def, "%s/%s", strcmp(home, "/") ? home : "", UNDER[i]);
        if (!now || *now != '/') setenv(VARS[i], def, 1);
        const char *v = getenv(VARS[i]);
        char *p = v && *v == '/' ? strdup(v) : NULL;
        // Each '/' after the first ends a parent to make. Starting at
        // p + 1 itself turned "/home/.config" into "//ome/.config".
        for (char *s = p ? strchr(p + 1, '/') : NULL; s; s = strchr(s + 1, '/')) {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
        if (p) mkdir(p, 0755);
        free(p);
    }
}

// Another ScummVM process (not a thread, not this one): it holds its own
// copy of the list and writes it back.
static int other_scummvm_running(void) {
    struct proc_info p;
    int me = getpid();
    for (int i = 0; sys_proc_info(i, &p) == 0; i++)
        if (p.pid && p.pid == p.tgid && p.pid != me && !strcmp(p.name, "scummvm")) return 1;
    return 0;
}

// ONCE PER MACHINE: a game an open ScummVM wrote out of the list before
// the fix stayed marked offered, so it was never added again. Each game
// on disk, marked, and missing from the list is unmarked here -- which
// also brings back, once, one a person had removed.
#define HEAL_KEY "relisted"
static void heal_lost_games(void) {
    char v[8];
    if (uconf_get(GAMES_CONF, HEAL_KEY, v, sizeof v) && !strcmp(v, "yes")) return;
    for (int i = 0; i < GAME_COUNT; i++) {
        char *dir = udata_find(GAMES_DATA, &GAMES[i]), key[48];
        if (dir && offered(&GAMES[i]) && !registered(dir)) {
            snprintf(key, sizeof key, "added_%s", GAMES[i].id);
            uconf_set(GAMES_CONF, key, "no");
            snprintf(key, sizeof key, "tries_%s", GAMES[i].id);
            uconf_set(GAMES_CONF, key, "0");
            ulogf("scummvm: %s was marked added but is not in ScummVM's list; adding it again\n",
                  GAMES[i].id);
        }
        free(dir);
    }
    uconf_set(GAMES_CONF, HEAL_KEY, "yes");
}

// Every game here that ScummVM's list does not name yet, added by ScummVM
// -- once, unless the list itself is gone (a fresh /home).
static void register_games(void) {
    make_xdg_dirs();
    char *ini = ini_path();
    int have_ini = ini && access(ini, F_OK) == 0;
    free(ini);
    if (have_ini) heal_lost_games();
    int busy = other_scummvm_running();
    for (int i = 0; i < GAME_COUNT; i++) {
        char *dir = udata_find(GAMES_DATA, &GAMES[i]), *arg = NULL;
        if (!dir || (have_ini && offered(&GAMES[i]))) {
            free(dir);
            continue;
        }
        // Listed already (ScummVM's own Add Game, an add whose mark was
        // lost): marked here too, or removing it later would not stick.
        if (registered(dir)) {
            if (!busy) mark_offered(&GAMES[i]);
        } else if (asprintf(&arg, "--path=%s", dir) >= 0) {
            char *argv[] = { SCUMMVM, "--add", arg, NULL };
            int rc = run_wait(argv);
            if (registered(dir)) {
                if (!busy) mark_offered(&GAMES[i]);
                ulogf("scummvm: added %s to ScummVM's list (exit %d)%s\n", GAMES[i].id, rc,
                      busy ? "; another ScummVM is open, so it is checked again next start" : "");
            } else if (another_try(&GAMES[i])) {
                ulogf("scummvm: ScummVM did not list %s (exit %d); asked again next start\n",
                      GAMES[i].id, rc);
            } else {
                ulogf("scummvm: ScummVM did not list %s (exit %d) in %d tries; left alone\n",
                      GAMES[i].id, rc, TRIES_MAX);
            }
        }
        free(arg);
        free(dir);
    }
}

static int any_game(void) {
    for (int i = 0; i < GAME_COUNT; i++)
        if (udata_present(GAMES_DATA, &GAMES[i])) return 1;
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
    .log = "scummvm", .conf = GAMES_CONF, .name = GAMES_DATA,
    .items = GAMES, .count = GAME_COUNT, .multi = 1,
    .title_empty = "ScummVM needs a game", .title_some = "Free games for ScummVM",
    .subtitle = "Choose the games to download. They go to your own data folder.",
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
