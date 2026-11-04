// Disc images: an .iso opened here (a double-click, Enter, "Mount as a CD
// drive", the details pane's Mount) is MOUNTED as a CD drive and the pane
// goes to it -- Windows 8 Explorer's default verb for an image, and GNOME's
// Disk Image Mounter. One already mounted just opens. Eject unmounts it.
//
// AUTOMOUNTD DOES THE MOUNTING (lib/udrive.h): this window asks and goes
// on, and the answer comes back on a later tick through
// places_eject_tick(). An answer names no image -- a uchan reply is too
// small for a path -- so the asks wait here IN ORDER, and the service
// answers in the order it was asked.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include "lib/udrive.h"
#include "lib/uiso.h"
#include "lib/uvolinfo.h"
#include "ui/ulog.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// An ask not yet answered: the image (a mount) or the point (an eject),
// and the pane a mount opens in.
struct ask { int verb, pane; char path[PATH_MAX_LEN]; };
static struct ask *g_ask;
static int g_nask, g_cap;

static void remember(int verb, int pane, const char *path) {
    if (g_nask == g_cap) {
        int cap = g_cap ? g_cap * 2 : 4;
        struct ask *a = realloc(g_ask, (size_t)cap * sizeof *a);
        if (!a) return;   // the answer then comes back unnamed: said plainly below
        g_ask = a;
        g_cap = cap;
    }
    struct ask *a = &g_ask[g_nask++];
    a->verb = verb;
    a->pane = pane;
    strlcpy(a->path, path, sizeof a->path);
}

// The oldest ask of `verb`, taken off the list: 1, or 0 when none waits.
static int take(int verb, struct ask *out) {
    for (int i = 0; i < g_nask; i++) {
        if (g_ask[i].verb != verb) continue;
        *out = g_ask[i];
        memmove(&g_ask[i], &g_ask[i + 1], (size_t)(g_nask - i - 1) * sizeof *g_ask);
        g_nask--;
        return 1;
    }
    return 0;
}

int disc_is_image_name(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && dot != path && dot[-1] != '/' && !strcasecmp(dot, ".iso");
}

int disc_selected(char *path, int cap) {
    struct uui_fileview *fv = active();
    if (uui_fileview_mark_count(fv) > 1 || uui_fileview_selected_is_dir(fv) ||
        in_bin(fv) || in_archive(fv) || !uui_fileview_selected_path(fv, path, cap))
        return 0;
    return disc_is_image_name(path);
}

static void go(int pane, const char *point) {
    places_build();
    tree_rebuild();
    if (fm_goto(pane, point)) tree_reveal_path(point);
}

void disc_open(int pane, const char *path) {
    char point[UVOL_POINT];
    if (udrive_image_point(path, point, sizeof point)) {
        ulogf("files: disc %s open at %s\n", path, point);
        go(pane, point);
        return;
    }
    if (uiso_is_image(path) != 1) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s is not a disc image", k_path_basename(path));
        ulogf("files: disc %s refused: no image\n", path);
        return;
    }
    if (udrive_ask_mount_image(path) < 0) {
        set_note("could not mount it -- the drive service is not running");
        ulogf("files: disc %s refused: no service\n", path);
        return;
    }
    remember(UDRIVE_MOUNT_IMAGE, pane, path);
    snprintf(g_stat_note, sizeof g_stat_note, "mounting %s...", k_path_basename(path));
    ulogf("files: disc %s mount asked\n", path);
}

void disc_eject(const char *point) {
    fm_leave_volume(point);
    if (udrive_ask_eject_image(point) < 0) {
        set_note("could not eject it -- the drive service is not running");
        return;
    }
    remember(UDRIVE_EJECT_IMAGE, g_active, point);
    snprintf(g_stat_note, sizeof g_stat_note, "ejecting %s...", k_path_basename(point));
    ulogf("files: eject image %s\n", point);
}

int disc_reply(const struct udrive_msg *m) {
    if (m->verb != UDRIVE_MOUNT_IMAGE && m->verb != UDRIVE_EJECT_IMAGE) return 0;
    struct ask a;
    if (!take((int)m->verb, &a)) return 1;   // another window's ask, or forgotten for want of memory
    const char *name = k_path_basename(a.path);
    if (m->verb == UDRIVE_EJECT_IMAGE) {
        if (m->result == 0) snprintf(g_stat_note, sizeof g_stat_note, "%s ejected", name);
        else if (m->result == -EBUSY)
            snprintf(g_stat_note, sizeof g_stat_note, "%s was not ejected -- a file on it is still open", name);
        else snprintf(g_stat_note, sizeof g_stat_note, "%s was not ejected: %s", name, strerror(-m->result));
        ulogf("files: eject image %s rc %d\n", a.path, (int)m->result);
        places_build();
        tree_rebuild();
        return 1;
    }
    char point[UVOL_POINT] = "";
    if (m->result == 0 && udrive_image_point(a.path, point, sizeof point)) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s is mounted as a CD drive", name);
        go(a.pane, point);
    } else if (m->result == 0) {
        // Mounted, but its path is past what the mount table spells out
        // (QUERY_FSINFO's `source`): it is under This computer all the same.
        snprintf(g_stat_note, sizeof g_stat_note, "%s is mounted -- see This computer", name);
        places_build();
        tree_rebuild();
    } else if (m->result == -ENODEV) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s is not a disc image", name);
    } else {
        snprintf(g_stat_note, sizeof g_stat_note, "could not mount %s: %s", name, strerror(-m->result));
    }
    ulogf("files: disc %s mount rc %d at %s\n", a.path, (int)m->result, point[0] ? point : "-");
    return 1;
}
