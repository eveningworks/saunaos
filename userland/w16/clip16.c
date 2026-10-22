// clip16.c -- USER's clipboard, on toy-os's (lib/uclip).
//
// ONE CLIPBOARD, toy-os's: text a program puts there is what every toy-os
// app pastes, and toy-os's text and pictures are what the program pastes --
// converted at the edge, ANSI and "\r\n" here, UTF-8 and "\n" there; a
// picture as CF_BITMAP and CF_DIB. A program's OWN formats (Write's
// formatted text, an OLE object's Native data, CF_METAFILEPICT) live here
// in w16, for as long as the clipboard is still what the program last put
// on it: toy-os's serial says so. Another program -- each its own w16 --
// gets the text and the picture of them, as a Windows program that does
// not know a format does.
//
// The program's handles are the clipboard's once given (SetClipboardData),
// freed when it is emptied, and handed back as they are; what came from
// toy-os is made into a block once per clipboard and freed likewise.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/ucharset.h"
#include "lib/uclip.h"
#include "lib/uimg.h"

#define CF_TEXT         1
#define CF_BITMAP       2
#define CF_METAFILEPICT 3
#define CF_OEMTEXT      7
#define CF_DIB          8

struct fmt {
    uint16_t format;
    uint16_t handle;      // the program's block (a bitmap's HBITMAP), or the one made from toy-os's; 0: not yet
    int delayed;          // SetClipboardData(format, NULL): WM_RENDERFORMAT makes it when asked
    int ours;             // the handle came from toy-os's clipboard, not the program
};

static struct {
    struct fmt *v;
    int n, cap;
    uint16_t owner;       // EmptyClipboard's caller: the window that opened the clipboard
    uint16_t opener;      // OpenClipboard's window
    unsigned serial;      // toy-os's clipboard as the program last left it
    int set;              // the program has put anything since emptying
    unsigned seen;        // the serial the imported formats belong to
} g;

// --- text across the edge --------------------------------------------------------

int w16_clip_put_text(const char *ansi, int n) {
    char *lf = malloc((size_t)n + 1), *out = malloc((size_t)n * 3 + 1);
    int m = 0, ok = 0;
    for (int i = 0; lf && i < n; i++)
        if (!(ansi[i] == '\r' && i + 1 < n && ansi[i + 1] == '\n')) lf[m++] = ansi[i];   // toy-os's lines end in "\n"
    long u = lf && out ? ucharset_cp1252_to_utf8(out, (size_t)n * 3 + 1, lf, (size_t)m, '?') : -1;
    if (u >= 0) ok = uclip_set_text(out, (int)u);
    free(lf);
    free(out);
    return ok;
}

char *w16_clip_get_text(int *n) {
    static struct uclip c;                       // 64 KiB: not on the task's stack
    int len;
    if (!uclip_load(&c)) return 0;
    const char *s = uclip_text(&c, &len);
    if (!s) return 0;
    char *ansi = malloc((size_t)len + 1), *out = malloc((size_t)len * 2 + 1);
    long a = ansi && out ? ucharset_utf8_to_cp1252(ansi, (size_t)len + 1, s, (size_t)len, '?') : -1;
    if (a < 0) { free(ansi); free(out); return 0; }
    int m = 0;
    for (long i = 0; i < a; i++) {
        if (ansi[i] == '\n' && (i == 0 || ansi[i - 1] != '\r')) out[m++] = '\r';   // Windows' "\r\n"
        out[m++] = ansi[i];
    }
    out[m] = 0;
    free(ansi);
    *n = m;
    return out;
}

// --- the program's store -----------------------------------------------------------

static int current(void) { return g.set && uclip_peek_serial() == g.serial; }

static void free_handle(struct w16 *w, const struct fmt *f) {
    if (!f->handle) return;
    if (f->format == CF_BITMAP) { w16_gdi_delete(w, f->handle); return; }
    if (f->format == CF_METAFILEPICT && !f->ours) {      // the METAFILEPICT, then the metafile it names
        uint32_t size;
        const uint8_t *p = w16_global_ptr(w, f->handle, &size);
        if (p && size >= 8) w16_global_free(w, (uint16_t)(p[6] | p[7] << 8));
    }
    w16_global_free(w, f->handle);
}

static void empty(struct w16 *w) {
    for (int i = 0; i < g.n; i++) free_handle(w, &g.v[i]);
    g.n = 0;
    g.set = 0;
    g.owner = 0;
}

static struct fmt *find(uint16_t format) {
    for (int i = 0; i < g.n; i++)
        if (g.v[i].format == format) return &g.v[i];
    return 0;
}

