// See ubootmode.h.
#include "lib/ubootmode.h"
#include "lib/ubootmenu.h"
#include "lib/ubootcfg.h"
#include "lib/uconf.h"
#include "query_abi.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { int w, h; } SIZES[] = {
    { 3840, 2160 }, { 2560, 1600 }, { 2560, 1440 }, { 1920, 1200 }, { 1920, 1080 },
    { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 },  { 1440, 900 },  { 1366, 768 },
    { 1280, 1024 }, { 1280, 800 },  { 1280, 720 },  { 1024, 768 },  { 800, 600 },
    { 640, 480 },
};
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

int ubootmode_size_at(int i, int *w, int *h) {
    if (i < 0 || i >= NSIZES) return 0;
    *w = SIZES[i].w;
    *h = SIZES[i].h;
    return 1;
}

int ubootmode_parse(const char *s, int *w, int *h) {
    char *e;
    if (!s || *s < '1' || *s > '9') return 0;
    long pw = strtol(s, &e, 10);
    if (*e != 'x' || e[1] < '0' || e[1] > '9') return 0;
    long ph = strtol(e + 1, &e, 10);
    if (*e || pw < 640 || ph < 480 || pw > 3840 || ph > 2160) return 0;
    *w = (int)pw;
    *h = (int)ph;
    return 1;
}

int ubootmode_read(struct ubootmode *m) {
    memset(m, 0, sizeof *m);
    struct query_display d;
    memset(&d, 0, sizeof d);
    sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d);
    m->now_w = (int)d.width;
    m->now_h = (int)d.height;
    ubootmenu_env_get(UBOOTMODE_KEPT, m->kept, sizeof m->kept);
    ubootmenu_env_get(UBOOTMODE_TRY, m->pending, sizeof m->pending);
    ubootmenu_env_get(UBOOTMODE_TRIED, m->tried, sizeof m->tried);

    static struct ubootmenu menu;
    if (d.caps & QUERY_DISPLAY_CAP_MODESET)
        m->why_not = "this display sets its own size";
    else if (ubootmenu_read(&menu, 0) < 0)
        m->why_not = "there is no boot menu here to tell -- a live boot";
    else if (!menu.oneshot)
        m->why_not = menu.why_not;
    else
        m->usable = 1;
    return m->usable;
}

// The stanza grub.cfg needs before a variable means anything to GRUB.
static int ensure_stanza(const char **why) {
    static struct ubootcfg c;
    if (ubootcfg_load(&c, UBOOTMENU_CFG) < 0) { *why = "grub.cfg could not be read"; return -1; }
    if (!ubootcfg_video_missing(&c)) return 0;
    if (ubootcfg_add_video(&c) < 0) { *why = c.why; return -1; }
    return ubootcfg_save(&c, UBOOTMENU_CFG, why);
}

int ubootmode_try(const char *size, const char **why) {
    int w, h;
    *why = 0;
    if (size && !ubootmode_parse(size, &w, &h)) { *why = "not a screen size"; return -1; }
    char dev[64];
    if (uboot_writable(dev, sizeof dev) < 0) { *why = "cannot remount /boot read-write"; return -1; }
    int rc = ensure_stanza(why);
    if (rc == 0 && size)
        rc = ubootmenu_env_set(UBOOTMODE_TRY, size);
    else if (rc == 0)
        rc = ubootmenu_env_set(UBOOTMODE_TRY, 0) | ubootmenu_env_set(UBOOTMODE_KEPT, 0);
    if (rc < 0 && !*why) *why = "GRUB's environment block could not be written";
    uboot_restore(dev);
    return rc;
}

int ubootmode_cancel(void) { return ubootmenu_env_set(UBOOTMODE_TRY, 0); }

int ubootmode_keep(void) {
    char tried[UBOOTMODE_SIZE];
    if (!ubootmenu_env_get(UBOOTMODE_TRIED, tried, sizeof tried) || !tried[0]) return -1;
    char dev[64];
    if (uboot_writable(dev, sizeof dev) < 0) return -1;
    int rc = ubootmenu_env_set(UBOOTMODE_KEPT, tried) | ubootmenu_env_set(UBOOTMODE_TRIED, 0);
    uboot_restore(dev);
    return rc;
}

int ubootmode_drop(void) { return ubootmenu_env_set(UBOOTMODE_TRIED, 0); }

// The list is bounded by SIZES, so it fits a buffer sized from it.
#define REFUSED_CAP (NSIZES * UBOOTMODE_SIZE)

// QUERY_FIRMWARE; a kernel without the class booted the BIOS way.
static const char *refused_key(void) {
    struct query_firmware f;
    int uefi = sys_query_record(QUERY_FIRMWARE, 0, &f, sizeof f) == (int)sizeof f && f.uefi;
    return uefi ? UBOOTMODE_REFUSED_UEFI_KEY : UBOOTMODE_REFUSED_KEY;
}

int ubootmode_refused(const char *size) {
    char list[REFUSED_CAP];
    if (!uconf_get(UBOOTMODE_CONF, refused_key(), list, sizeof list)) return 0;
    int n = (int)strlen(size);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, ',');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len == n && !strncmp(p, size, (size_t)n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int ubootmode_refuse(const char *size) {
    int w, h;
    if (!ubootmode_parse(size, &w, &h)) return -1;
    if (ubootmode_refused(size)) return 0;
    char list[REFUSED_CAP], out[REFUSED_CAP + UBOOTMODE_SIZE];
    const char *key = refused_key();
    if (!uconf_get(UBOOTMODE_CONF, key, list, sizeof list)) list[0] = 0;
    snprintf(out, sizeof out, "%s%s%s", list, list[0] ? "," : "", size);
    return uconf_set(UBOOTMODE_CONF, key, out) ? 0 : -1;
}
