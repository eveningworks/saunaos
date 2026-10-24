// ver16.c -- VER.DLL, the version-installation library: where a file is and
// where it should go. Control Panel's applets ask it at start-up which of
// their drivers are installed.
#include "w16_internal.h"

#include <stdio.h>
#include <string.h>

#define VFFF_ISSHAREDFILE  0x0001
#define VFF_CURNEDEST      0x0001
#define VFF_BUFFTOOSMALL   0x0004

// A string from the program, or "" for a NULL pointer; -1 when it does not fit or is outside its memory.
static int far_str(struct w16 *w, uint32_t far, char *out, size_t cap) {
    out[0] = 0;
    if (!(far >> 16) && !(uint16_t)far) return 0;
    return cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, out, cap) < 0 ? -1 : 0;
}

static int exists_in(const char *dir, const char *file) {
    char dos[160], host[512];
    snprintf(dos, sizeof dos, "%s\\%s", dir, file);
    return w16_dos_path(dos, host, sizeof host, 0, 0, 0) == 0;
}

// `dir` into the program's buffer of *lpuLen bytes, *lpuLen set to its length with the NUL; 0 when
// it fit, else VFF_BUFFTOOSMALL (and the buffer left as it was).
static unsigned put_dir(struct w16 *w, uint32_t buf, uint32_t lpu, const char *dir) {
    uint8_t *len = lpu ? cpu16_ptr(&w->cpu, (uint16_t)(lpu >> 16), (uint16_t)lpu, 2, 1) : 0;
    if (!len) return VFF_BUFFTOOSMALL;
    unsigned cap = len[0] | len[1] << 8, n = (unsigned)strlen(dir) + 1;
    len[0] = (uint8_t)n;
    len[1] = (uint8_t)(n >> 8);
    if (n > cap) return VFF_BUFFTOOSMALL;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(buf >> 16), (uint16_t)buf, n, 1);
    if (!p) return VFF_BUFFTOOSMALL;
    memcpy(p, dir, n);
    return 0;
}

// VerFindFile(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen) -> UINT
// Windows: Where a file about to be installed is now and where it should go: a shared file
// (VFFF_ISSHAREDFILE) belongs in the Windows SYSTEM directory, anything else in the program's
// own; szCurDir gets the directory holding a copy now ("" for none), szDestDir the one to put it
// in. VFF_CURNEDEST says those differ, VFF_BUFFTOOSMALL that a buffer was too short.
// w16: The same, looked for on drive C: -- C:\WINDOWS\SYSTEM, then C:\WINDOWS (or the szWinDir
// given), for a shared file; the program's directory for any other. No file is ever in use by
// another program, so VFF_FILEINUSE is never set.
static int api_VerFindFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    unsigned flags = w16_arg16(a);
    uint32_t file = w16_arg32(a), windir = w16_arg32(a), appdir = w16_arg32(a);
    uint32_t cur = w16_arg32(a), curlen = w16_arg32(a), dest = w16_arg32(a), destlen = w16_arg32(a);
    char name[80], win[128], app[128];
    if (far_str(w, file, name, sizeof name) < 0 || far_str(w, windir, win, sizeof win) < 0 ||
        far_str(w, appdir, app, sizeof app) < 0)
        return -1;
    if (!win[0]) snprintf(win, sizeof win, "C:\\WINDOWS");
    char sys[160];
    snprintf(sys, sizeof sys, "%s\\SYSTEM", win);
    const char *to = (flags & VFFF_ISSHAREDFILE) || !app[0] ? sys : app;
    const char *now = "";
    if (name[0]) {
        if (flags & VFFF_ISSHAREDFILE) now = exists_in(sys, name) ? sys : exists_in(win, name) ? win : "";
        else now = app[0] && exists_in(app, name) ? app : "";
    }
    unsigned r = put_dir(w, cur, curlen, now) | put_dir(w, dest, destlen, to);
    if (now[0] && strcasecmp(now, to)) r |= VFF_CURNEDEST;
    w->cpu.r[R_AX] = (uint16_t)r;
    return 30;
}

const struct w16_api g_ver_api[] = {
    { 8, api_VerFindFile },
};
const unsigned g_ver_api_n = sizeof g_ver_api / sizeof g_ver_api[0];