static struct fmt *add(uint16_t format) {
    struct fmt *f = find(format);
    if (f) return f;
    if (g.n == g.cap) {
        int cap = g.cap ? 2 * g.cap : 8;
        struct fmt *v = realloc(g.v, sizeof *v * (size_t)cap);
        if (!v) return 0;
        g.v = v;
        g.cap = cap;
    }
    f = &g.v[g.n++];
    memset(f, 0, sizeof *f);
    f->format = format;
    return f;
}

// What toy-os's clipboard holds now, as formats: forgotten (and freed) when
// it has changed since they were made.
static void imported(struct w16 *w) {
    unsigned serial = uclip_peek_serial();
    if (g.set && serial == g.serial) return;     // the program's own
    if (g.seen == serial && !g.set) return;
    empty(w);
    g.seen = serial;
    static struct uclip c;
    if (!uclip_load(&c)) return;
    if (uclip_kind(&c) == UCLIP_KIND_TEXT) {
        struct fmt *f = add(CF_TEXT);
        if (f) f->ours = 1;
        if ((f = add(CF_OEMTEXT))) f->ours = 1;
    } else if (uclip_kind(&c) == UCLIP_KIND_IMAGE && uclip_image(&c)) {
        struct fmt *f = add(CF_BITMAP);
        if (f) f->ours = 1;
        if ((f = add(CF_DIB))) f->ours = 1;
    }
}

// The picture on toy-os's clipboard, decoded; 0 when it holds none.
static int clip_image(struct uimg *im) {
    static struct uclip c;
    if (!uclip_load(&c) || uclip_kind(&c) != UCLIP_KIND_IMAGE || !uclip_image(&c)) return 0;
    return uimg_load(uclip_image(&c), im) == 0;
}

// An imported format made into the block (or bitmap) a program reads.
static uint16_t make(struct w16 *w, uint16_t format) {
    if (format == CF_TEXT || format == CF_OEMTEXT) {
        int n;
        char *t = w16_clip_get_text(&n);
        uint16_t h = t && n >= 0 ? w16_global_alloc(w, (uint32_t)n + 1, t) : 0;   // over 64 KB: a huge block
        free(t);
        return h;
    }
    struct uimg im = { 0 };
    if (!clip_image(&im)) return 0;
    uint16_t h = 0;
    for (int i = 0; i < im.w * im.h; i++) im.px[i] &= 0xFFFFFF;
    if (format == CF_BITMAP) h = w16_gdi_bitmap_from(w, im.px, im.w, im.h);
    else if (format == CF_DIB) {                 // a packed DIB: header, then 24-bit rows bottom-up
        uint32_t stride = ((uint32_t)im.w * 3 + 3) & ~3u, size = 40 + stride * (uint32_t)im.h;
        if ((h = w16_global_alloc(w, size, 0))) {   // a huge block past 64 KB, as Windows gave one
            uint8_t *p = w16_global_ptr(w, h, 0);
            int32_t hdr[3] = { 40, im.w, im.h };
            memcpy(p, hdr, 12);
            p[12] = 1;                           // biPlanes
            p[14] = 24;                          // biBitCount
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) {
                    uint32_t px = im.px[(size_t)(im.h - 1 - y) * im.w + x];
                    uint8_t *q = p + 40 + (size_t)y * stride + (size_t)x * 3;
                    q[0] = (uint8_t)px; q[1] = (uint8_t)(px >> 8); q[2] = (uint8_t)(px >> 16);
                }
        }
    }
    uimg_free(&im);
    return h;
}

