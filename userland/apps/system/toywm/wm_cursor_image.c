// wm_cursor_image.c -- the pointer images clients define themselves
// (WIN_REQ_CURSOR_IMAGE, abi/win_proto.h): copied out of the client's shm
// object at definition, kept per (pid, id) until the client goes, and
// drawn by wm_render.c when WIN_REQ_CURSOR names one.
//
// COPIED, NEVER MAPPED WHILE SHOWN: the client unlinks its object as
// soon as the reply says the pixels are here, and a client rewriting
// them meanwhile cannot change a shape the compositor is drawing.
#include "wm_internal.h"
#include "kapi.h"
#include "win_proto.h"
#include "rt/sys.h"
#include "wm_log.h"

#include <stdlib.h>

static struct wm_cursor_img *g_imgs;   // grown; a gone client's are dropped
static int g_count, g_cap;
static uint32_t g_serial;

const struct wm_cursor_img *wm_cursor_img_find(int pid, int id) {
    for (int i = 0; i < g_count; i++)
        if (g_imgs[i].pid == pid && g_imgs[i].id == id) return &g_imgs[i];
    return 0;
}

static void img_free(struct wm_cursor_img *c) {
    free(c->argb);
    free(c->inv);
}

int wm_cursor_img_define(int pid, int id) {
    if (id < 0 || id >= WIN_CURSOR_IMAGE_IDS) return -1;
    char nm[SHM_NAME_MAX];
    k_snprintf(nm, sizeof nm, WIN_CURSOR_NAME_FMT, pid, id);
    int fd = sys_shm_open(nm, 0, 0);
    if (fd < 0) return -1;
    // THE HEADER FIRST, THEN EXACTLY WHAT IT CLAIMS -- mapping the claimed
    // extent is the size check, as for a window buffer (map_buf()).
    struct win_cursor_image *hd = sys_mmap(0, 4096, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    if (hd == (void *)-1) { sys_close(fd); return -1; }
    struct win_cursor_image h = *hd;
    sys_munmap(hd, 4096);
    if (h.w < 1 || h.h < 1 || h.w > WIN_CURSOR_IMAGE_MAX || h.h > WIN_CURSOR_IMAGE_MAX ||
        h.hot_x >= h.w || h.hot_y >= h.h) {
        sys_close(fd);
        return -1;
    }
    uint64_t bytes = (WIN_CURSOR_IMAGE_BYTES(h.w, h.h) + 4095) & ~4095ULL;
    const uint8_t *p = sys_mmap(0, bytes, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return -1;
    int n = h.w * h.h;
    struct wm_cursor_img c = { .pid = pid, .id = id, .serial = ++g_serial,
                               .w = h.w, .h = h.h, .hot_x = h.hot_x, .hot_y = h.hot_y };
    c.argb = malloc((size_t)n * 4);
    c.inv = malloc((size_t)n);
    if (!c.argb || !c.inv) {
        img_free(&c);
        sys_munmap((void *)p, bytes);
        return -1;
    }
    k_memcpy(c.argb, p + sizeof h, (size_t)n * 4);
    const uint8_t *inv = p + sizeof h + (size_t)n * 4;
    for (int i = 0; i < n; i++) {
        c.inv[i] = inv[i] ? 1 : 0;
        c.inverts |= c.inv[i];
    }
    sys_munmap((void *)p, bytes);

    for (int i = 0; i < g_count; i++)
        if (g_imgs[i].pid == pid && g_imgs[i].id == id) {
            img_free(&g_imgs[i]);
            g_imgs[i] = c;
            return 0;
        }
    if (g_count == g_cap) {
        int cap = g_cap ? g_cap * 2 : 16;
        struct wm_cursor_img *g = realloc(g_imgs, sizeof *g * (size_t)cap);
        if (!g) { img_free(&c); return -1; }
        g_imgs = g;
        g_cap = cap;
    }
    g_imgs[g_count++] = c;
    return 0;
}

void wm_cursor_img_client_gone(int pid) {
    for (int i = g_count - 1; i >= 0; i--)
        if (g_imgs[i].pid == pid) {
            img_free(&g_imgs[i]);
            g_imgs[i] = g_imgs[--g_count];
        }
}
