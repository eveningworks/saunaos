// ver16.c -- VER.DLL, the version-installation library: where a file is and
// where it should go. Control Panel's applets ask it at start-up which of
// their drivers are installed.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/ufile.h"
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

// --- version information --------------------------------------------------------------
//
// A file's VERSIONINFO resource (RT_VERSION, 16) is a tree of blocks, each
// { WORD wLength; WORD wValueLength; char szKey[]; value; children }, the
// value and every child aligned to 4 bytes from the block's start -- Win16's
// form, ANSI keys and values. "\" is the root's VS_FIXEDFILEINFO,
// "\StringFileInfo\040904E4\ProductVersion" a string.

#define RT_VERSION 16

// The file's first RT_VERSION resource: 0 and the whole file in *img (the caller frees it), or -1.
static int version_resource(struct w16 *w, uint32_t file, unsigned char **img, struct ne_resource *r) {
    char dos[260], host[512];
    size_t n = 0;
    *img = 0;
    if (far_str(w, file, dos, sizeof dos) < 0 || w16_dos_path(dos, host, sizeof host, 0, 0, 0) != 0 ||
        ufile_slurp(host, 0, img, &n) != UFILE_OK)
        return -1;
    struct ne_module *m = malloc(sizeof *m);
    int found = 0;
    if (m && ne_parse(*img, (uint32_t)n, m) == 0)
        for (unsigned i = 0; !found && ne_resource_at(m, i, r) == 0; i++) found = r->type == RT_VERSION && r->len >= 4;
    free(m);
    if (!found) { free(*img); *img = 0; return -1; }
    return 0;
}

// GetFileVersionInfoSize(lpszFileName, lpdwHandle) -> DWORD
// Windows: How many bytes a file's version information takes -- the buffer GetFileVersionInfo
// needs; 0 when the file has none. *lpdwHandle is set to 0, a value only GetFileVersionInfo reads.
// w16: The same, from the file's RT_VERSION resource (ne.c's resource table).
static int api_GetFileVersionInfoSize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t file = w16_arg32(a), handle = w16_arg32(a);
    unsigned char *img;
    struct ne_resource r;
    uint32_t len = version_resource(w, file, &img, &r) == 0 ? r.len : 0;
    free(img);
    uint8_t *h = handle ? cpu16_ptr(&w->cpu, (uint16_t)(handle >> 16), (uint16_t)handle, 4, 1) : 0;
    if (h) memset(h, 0, 4);
    w->cpu.r[R_AX] = (uint16_t)len;
    w->cpu.r[R_DX] = (uint16_t)(len >> 16);
    return 8;
}

// GetFileVersionInfo(lpszFileName, dwHandle, dwLen, lpvData) -> BOOL
// Windows: A file's version information, the first dwLen bytes of it, into lpvData.
// w16: The same.
static int api_GetFileVersionInfo(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t file = w16_arg32(a);
    w16_arg32(a);
    uint32_t len = w16_arg32(a), data = w16_arg32(a);
    unsigned char *img;
    struct ne_resource r;
    int ok = 0;
    if (version_resource(w, file, &img, &r) == 0) {
        uint32_t n = len < r.len ? len : r.len;
        uint8_t *p = n ? cpu16_ptr(&w->cpu, (uint16_t)(data >> 16), (uint16_t)data, n, 1) : 0;
        if (p) { memcpy(p, img + r.off, n); ok = 1; }
        free(img);
    }
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 16;
}

static uint16_t rd16le(const uint8_t *b, uint32_t at) { return (uint16_t)(b[at] | b[at + 1] << 8); }
static uint32_t align4(uint32_t v) { return (v + 3) & ~3u; }

// The node at `at` (inside b[0..n)) reached by `path` ("A\B", "" for the node itself): its value's
// offset and length. 1, or 0 when a step is not there or a block runs outside the buffer.
static int vq_find(const uint8_t *b, uint32_t n, uint32_t at, const char *path, uint32_t *val, uint16_t *vlen) {
    if (at + 4 > n) return 0;
    uint32_t len = rd16le(b, at), end = at + len;
    if (len < 4 || end > n) return 0;
    uint32_t k = at + 4;
    while (k < end && b[k]) k++;
    if (k >= end) return 0;
    uint32_t v = align4(k + 1);
    uint16_t vl = rd16le(b, at + 2);
    while (*path == '\\') path++;
    if (!*path) {
        if (v + vl > end) return 0;
        *val = v;
        *vlen = vl;
        return 1;
    }
    size_t step = strcspn(path, "\\");
    for (uint32_t c = align4(v + vl); c + 4 <= end;) {
        uint32_t clen = rd16le(b, c);
        if (clen < 4) break;
        const char *key = (const char *)b + c + 4;
        const char *nul = memchr(key, 0, end - c - 4);
        if (nul && (size_t)(nul - key) == step && !strncasecmp(key, path, step))
            return vq_find(b, n, c, path + step, val, vlen);
        c = align4(c + clen);
    }
    return 0;
}

// VerQueryValue(lpvBlock, lpszSubBlock, lplpBuffer, lpcb) -> BOOL
// Windows: A value inside version information GetFileVersionInfo read: "\" the VS_FIXEDFILEINFO,
// "\VarFileInfo\Translation" the languages, "\StringFileInfo\<lang+charset>\<name>" a string.
// *lplpBuffer points INTO lpvBlock, *lpcb is the value's length. FALSE when it is not there.
// w16: The same, keys compared without case as Windows did.
static int api_VerQueryValue(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct cpu16 *c = &w->cpu;
    uint32_t block = w16_arg32(a), sub = w16_arg32(a), out = w16_arg32(a), lenp = w16_arg32(a);
    char path[128];
    w->cpu.r[R_AX] = 0;
    if (far_str(w, sub, path, sizeof path) < 0) return -1;
    const uint8_t *hd = cpu16_ptr(c, (uint16_t)(block >> 16), (uint16_t)block, 4, 0);
    if (!hd) return -1;
    uint16_t total = rd16le(hd, 0);
    const uint8_t *b = total >= 4 ? cpu16_ptr(c, (uint16_t)(block >> 16), (uint16_t)block, total, 0) : 0;
    if (!b) { c->fault = 0; return 16; }   // a block that says it runs past its memory: not found
    uint32_t val;
    uint16_t vl;
    if (!vq_find(b, total, 0, path, &val, &vl)) return 16;
    uint8_t *o = out ? cpu16_ptr(c, (uint16_t)(out >> 16), (uint16_t)out, 4, 1) : 0;
    uint8_t *l = lenp ? cpu16_ptr(c, (uint16_t)(lenp >> 16), (uint16_t)lenp, 2, 1) : 0;
    if ((out && !o) || (lenp && !l)) return -1;
    uint32_t far = block + val;                  // inside the block, so inside its segment
    if (o) { o[0] = (uint8_t)far; o[1] = (uint8_t)(far >> 8); o[2] = (uint8_t)(far >> 16); o[3] = (uint8_t)(far >> 24); }
    if (l) { l[0] = (uint8_t)vl; l[1] = (uint8_t)(vl >> 8); }
    w->cpu.r[R_AX] = 1;
    return 16;
}

const struct w16_api g_ver_api[] = {
    { 2, api_GetFileResourceSize },
    { 3, api_GetFileResource },
    { 6, api_GetFileVersionInfoSize },
    { 7, api_GetFileVersionInfo },
    { 8, api_VerFindFile },
    { 11, api_VerQueryValue },
};
const unsigned g_ver_api_n = sizeof g_ver_api / sizeof g_ver_api[0];