// A format the program put there, published to toy-os's clipboard when
// toy-os has a kind for it: text as text, a bitmap or DIB as a picture.
static void publish(struct w16 *w, const struct fmt *f) {
    uint32_t size;
    const uint8_t *p;
    if (f->format == CF_TEXT && (p = w16_global_ptr(w, f->handle, &size))) {
        uint32_t n = 0;
        while (n < size && p[n]) n++;
        w16_clip_put_text((const char *)p, (int)n);
    } else if (f->format == CF_BITMAP || f->format == CF_DIB || f->format == CF_METAFILEPICT) {
        const struct fmt *text = find(CF_TEXT);
        if (text && (text->handle || text->delayed)) return;   // toy-os keeps one kind: the text wins
        struct uimg im = { 0 };
        int bw = 0, bh = 0;
        uint16_t drawn = 0;
        if (f->format == CF_METAFILEPICT && (p = w16_global_ptr(w, f->handle, &size)) && size >= 8) {
            // a metafile drawn at its size, as a picture other programs can paste
            uint32_t ms;
            const uint8_t *m = w16_global_ptr(w, (uint16_t)(p[6] | p[7] << 8), &ms);
            int xe = (int16_t)(p[2] | p[3] << 8), ye = (int16_t)(p[4] | p[5] << 8);
            if (m) drawn = w16_render_metafile(w, m, ms, xe, ye);
        }
        const uint32_t *px = f->format == CF_BITMAP ? w16_gdi_bitmap_pixels(w, f->handle, &bw, &bh)
                           : drawn ? w16_gdi_bitmap_pixels(w, drawn, &bw, &bh) : 0;
        struct gdi_dib d = { 0 };
        if (f->format == CF_DIB && (p = w16_global_ptr(w, f->handle, &size)) && gdi_dib_header(p, size, 0, &d) == 0) {
            bw = d.w;
            bh = d.h < 0 ? -d.h : d.h;
            if ((uint64_t)d.header_len + (uint64_t)d.stride * (unsigned)bh > size) bw = 0;
        }
        if (bw <= 0 || bh <= 0 || !(im.px = malloc((size_t)bw * bh * 4))) {
            if (drawn) w16_gdi_delete(w, drawn);
            return;
        }
        im.w = bw;
        im.h = bh;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                uint32_t v;
                if (px) v = px[(size_t)y * bw + x];
                else v = gdi_dib_px(&d, p + d.header_len + (size_t)(d.h < 0 ? y : bh - 1 - y) * d.stride, x);
                im.px[(size_t)y * bw + x] = 0xFF000000u | (v & 0xFFFFFF);
            }
        uclip_set_image(&im);
        uimg_free(&im);
        if (drawn) w16_gdi_delete(w, drawn);
    } else {
        return;
    }
    g.serial = uclip_peek_serial();
}

// --- the exports ---------------------------------------------------------------------

// OpenClipboard(hWnd) -> BOOL
// CloseClipboard() -> BOOL
// Windows: Open the clipboard for the program to read or change, and close it.
// w16: Both succeed. The clipboard is toy-os's, shared with every program; closing it makes the
// text -- else the picture -- the program promised but has not made yet (SetClipboardData with no
// data), so the rest of toy-os can paste it.
static int api_OpenClipboard(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    if (ordinal == 137) {
        g.opener = w16_arg16(a);
    } else if (g_w16_trace) {
        fprintf(g_w16_trace, "  CloseClipboard: %s, owner %04x, %d formats\n", current() ? "ours" : "not ours", g.owner, g.n);
        fflush(g_w16_trace);
    }
    if (ordinal == 138 && current() && g.owner) {
        // What toy-os's clipboard can hold, made now: the text, else the picture.
        static const uint16_t KINDS[] = { CF_TEXT, CF_DIB, CF_BITMAP, CF_METAFILEPICT };
        for (unsigned k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
            struct fmt *f = find(KINDS[k]);
            if (!f) continue;
            if (f->delayed && !f->handle) w16_send(w, g.owner, WM_RENDERFORMAT, KINDS[k], 0);
            break;
        }
    }
    w->cpu.r[R_AX] = 1;
    return ordinal == 137 ? 2 : 0;
}

// EmptyClipboard() -> BOOL
// Windows: Empties the clipboard, freeing what it held, and makes the window that opened it the
// clipboard's owner -- the one asked to render a format it promised.
// w16: The same, toy-os's clipboard emptied too.
static int api_EmptyClipboard(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    empty(w);
    uclip_clear();
    g.set = 1;
    g.owner = g.opener && w16_wnd_get(w, g.opener) ? g.opener : W16_HWND;
    g.serial = uclip_peek_serial();
    w->cpu.r[R_AX] = 1;
    return 0;
}

