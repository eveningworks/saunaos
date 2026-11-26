// A path past 255 bytes through the File Manager's widgets: uui_fileview
// descends to it a folder at a time and hands its paths out whole, Up
// climbs back whole, uui_pathbar shows (and takes) a deep folder's
// segments however many there are, and uui_places keeps one as a row.
// The widgets held 256-byte paths, and cut, a path names another file.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui/uui_fileview.h"
#include "ui/uui_pathbar.h"
#include "ui/uui_places.h"
#include "ui/uui_widget.h"
#include "rt/sys.h"
#include "uitest.h"

#define ROOT   "/home/dp"                                // this case's own (uitest_host.c)
#define PART   "deep_0123456789abcdefghijklmnopqrstuvw"   // 38 bytes, then "_N"
#define LEVELS 8

static struct uui_fileview fv;
static struct uui_pathbar pb;
static struct uui_places pl;
static char deep[1024], file[1100], name[64];

// The path bar's segment `k`, clicked: what it parks for the app.
static char *click_segment(int k) {
    int x, y, w, h;
    if (!uui_pathbar_segment_rect(&pb, k, &x, &y, &w, &h)) return 0;
    uui_pathbar_ops.press(&pb, x + w / 2, y + h / 2, 0);
    uui_pathbar_ops.release(&pb, x + w / 2, y + h / 2);
    return uui_pathbar_take(&pb);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("deeppath", "fileview, pathbar and places with a path past 255 bytes");

    strlcpy(deep, ROOT, sizeof deep);
    sys_mkdir(deep);
    for (int i = 0; i < LEVELS; i++) {
        size_t n = strlen(deep);
        snprintf(deep + n, sizeof deep - n, "/%s_%d", PART, i);
        sys_mkdir(deep);
    }
    snprintf(file, sizeof file, "%s/inside.txt", deep);
    utest_checkf(strlen(file) > 255 && uitest_file(file, "x"), "a %d-byte file to reach", (int)strlen(file));

    // --- the file view, down a folder at a time, as Enter does ------------
    uui_fileview_init(&fv, 0, 0, 600, 400);
    utest_check(uui_fileview_set_dir(&fv, ROOT), "the short folder lists");
    for (int i = 0; i < LEVELS; i++) {
        snprintf(name, sizeof name, "%s_%d", PART, i);
        if (!uui_fileview_select_name(&fv, name) || !uui_fileview_activate(&fv)) break;
    }
    utest_checkf(!strcmp(uui_fileview_dir(&fv), deep), "it descends to the %d-byte folder (%d)",
                 (int)strlen(deep), (int)strlen(uui_fileview_dir(&fv)));
    utest_check(uui_fileview_row_count(&fv) == 2, "...and lists it: \"..\" and the file");
    char *sel = uui_fileview_select_name(&fv, "inside.txt") ? uui_fileview_selected_path(&fv) : 0;
    utest_checkf(sel && !strcmp(sel, file), "the file's path is handed out whole (%d bytes)",
                 sel ? (int)strlen(sel) : -1);
    free(sel);
    uui_fileview_toggle_mark(&fv, uui_fileview_row_of(&fv, "inside.txt"));
    char *marked = uui_fileview_marked_path(&fv, 0);
    utest_check(marked && !strcmp(marked, file), "...and so is a mark's");
    free(marked);
    uui_fileview_reload(&fv);
    utest_check(uui_fileview_mark_count(&fv) == 1, "the mark survives a reload, by its whole path");

    char parent[1024];
    strlcpy(parent, deep, sizeof parent);
    *strrchr(parent, '/') = '\0';
    snprintf(name, sizeof name, "%s_%d", PART, LEVELS - 1);
    const char *was = uui_fileview_selected_name(&fv);
    utest_check(uui_fileview_up(&fv) && !strcmp(uui_fileview_dir(&fv), parent) &&
                (was = uui_fileview_selected_name(&fv)) && !strcmp(was, name),
                "Up climbs to the whole parent, the folder left selected");

    // --- the path bar --------------------------------------------------------
    uui_pathbar_init(&pb, "System", 0);
    uui_pathbar_ops.set_geometry(&pb, 0, 0, 6000, 28);
    uui_pathbar_set_path(&pb, deep);
    utest_check(!strcmp(uui_pathbar_path(&pb), deep), "the path bar holds the folder whole");
    int last = 1 + 2 + LEVELS - 1;   // the root, "home", "dp", then the levels
    char *to = click_segment(last);
    utest_checkf(to && !strcmp(to, deep), "its last segment takes you to the whole folder (%d)",
                 to ? (int)strlen(to) : -1);
    free(to);
    to = click_segment(last - 1);
    utest_check(to && !strcmp(to, parent), "...and the one before, to its whole parent");
    free(to);

    // Deeper than the bar has slots: the last folder is still the one shown.
    char many[256] = "";
    for (int i = 0; i < 40; i++) strlcat(many, "/a", sizeof many);
    uui_pathbar_set_path(&pb, many);
    int x, y, w, h;
    utest_check(uui_pathbar_segment_rect(&pb, 40, &x, &y, &w, &h) &&
                uui_pathbar_segment_rect(&pb, 0, &x, &y, &w, &h) &&
                uui_pathbar_segment_count(&pb) <= UUI_PATHBAR_SEGS,
                "40 folders deep, the root and the folder you are in are shown, folded between");
    uui_pathbar_free(&pb);

    // --- places ---------------------------------------------------------------
    uui_places_init(&pl);
    uui_places_add(&pl, "Deep", "folder", deep);
    uui_places_refresh(&pl);
    uui_places_select_path(&pl, deep);
    utest_check(pl.places == 1 && pl.selected == 0 && !strcmp(pl.row[0].path, deep),
                "a place keeps a deep folder whole, and is found by it");
    uui_places_free(&pl);

    uui_fileview_free(&fv);
    return uitest_end();
}
