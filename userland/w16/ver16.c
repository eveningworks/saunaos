// ver16.c -- VER.DLL, the version-installation library: where a file is and
// where it should go. Control Panel's applets ask it at start-up which of
// their drivers are installed.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/ufile.h"
#include "ne.h"

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

// A resource type or ID as the program names it: a number (a far pointer whose segment is 0, or
// "#123"), else a name; 0, or -1 when the string is unreadable.
static int res_name(struct w16 *w, uint32_t far, uint16_t *num, char *name, size_t cap) {
    *num = 0;
    name[0] = 0;
    if (!(far >> 16)) { *num = (uint16_t)far; return 0; }
    if (far_str(w, far, name, cap) < 0) return -1;
    if (name[0] == '#') { *num = (uint16_t)strtoul(name + 1, 0, 10); name[0] = 0; }
    return 0;
}

// The resource `type`/`id` of the file the program names: its offset and length in the file, and
// the whole file in *img (the caller frees it). 0, or -1 for no file, no NE header or no resource.
static int file_resource(struct w16 *w, uint32_t file, uint32_t type, uint32_t id, unsigned char **img,
                         struct ne_resource *r) {
    char dos[260], host[512], tname[32], iname[32];
    uint16_t tn, in;
    size_t n = 0;
    *img = 0;
    if (far_str(w, file, dos, sizeof dos) < 0 || res_name(w, type, &tn, tname, sizeof tname) < 0 ||
        res_name(w, id, &in, iname, sizeof iname) < 0 || w16_dos_path(dos, host, sizeof host, 0, 0, 0) != 0 ||
        ufile_slurp(host, 0, img, &n) != UFILE_OK)
        return -1;
    struct ne_module *m = malloc(sizeof *m);        // ~7 KB: not on the stack
    int found = m && ne_parse(*img, (uint32_t)n, m) == 0 &&
                ne_resource_find(m, tn, tname[0] ? tname : 0, in, iname[0] ? iname : 0, r) >= 0;
    free(m);
    if (!found) {
        free(*img);
        *img = 0;
        return -1;
    }
    return 0;
}

// GetFileResourceSize(lpszFileName, lpszResType, lpszResID, lpdwFileOffset) -> DWORD
// Windows: How long one resource of a file is, without loading it as a module; its offset in the
// file goes to *lpdwFileOffset, for GetFileResource. 0 when the file or the resource is not there.
// w16: The same, from the file on drive C: (ne.c's resource table).
static int api_GetFileResourceSize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t file = w16_arg32(a), type = w16_arg32(a), id = w16_arg32(a), offp = w16_arg32(a);
    unsigned char *img;
    struct ne_resource r;
    uint32_t len = 0;
    if (file_resource(w, file, type, id, &img, &r) == 0) {
        len = r.len;
        uint8_t *p = offp ? cpu16_ptr(&w->cpu, (uint16_t)(offp >> 16), (uint16_t)offp, 4, 1) : 0;
        if (p) memcpy(p, &r.off, 4);
        free(img);
    }
    w->cpu.r[R_AX] = (uint16_t)len;
    w->cpu.r[R_DX] = (uint16_t)(len >> 16);
    return 16;
}

// GetFileResource(lpszFileName, lpszResType, lpszResID, dwFileOffset, dwResLen, lpvData) -> BOOL
// Windows: Reads one resource straight from a file, at most dwResLen bytes of it into lpvData:
// from dwFileOffset when GetFileResourceSize gave one, else found by type and ID. Control Panel's
// Fonts reads a .FOT's resource 204 this way -- the name of the TrueType file it stands for.
// w16: The same, from the file on drive C:.
static int api_GetFileResource(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t file = w16_arg32(a), type = w16_arg32(a), id = w16_arg32(a), off = w16_arg32(a);
    uint32_t len = w16_arg32(a), data = w16_arg32(a);
    unsigned char *img;
    struct ne_resource r;
    int ok = 0;
    if (file_resource(w, file, type, id, &img, &r) == 0) {
        // A given offset is GetFileResourceSize's for this resource; the one found is the same.
        uint32_t n = len < r.len ? len : r.len;
        uint8_t *p = n ? cpu16_ptr(&w->cpu, (uint16_t)(data >> 16), (uint16_t)data, n, 1) : 0;
        if (p && (!off || off == r.off)) {
            memcpy(p, img + r.off, n);
            ok = 1;
        }
        free(img);
    }
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 24;
}

const struct w16_api g_ver_api[] = {
    { 2, api_GetFileResourceSize },
    { 3, api_GetFileResource },
    { 8, api_VerFindFile },
};
const unsigned g_ver_api_n = sizeof g_ver_api / sizeof g_ver_api[0];