// SetClipboardData(uFormat, hData) -> HANDLE
// Windows: Puts data on the clipboard in a format -- CF_TEXT, CF_BITMAP, CF_METAFILEPICT, CF_DIB,
// or one RegisterClipboardFormat named -- the block then the clipboard's. With no data, the
// owner is sent WM_RENDERFORMAT when a program asks for that format.
// w16: The same. Text, a bitmap, a DIB and a metafile (drawn as a picture) reach toy-os's
// clipboard, for every program; the other formats stay with this program.
// Why: Each Windows program is its own w16, and toy-os's clipboard holds files, text or one
// picture -- not a list of a Windows program's formats.
// Notice: Copying between two Windows programs carries the text and pictures only: Write's
// formatting, or an object, pastes as plain text in another copy of Write.
// Whole: The Windows formats kept on toy-os's clipboard beside its own kinds.
static int api_SetClipboardData(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t format = w16_arg16(a), h = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (!format) return 4;
    if (h) {
        w16_clip_set(w, format, h);
    } else {                                     // promised: made by WM_RENDERFORMAT when asked
        if (!current()) { empty(w); g.set = 1; g.serial = uclip_peek_serial(); }
        struct fmt *f = find(format);
        if (f && f->handle) free_handle(w, f);
        if (!f && !(f = add(format))) return 4;
        f->handle = 0;
        f->delayed = 1;
        f->ours = 0;
    }
    w->cpu.r[R_AX] = h;
    return 4;
}

// GetClipboardData(uFormat) -> HANDLE
// Windows: The clipboard's data in a format, the clipboard's own block -- the program reads it and
// leaves it be. A format promised but not yet made is asked of its owner first (WM_RENDERFORMAT).
// w16: The same: what the program put there, or what toy-os's clipboard holds, as CF_TEXT and
// CF_OEMTEXT for text and CF_BITMAP and CF_DIB for a picture.
static int api_GetClipboardData(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = w16_clip_get(w, w16_arg16(a));
    return 2;
}

// GetClipboardOwner() -> HWND
// Windows: The window that last emptied the clipboard, 0 when no window owns it.
// w16: The same while the clipboard holds what this program put there; 0 once another program --
// any toy-os app -- has copied since.
static int api_GetClipboardOwner(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = current() ? g.owner : 0;
    return 0;
}

// EnumClipboardFormats(format) -> UINT
// Windows: The clipboard's formats one by one: what follows `format` (0 for the first), 0 at the end.
// w16: The same, in the order they were put there; toy-os's text as CF_TEXT and CF_OEMTEXT, its
// picture as CF_BITMAP and CF_DIB.
static int api_EnumClipboardFormats(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t format = w16_arg16(a);
    imported(w);
    int i = 0;
    if (format) {
        while (i < g.n && g.v[i].format != format) i++;
        i++;
    }
    w->cpu.r[R_AX] = i < g.n ? g.v[i].format : 0;
    return 2;
}

// IsClipboardFormatAvailable(format) -> BOOL
// Windows: Whether the clipboard holds a format -- whether Paste should be enabled.
// w16: The same, over the formats EnumClipboardFormats lists.
static int api_IsClipboardFormatAvailable(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t format = w16_arg16(a);
    imported(w);
    w->cpu.r[R_AX] = find(format) != 0;
    return 2;
}

// RegisterClipboardFormat(lpszFormatName) -> UINT
// Windows: A format number (0xC000 and up) for a name, the same for every program asking by that
// name -- "Native", "OwnerLink", a program's own.
// w16: The same, from the table RegisterWindowMessage uses, as Windows' atoms were.
static int api_RegisterClipboardFormat(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    char name[128];
    w->cpu.r[R_AX] = 0;
    if (!(far >> 16) || cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, name, sizeof name) < 0) return 4;
    w->cpu.r[R_AX] = w16_atom_add(w, name);
    return 4;
}

void w16_clip_set(struct w16 *w, uint16_t format, uint16_t h) {
    if (!current()) { empty(w); g.set = 1; g.serial = uclip_peek_serial(); }
    struct fmt *f = find(format);
    if (f && f->handle && f->handle != h) free_handle(w, f);
    if (!f && !(f = add(format))) return;
    f->handle = h;
    f->delayed = 0;
    f->ours = 0;
    publish(w, f);
}

uint16_t w16_clip_get(struct w16 *w, uint16_t format) {
    imported(w);
    struct fmt *f = find(format);
    if (f && f->delayed && !f->handle && g.owner) {
        w16_send(w, g.owner, WM_RENDERFORMAT, format, 0);
        f = find(format);
    }
    if (f && !f->handle && f->ours) f->handle = make(w, format);
    return f ? f->handle : 0;
}

const struct w16_api g_clip_api[] = {
    { 137, api_OpenClipboard },
    { 138, api_OpenClipboard },
    { 139, api_EmptyClipboard },
    { 140, api_GetClipboardOwner },
    { 141, api_SetClipboardData },
    { 142, api_GetClipboardData },
    { 144, api_EnumClipboardFormats },
    { 145, api_RegisterClipboardFormat },
    { 193, api_IsClipboardFormatAvailable },
};
const unsigned g_clip_api_n = sizeof g_clip_api / sizeof g_clip_api[0];
