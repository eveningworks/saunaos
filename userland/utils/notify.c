// notify -- put up a notification from a shell or a script: freedesktop's
// notify-send in shape (an app name, an icon, a title and body, actions,
// an urgency), through the one API every program has (lib/unotify.h).
// With --wait it blocks for the answer and prints which action was taken,
// which is what makes it usable from a script.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lib/unotify.h"
#include "lib/uargs.h"

static const char *g_app = "Shell", *g_icon, *g_urgency = "normal", *g_actions, *g_replace, *g_close;
static int g_transient, g_wait;

static const struct uargs_opt OPTS[] = {
    { "app",       'a', "NAME",  "who it is from (default Shell): the name its setting has", 0, &g_app },
    { "icon",      'i', "ICON",  "an icon name, such as drive or tray-bell", 0, &g_icon },
    { "urgency",   'u', "LEVEL", "low, normal (the default) or critical", 0, &g_urgency },
    { "actions",   'A', "LIST",  "up to three button labels, comma-separated", 0, &g_actions },
    { "transient", 't', 0,       "not kept in the history", &g_transient, 0 },
    { "replace",   'r', "ID",    "update notification ID in place", 0, &g_replace },
    { "wait",      'w', 0,       "wait for the answer and print it", &g_wait, 0 },
    { "close",     'c', "ID",    "take notification ID down, and do nothing else", 0, &g_close },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "notify",
    .usage = "[OPTION]... TITLE [BODY]\n--close ID",
    .summary = "Show a notification: a card above the tray, kept in the bell's history.\n"
               "Prints its id.",
    .opts = OPTS,
    .notes = "With --wait, prints the label of the action taken, `clicked` for a click\n"
             "on the card or its history row, or `closed` when it was dismissed; the\n"
             "exit status is 0 for an action or a click, 1 for closed.\n"
             "A critical notification shows under Do Not Disturb and stays until\n"
             "answered. An app turned off in Settings > Desktop > Notifications shows\n"
             "nothing.",
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (g_close) {
        if (a.argc) return uargs_error(&PROG, "--close takes no TITLE");
        uint32_t id = (uint32_t)strtoul(g_close, 0, 10);
        if (!id || !unotify_close(id)) { fprintf(stderr, "notify: no desktop to ask\n"); return 1; }
        return 0;
    }
    if (a.argc < 1 || a.argc > 2) return uargs_error(&PROG, a.argc ? "too many arguments" : "a TITLE is needed");

    struct unotify n;
    memset(&n, 0, sizeof n);
    n.app = g_app;
    n.icon = g_icon;
    n.title = a.argv[0];
    n.body = a.argc > 1 ? a.argv[1] : 0;
    n.transient = g_transient != 0;
    if (!strcmp(g_urgency, "low")) n.urgency = WIN_NOTIFY_URGENCY_LOW;
    else if (!strcmp(g_urgency, "normal")) n.urgency = WIN_NOTIFY_URGENCY_NORMAL;
    else if (!strcmp(g_urgency, "critical")) n.urgency = WIN_NOTIFY_URGENCY_CRITICAL;
    else return uargs_error(&PROG, "--urgency is low, normal or critical, not '%s'", g_urgency);
    if (g_replace) n.replaces = (uint32_t)strtoul(g_replace, 0, 10);

    // The labels, split in place: at most WIN_NOTIFY_ACTIONS.
    static char labels[256];
    if (g_actions) {
        int commas = 0;
        for (const char *p = g_actions; *p; p++) commas += *p == ',';
        if (commas >= WIN_NOTIFY_ACTIONS) return uargs_error(&PROG, "at most %d actions", WIN_NOTIFY_ACTIONS);
        snprintf(labels, sizeof labels, "%s", g_actions);
        int k = 0;
        for (char *p = labels; p && *p && k < WIN_NOTIFY_ACTIONS; k++) {
            n.actions[k] = p;
            char *comma = strchr(p, ',');
            if (comma) *comma++ = 0;
            p = comma;
        }
    }

    uint32_t id = unotify_send(&n);
    if (!id) { fprintf(stderr, "notify: no desktop to show it on\n"); return 1; }
    printf("%u\n", id);
    if (!g_wait) return 0;
    fflush(stdout);
    int act;
    if (!unotify_wait(id, -1, &act)) { fprintf(stderr, "notify: the desktop went away\n"); return 1; }
    if (act == WIN_NOTIFY_CLICKED) { printf("clicked\n"); return 0; }
    if (act == WIN_NOTIFY_CLOSED) { printf("closed\n"); return 1; }
    printf("%s\n", act >= 0 && act < WIN_NOTIFY_ACTIONS && n.actions[act] ? n.actions[act] : "?");
    return 0;
}
