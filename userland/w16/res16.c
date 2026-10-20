// res16.c -- a program's resources: KERNEL's FindResource / LoadResource /
// LockResource, and USER's LoadString on top of them.
//
// An HRSRC is the resource's index in the NE table (plus HRSRC_BASE, so 0
// stays "not found"); LoadResource copies its bytes into a global block,
// once -- a second load hands back the same block, as Windows' usage count
// did. The bytes are the FILE's, already checked against it by ne.c.
#include "w16_internal.h"

#include <string.h>

#define HRSRC_BASE 0x7000

// A resource name or type: an integer when the selector is 0 (or the
// string is "#123", which ne_resource_find() reads), else a string.
static int res_arg(struct w16 *w, uint32_t far, uint16_t *num, char *name, unsigned cap) {
    *num = 0;
    name[0] = 0;
    if ((far >> 16) == 0) { *num = (uint16_t)far; return 0; }
    return cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, name, cap) < 0 ? -1 : 0;
}

int w16_res_find(struct w16 *w, uint16_t type, const char *type_name, uint16_t id,
                 const char *name, struct ne_resource *out) {
    int i = ne_resource_find(&w->ne, type, type_name, id, name, out);
    return i < 0 ? -1 : i;
}

// The resource's bytes as a global block, made once.
uint16_t w16_res_load(struct w16 *w, unsigned index) {
    if (index >= W16_MAX_RES) return 0;
    if (w->res_sel[index]) return w->res_sel[index];
    struct ne_resource r;
    if (ne_resource_at(&w->ne, index, &r) < 0) return 0;
    uint32_t len = r.len > 0x10000 ? 0x10000 : r.len;
    w->res_sel[index] = w16_global_alloc(w, len, w->ne.img + r.off);
    return w->res_sel[index];
}

// String `id` from the RT_STRING tables into out (NUL-terminated); its
// length, or -1 when there is none.
int w16_load_string(struct w16 *w, uint16_t id, char *out, unsigned cap) {
    struct ne_resource r;
    if (cap == 0) return -1;
    out[0] = 0;
    if (ne_resource_find(&w->ne, NE_RT_STRING, 0, (uint16_t)(id / 16 + 1), 0, &r) < 0) return -1;
    const uint8_t *p = w->ne.img + r.off, *end = p + r.len;
    for (unsigned k = 0; k < (unsigned)(id % 16); k++) {
        if (p >= end) return -1;
        p += 1 + *p;
    }
    if (p >= end) return -1;
    unsigned n = *p++;
    if (n > (unsigned)(end - p)) return -1;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return (int)n;
}

// FindResource(hInst, lpName, lpType) -> HRSRC
// Windows: Finds a resource in a module's resource table -- a bitmap, an icon, a dialog template, a
// string table, a program's own data -- by its type and its name or number, and returns a handle to
// say which.
// w16: Looks it up in the program's NE resource table, by number or by name, standard types and the
// program's own alike, and returns a handle naming its place in that table; 0 when there is no such
// resource.
static int api_FindResource(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint32_t lpname = w16_arg32(a), lptype = w16_arg32(a);
    uint16_t id, type;
    char name[32], tname[32];
    if (res_arg(w, lpname, &id, name, sizeof name) < 0 ||
        res_arg(w, lptype, &type, tname, sizeof tname) < 0)
        return -1;
    struct ne_resource r;
    int i = w16_res_find(w, type, tname[0] ? tname : 0, id, name[0] ? name : 0, &r);
    c->r[R_AX] = i < 0 ? 0 : (uint16_t)(HRSRC_BASE + i);
    return 10;
}

// LoadResource(hInst, hRsrc) -> HGLOBAL
// Windows: Reads the resource FindResource found into memory and returns a global handle to it,
// loading it only once however often it is asked.
// w16: Copies the resource's bytes from the file into a moveable global block, once per resource,
// and returns its handle.
static int api_LoadResource(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = h >= HRSRC_BASE ? w16_res_load(w, h - HRSRC_BASE) : 0;
    return 4;
}

// LockResource(hResData) -> LPVOID
// Windows: Returns a far pointer to a loaded resource's bytes.
// w16: Returns the block's selector with offset 0: a global handle in w16 is its selector.
static int api_LockResource(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = 0;
    c->r[R_DX] = h;
    return 2;
}

// FreeResource(hResData) -> BOOL
// Windows: Releases a loaded resource when nothing else uses it.
// w16: Keeps the resource loaded for the next LoadResource, and answers that it was freed. A
// program has few resources, and a process's memory goes when it ends.
static int api_FreeResource(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// SizeofResource(hInst, hRsrc) -> DWORD
// Windows: The size of a resource in bytes -- rounded up to the module's alignment unit.
// w16: The size its resource table entry gives.
static int api_SizeofResource(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint16_t h = w16_arg16(a);
    struct ne_resource r;
    uint32_t n = h >= HRSRC_BASE && ne_resource_at(&w->ne, h - HRSRC_BASE, &r) == 0 ? r.len : 0;
    c->r[R_AX] = (uint16_t)n;
    c->r[R_DX] = (uint16_t)(n >> 16);
    return 4;
}

const struct w16_api g_kernel_res_api[] = {
    { 60, api_FindResource },
    { 61, api_LoadResource },
    { 62, api_LockResource },
    { 63, api_FreeResource },
    { 65, api_SizeofResource },
};
const unsigned g_kernel_res_api_n = sizeof g_kernel_res_api / sizeof g_kernel_res_api[0];

int w16_kernel_res(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_kernel_res_api, sizeof g_kernel_res_api / sizeof g_kernel_res_api[0], w, ordinal, a);
}

// USER.176 LoadString(hInst, id, lpBuffer, cbBuffer): the length copied.
int w16_user_loadstring(struct w16 *w, struct w16_args *a) {
    w16_arg16(a);
    uint16_t id = w16_arg16(a);
    uint32_t buf = w16_arg32(a);
    uint16_t cap = w16_arg16(a);
    char tmp[256];
    int n = w16_load_string(w, id, tmp, sizeof tmp);
    if (n < 0 || cap == 0) { w->cpu.r[R_AX] = 0; return 10; }
    if ((unsigned)n >= cap) n = cap - 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(buf >> 16), (uint16_t)buf, (uint32_t)n + 1, 1);
    if (!p) return -1;
    memcpy(p, tmp, (size_t)n);
    p[n] = 0;
    w->cpu.r[R_AX] = (uint16_t)n;
    return 10;
}
