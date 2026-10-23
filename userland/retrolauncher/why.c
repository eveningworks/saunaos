// why.c -- About, Why? and What? on a call: its section of the calls page
// or of the missing calls' page, in a window of its own (w16_call_doc()).
// The pages are generated from the comments above each handler and in
// missing16.c (tools/gen_w16_calls.py) and read whole in Help; a missing
// call with no section gets a card of what is known.
#include "retrolauncher/retrolauncher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ulog.h"
#include "ui/uui_docdialog.h"
#include "w16/w16prefs.h"

void retro_why_open(struct uapp *a, const char *module, unsigned ordinal, const char *name, const char *kind,
                    int status) {
    char key[48], title[96];
    snprintf(key, sizeof key, "%s.%u", module, ordinal);
    const char *shown = name && name[0] && strcmp(name, "?") ? name : key;
    snprintf(title, sizeof title, status == RETRO_CALL_PARTLY ? "%s - partly there"
             : status == RETRO_CALL_MISSING ? "%s - not there yet" : "%s - about", shown);
    int len = 0, from = 0;
    char *doc = w16_call_doc(key, name, kind, &len, &from);
    ulogf("retrolauncher: why %s %s\n", key, !doc ? "none" : from == W16_DOC_MADE ? "made" : "shown");
    if (doc) uui_docdialog_open(a, title, doc, len);
    free(doc);
}
