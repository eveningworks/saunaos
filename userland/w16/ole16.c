// ole16.c -- OLECLI, OLE 1.0's client library: the objects a document
// holds (a Paintbrush picture in a Write letter), and SHELL's registration
// database, which says which programs serve them.
//
// AN OBJECT IS ITS BYTES AND ITS PICTURE. OleLoadFromStream reads it
// through the program's own OLESTREAM in OLE 1.0's persistent format
// ([MS-OLEDS] 2.2: an ObjectHeader, the native data, a presentation
// object); every byte read is kept and OleSaveToStream writes them back
// as they came, so a document saved keeps its objects whole. OleDraw
// shows the presentation: a metafile (meta16.c) or a DIB, stretched to the
// rectangle given. Nothing can EDIT an object -- there is no Paintbrush to
// serve it, and the registration database is empty, so Insert Object
// offers nothing and OleActivate says the server cannot start. A picture
// pasted in becomes a STATIC object, as 3.1 made one with no server.
//
// THE BYTES ARE THE DOCUMENT'S, untrusted like the program: every length
// is checked before what it counts is read, and capped.
//
// Everything answers at once (OLE_OK, never OLE_WAIT_FOR_RELEASE), so the
// client's CallBack is never needed.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OLE_OK               0
#define OLE_ERROR_MEMORY     4
#define OLE_ERROR_STREAM     5
#define OLE_ERROR_STATIC     6
#define OLE_ERROR_FORMAT     12
#define OLE_ERROR_OBJECT     13
#define OLE_ERROR_PROTOCOL   15
#define OLE_ERROR_GENERIC    19
#define OLE_ERROR_CLASS      20
#define OLE_ERROR_NOT_LINK   24
#define OLE_ERROR_NOT_OPEN   34
#define OLE_ERROR_LAUNCH     35

#define OT_LINK     1
#define OT_EMBEDDED 2
#define OT_STATIC   3

#define CF_BITMAP       2
#define CF_METAFILEPICT 3
#define CF_DIB          8

#define MAX_FIELD  (32u << 20)                   // a length past this is a broken or hostile document
#define MAX_OBJECT (64u << 20)
#define CHUNK      0x8000u                       // what one Get or Put moves

struct ole_obj {
    uint16_t sel;                 // the LPOLEOBJECT the program holds: sel:0000, a block of its own
    uint32_t doc;                 // its LHCLIENTDOC
    uint32_t client;              // the program's LPOLECLIENT
    long type;                    // OT_*
    char cls[64];                 // its class: "PBrush", or a static object's picture's
    uint8_t *raw;                 // the object as read or made: written back as it is
    uint32_t nraw;
    int pformat;                  // its picture: CF_METAFILEPICT or CF_DIB, 0 none
    uint32_t poff, plen;          // where the picture is in `raw`
    int32_t pw, ph;               // the picture's size, MM_HIMETRIC
};

static struct ole_obj **g_obj;
static int g_nobj;
static uint32_t g_next_doc = 1;

static struct ole_obj *object_of(uint32_t far) {
    if ((uint16_t)far) return 0;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i]->sel == (uint16_t)(far >> 16)) return g_obj[i];
    return 0;
}

static struct ole_obj *object_new(struct w16 *w) {
    struct ole_obj **g = realloc(g_obj, sizeof *g * (size_t)(g_nobj + 1));
    if (!g) return 0;
    g_obj = g;
    struct ole_obj *o = calloc(1, sizeof *o);
    if (!o) return 0;
    if (!(o->sel = w16_global_alloc(w, 16, 0))) { free(o); return 0; }
    g_obj[g_nobj++] = o;
    return o;
}

static void object_free(struct w16 *w, struct ole_obj *o) {
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i] == o) { g_obj[i] = g_obj[--g_nobj]; break; }
    w16_global_free(w, o->sel);
    free(o->raw);
    free(o);
}

// The far pointer `out` points at, set to `v`. 0 after a fault.
static int put_far(struct w16 *w, uint32_t out, uint32_t v) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(out >> 16), (uint16_t)out, 4, 1);
    if (!p) return 0;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i);
    return 1;
}

static uint32_t far_obj(const struct ole_obj *o) { return (uint32_t)o->sel << 16; }

// --- reading an object through the program's OLESTREAM ---------------------------------

struct rd {
    struct w16 *w;
    uint16_t fn_sel, fn_off;      // the stream's Get (or Put)
    uint32_t stream;
    uint16_t buf;                 // a global block the program's function fills
    uint8_t *raw;
    uint32_t n, cap;
};

// The stream's Get (which 0) or Put (which 1) function. 0 after a fault.
static int stream_fn(struct w16 *w, uint32_t stream, int which, uint16_t *sel, uint16_t *off) {
    const uint8_t *s = cpu16_ptr(&w->cpu, (uint16_t)(stream >> 16), (uint16_t)stream, 4, 0);
    if (!s) return 0;
    uint16_t vs = (uint16_t)(s[2] | s[3] << 8), vo = (uint16_t)(s[0] | s[1] << 8);
    const uint8_t *v = cpu16_ptr(&w->cpu, vs, (uint16_t)(vo + 4 * which), 4, 0);
    if (!v) return 0;
    *off = (uint16_t)(v[0] | v[1] << 8);
    *sel = (uint16_t)(v[2] | v[3] << 8);
    return 1;
}

static uint32_t call_stream(struct rd *r, uint32_t len) {
    const uint16_t words[] = { (uint16_t)(r->stream >> 16), (uint16_t)r->stream, r->buf, 0,
                               (uint16_t)(len >> 16), (uint16_t)len };
    return w16_call_far(r->w, r->fn_sel, r->fn_off, words, 6);
}

// `len` more bytes from the stream onto `raw`; 0 when the stream ends short.
static int pull(struct rd *r, uint32_t len) {
    if (len > MAX_FIELD || r->n + len > MAX_OBJECT) return 0;
    if (r->n + len > r->cap) {
        uint32_t cap = r->cap ? r->cap : 256;
        while (cap < r->n + len) cap *= 2;
        uint8_t *g = realloc(r->raw, cap);
        if (!g) return 0;
        r->raw = g;
        r->cap = cap;
    }
    while (len) {
        uint32_t k = len < CHUNK ? len : CHUNK;
        if (call_stream(r, k) != k || r->w->cpu.fault) return 0;
        memcpy(r->raw + r->n, w16_global_ptr(r->w, r->buf, 0), k);
        r->n += k;
        len -= k;
    }
    return 1;
}

static int u32(struct rd *r, uint32_t *v) {
    if (!pull(r, 4)) return 0;
    const uint8_t *p = r->raw + r->n - 4;
    *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return 1;
}

// A LengthPrefixedAnsiString: its length (the NUL counted), then the bytes.
static int lpstr(struct rd *r, char *out, size_t cap) {
    uint32_t len;
    if (!u32(r, &len) || len > 4096 || !pull(r, len)) return 0;
    size_t k = len && len - 1 < cap - 1 ? len - 1 : len ? cap - 1 : 0;
    if (out) {
        memcpy(out, r->raw + r->n - len, k);
        out[k] = 0;
    }
    return 1;
}

// A standard presentation's body after its class name: width, height, the
// picture's size and the picture.
static int picture_body(struct rd *r, struct ole_obj *o, const char *cls) {
    uint32_t pw, ph, size;
    if (!u32(r, &pw) || !u32(r, &ph) || !u32(r, &size)) return 0;
    uint32_t at = r->n;
    if (!pull(r, size)) return 0;
    o->pw = (int32_t)pw;
    o->ph = (int32_t)ph < 0 ? -(int32_t)ph : (int32_t)ph;   // stored times -1
    if (!strcmp(cls, "METAFILEPICT") && size >= 8) {
        o->pformat = CF_METAFILEPICT;
        o->poff = at + 8;                        // past the 8 reserved bytes (a METAFILEPICT16's)
        o->plen = size - 8;
    } else if (!strcmp(cls, "DIB")) {
        o->pformat = CF_DIB;
        o->poff = at;
        o->plen = size;
    } else if (!strcmp(cls, "BITMAP")) {        // a Bitmap16: the screen's own format
        o->pformat = CF_BITMAP;
        o->poff = at;
        o->plen = size;
    }
    return 1;
}

// [MS-OLEDS] 2.2.1-2.2.3: a presentation object, or its absence (FormatID 0).
static int presentation(struct rd *r, struct ole_obj *o) {
    uint32_t ver, fmt;
    char cls[64];
    if (!u32(r, &ver) || !u32(r, &fmt)) return 0;
    if (fmt == 0) return 1;
    if (fmt != 5 || !lpstr(r, cls, sizeof cls)) return 0;
    if (!strcmp(cls, "METAFILEPICT") || !strcmp(cls, "DIB") || !strcmp(cls, "BITMAP")) return picture_body(r, o, cls);
    uint32_t cf, size;                           // a clipboard format's: standard, or registered by name
    if (!u32(r, &cf)) return 0;
    if (cf == 0 && !lpstr(r, 0, 0)) return 0;
    if (!u32(r, &size)) return 0;
    uint32_t at = r->n;
    if (!pull(r, size)) return 0;
    if (cf == CF_DIB || cf == CF_METAFILEPICT) {
        o->pformat = (int)cf;
        o->poff = at;
        o->plen = size;
    }
    return 1;
}

static int dib_size(const uint8_t *d, uint32_t n, int *w, int *h);
#define HIMETRIC_OF(px) ((int32_t)((px) * 2540L / 96))   // at 3.1's 96 pixels to the inch

// [MS-OLEDS] 2.2.4-2.2.6: the object -- linked, embedded, or (OLE 1.0's
// FormatID 3) a static picture. 0 when it is malformed.
static int read_object(struct rd *r, struct ole_obj *o) {
    uint32_t ver, fmt, n;
    if (!u32(r, &ver) || !u32(r, &fmt)) return 0;
    if (fmt == OT_STATIC) {
        o->type = OT_STATIC;
        return lpstr(r, o->cls, sizeof o->cls) && picture_body(r, o, o->cls);
    }
    if (fmt != OT_LINK && fmt != OT_EMBEDDED) return 0;
    o->type = (long)fmt;
    if (!lpstr(r, o->cls, sizeof o->cls) || !lpstr(r, 0, 0) || !lpstr(r, 0, 0)) return 0;   // class, topic, item
    uint32_t native = 0;
    if (fmt == OT_EMBEDDED) {
        if (!u32(r, &n)) return 0;
        native = r->n;
        if (!pull(r, n)) return 0;               // the native data
    } else {
        uint32_t reserved, update;
        if (!lpstr(r, 0, 0) || !u32(r, &reserved) || !u32(r, &update)) return 0;   // the network name
    }
    if (!presentation(r, o)) return 0;
    // No picture saved with it (Paintbrush saves none): its native data is
    // a BMP file, whose DIB is the picture, as OLECLI drew it.
    if (!o->pformat && native && n > 14 && r->raw[native] == 'B' && r->raw[native + 1] == 'M') {
        int bw, bh;
        if (dib_size(r->raw + native + 14, n - 14, &bw, &bh)) {
            o->pformat = CF_DIB;
            o->poff = native + 14;
            o->plen = n - 14;
            o->pw = HIMETRIC_OF(bw);
            o->ph = HIMETRIC_OF(bh);
        }
    }
    return 1;
}

// --- making a static object from a picture ---------------------------------------------

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }

// A static object of `pic` (a metafile's records, or a packed DIB): its bytes
// in OLE 1.0's format, the picture `w` x `h` HIMETRIC.
static int make_static(struct ole_obj *o, int format, const uint8_t *pic, uint32_t len, int32_t w, int32_t h) {
    const char *cls = format == CF_METAFILEPICT ? "METAFILEPICT" : "DIB";
    uint32_t cl = (uint32_t)strlen(cls) + 1, extra = format == CF_METAFILEPICT ? 8 : 0;
    uint32_t n = 8 + 4 + cl + 12 + extra + len;
    uint8_t *raw = calloc(1, n);
    if (!raw) return 0;
    put32(raw, 0x0501);
    put32(raw + 4, OT_STATIC);
    put32(raw + 8, cl);
    memcpy(raw + 12, cls, cl);
    uint8_t *p = raw + 12 + cl;
    put32(p, (uint32_t)w);
    put32(p + 4, (uint32_t)-h);
    put32(p + 8, len + extra);
    if (extra) { p[12] = 8; p[14] = (uint8_t)w; p[15] = (uint8_t)(w >> 8); p[16] = (uint8_t)h; p[17] = (uint8_t)(h >> 8); }
    memcpy(p + 12 + extra, pic, len);
    free(o->raw);
    o->raw = raw;
    o->nraw = n;
    o->type = OT_STATIC;
    snprintf(o->cls, sizeof o->cls, "%s", cls);
    o->pformat = format;
    o->poff = n - len;
    o->plen = len;
    o->pw = w;
    o->ph = h;
    return 1;
}

// A DIB's size in pixels; 0 when it is no DIB.
static int dib_size(const uint8_t *d, uint32_t n, int *w, int *h) {
    struct gdi_dib g;
    if (gdi_dib_header(d, n, 0, &g) < 0) return 0;
    *w = g.w;
    *h = g.h < 0 ? -g.h : g.h;
    return 1;
}

// --- the exports: documents ----------------------------------------------------------------

// OleRegisterClientDoc(lpszClass, lpszDoc, reserved, lplhdoc) -> OLESTATUS
// OleRevokeClientDoc(lhClientDoc) -> OLESTATUS
// OleRenameClientDoc(lhClientDoc, lpszNewDocname) -> OLESTATUS
// OleRevertClientDoc(lhClientDoc) -> OLESTATUS
// OleSavedClientDoc(lhClientDoc) -> OLESTATUS
// Windows: Tell OLE a document that holds objects is open, closed, renamed, reverted to its saved
// copy, or saved -- so the servers editing its objects keep up; OleRegisterClientDoc gives the
// document's handle.
// w16: The same handle; the rest succeed. No server ever edits an object here, so there is no one
// to tell.
static int api_OleRegisterClientDoc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    w->cpu.r[R_AX] = OLE_OK;
    w->cpu.r[R_DX] = 0;
    switch (ordinal) {
    case 41: {
        w16_arg32(a); w16_arg32(a); w16_arg32(a);
        if (!put_far(w, w16_arg32(a), g_next_doc++)) return -1;
        return 16;
    }
    case 43: w16_arg32(a); w16_arg32(a); return 8;
    default: w16_arg32(a); return 4;
    }
}

// OleEnumObjects(lhDoc, lplpObject) -> OLESTATUS
// Windows: A document's objects one by one: given NULL, the first; given one, the next; NULL at the end.
// w16: The same.
static int api_OleEnumObjects(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t doc = w16_arg32(a), out = w16_arg32(a);
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(out >> 16), (uint16_t)out, 4, 0);
    if (!p) return -1;
    uint32_t cur = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    int i = 0;
    if (cur)
        while (i < g_nobj && far_obj(g_obj[i]) != cur) i++;
    if (cur) i++;
    while (i < g_nobj && g_obj[i]->doc != doc) i++;
    if (!put_far(w, out, i < g_nobj ? far_obj(g_obj[i]) : 0)) return -1;
    w->cpu.r[R_AX] = OLE_OK;
    return 8;
}

// --- objects: made, kept, freed -------------------------------------------------------------

// OleLoadFromStream(lpstream, lpszProtocol, lpclient, lhClientDoc, lpszObjname, lplpobj) -> OLESTATUS
// Windows: Reads an object out of the document through the program's stream functions -- its
// class, its native data, its picture -- and makes it.
// w16: The same, in OLE 1.0's persistent format: linked, embedded and static objects, every byte
// kept so the object is saved back unchanged; its picture (a metafile or a DIB) is what OleDraw
// shows.
static int api_OleLoadFromStream(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t stream = w16_arg32(a);
    w16_arg32(a);
    uint32_t client = w16_arg32(a), doc = w16_arg32(a);
    w16_arg32(a);
    uint32_t out = w16_arg32(a);
    struct rd r = { .w = w, .stream = stream };
    w->cpu.r[R_AX] = OLE_ERROR_STREAM;
    w->cpu.r[R_DX] = 0;
    if (!stream_fn(w, stream, 0, &r.fn_sel, &r.fn_off)) return -1;
    struct ole_obj *o = object_new(w);
    if (!o || !(r.buf = w16_global_alloc(w, CHUNK, 0))) {
        if (o) object_free(w, o);
        w->cpu.r[R_AX] = OLE_ERROR_MEMORY;
        return 24;
    }
    int ok = read_object(&r, o);
    w16_global_free(w, r.buf);
    if (w->cpu.fault) { free(r.raw); return -1; }
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  OleLoadFromStream: %s, class %s, %u bytes, picture %d %ux%u\n", ok ? "read" : "MALFORMED",
                o->cls, (unsigned)r.n, o->pformat, (unsigned)o->pw, (unsigned)o->ph);
        fflush(g_w16_trace);
    }
    if (!ok) { free(r.raw); object_free(w, o); return 24; }
    o->raw = r.raw;
    o->nraw = r.n;
    o->doc = doc;
    o->client = client;
    if (!put_far(w, out, far_obj(o))) return -1;
    w->cpu.r[R_AX] = OLE_OK;
    return 24;
}

// OleSaveToStream(lpObject, lpstream) -> OLESTATUS
// Windows: Writes an object into the document through the program's stream functions.
// w16: The same bytes it was read as -- or, for a picture pasted in, made as -- so a document keeps
// its objects whole.
static int api_OleSaveToStream(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t stream = w16_arg32(a);
    w->cpu.r[R_AX] = OLE_ERROR_OBJECT;
    w->cpu.r[R_DX] = 0;
    if (!o) return 8;
    struct rd r = { .w = w, .stream = stream };
    if (!stream_fn(w, stream, 1, &r.fn_sel, &r.fn_off)) return -1;
    if (!(r.buf = w16_global_alloc(w, CHUNK, 0))) { w->cpu.r[R_AX] = OLE_ERROR_MEMORY; return 8; }
    int ok = 1;
    for (uint32_t at = 0; ok && at < o->nraw; ) {
        uint32_t k = o->nraw - at < CHUNK ? o->nraw - at : CHUNK;
        memcpy(w16_global_ptr(w, r.buf, 0), o->raw + at, k);
        ok = call_stream(&r, k) == k && !w->cpu.fault;
        at += k;
    }
    w16_global_free(w, r.buf);
    if (w->cpu.fault) return -1;
    w->cpu.r[R_AX] = ok ? OLE_OK : OLE_ERROR_STREAM;
    return 8;
}

// OleQuerySize(lpObject, lpdwSize) -> OLESTATUS
// Windows: How many bytes OleSaveToStream will write for an object.
// w16: The same.
static int api_OleQuerySize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t out = w16_arg32(a);
    w->cpu.r[R_AX] = o ? OLE_OK : OLE_ERROR_OBJECT;
    if (o && !put_far(w, out, o->nraw)) return -1;
    return 8;
}

// OleQueryType(lpObject, lpType) -> OLESTATUS
// Windows: Whether an object is linked, embedded or static.
// w16: The same.
static int api_OleQueryType(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t out = w16_arg32(a);
    w->cpu.r[R_AX] = o ? OLE_OK : OLE_ERROR_OBJECT;
    if (o && !put_far(w, out, (uint32_t)o->type)) return -1;
    return 8;
}

// OleClone(lpObject, lpClient, lhClientDoc, lpszObjname, lplpObject) -> OLESTATUS
// Windows: A copy of an object, in a document.
// w16: The same.
static int api_OleClone(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t client = w16_arg32(a), doc = w16_arg32(a);
    w16_arg32(a);
    uint32_t out = w16_arg32(a);
    w->cpu.r[R_AX] = OLE_ERROR_OBJECT;
    if (!o) return 20;
    struct ole_obj *c = object_new(w);
    uint8_t *raw = c ? malloc(o->nraw ? o->nraw : 1) : 0;
    if (!c || !raw) { if (c) object_free(w, c); free(raw); w->cpu.r[R_AX] = OLE_ERROR_MEMORY; return 20; }
    uint16_t sel = c->sel;
    *c = *o;
    c->sel = sel;
    c->raw = raw;
    memcpy(raw, o->raw, o->nraw);
    c->client = client;
    c->doc = doc;
    if (!put_far(w, out, far_obj(c))) return -1;
    w->cpu.r[R_AX] = OLE_OK;
    return 20;
}

// OleObjectConvert(lpObject, lpszProtocol, lpClient, lhClientDoc, lpszObjname, lplpObject) -> OLESTATUS
// Windows: A new object made from one -- in 3.1, a static picture of a linked or embedded one.
// w16: The same: "Static" makes a static object of the object's picture; any other protocol is
// refused (OLE_ERROR_PROTOCOL).
static int api_OleObjectConvert(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t proto = w16_arg32(a), client = w16_arg32(a), doc = w16_arg32(a);
    w16_arg32(a);
    uint32_t out = w16_arg32(a);
    char p[32] = "";
    if ((proto >> 16) && cpu16_str(&w->cpu, (uint16_t)(proto >> 16), (uint16_t)proto, p, sizeof p) < 0) return -1;
    w->cpu.r[R_AX] = !o ? OLE_ERROR_OBJECT : strcmp(p, "Static") ? OLE_ERROR_PROTOCOL : OLE_ERROR_STATIC;
    if (!o || strcmp(p, "Static") || !o->pformat) return 24;
    struct ole_obj *c = object_new(w);
    if (!c || !make_static(c, o->pformat, o->raw + o->poff, o->plen, o->pw, o->ph)) {
        if (c) object_free(w, c);
        w->cpu.r[R_AX] = OLE_ERROR_MEMORY;
        return 24;
    }
    c->client = client;
    c->doc = doc;
    if (!put_far(w, out, far_obj(c))) return -1;
    w->cpu.r[R_AX] = OLE_OK;
    return 24;
}

// OleDelete(lpObject) -> OLESTATUS
// OleRelease(lpObject) -> OLESTATUS
// Windows: Free an object -- OleDelete also takes it out of the document for good, closing any
// server editing it.
// w16: Both free it; there is never a server to close.
static int api_OleDelete(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    if (o) object_free(w, o);
    w->cpu.r[R_AX] = o ? OLE_OK : OLE_ERROR_OBJECT;
    w->cpu.r[R_DX] = 0;
    return 4;
}

// --- objects: shown -------------------------------------------------------------------------

// The picture into a DC's rectangle: a metafile through MM_ANISOTROPIC,
// its own extents (or the object's size) onto the rectangle; a DIB stretched.
static int draw(struct w16 *w, struct ole_obj *o, uint16_t hdc, const int16_t rc[4]) {
    if (!o->pformat) return 0;
    if (o->pformat == CF_DIB || o->pformat == CF_BITMAP) {
        int bw, bh;
        uint16_t bm = o->pformat == CF_BITMAP ? w16_gdi_bitmap_of_ddb(w, o->raw + o->poff, o->plen)
                    : dib_size(o->raw + o->poff, o->plen, &bw, &bh) ? w16_gdi_bitmap_of_dib(w, o->raw + o->poff, o->plen) : 0;
        if (!bm) return 0;
        w16_gdi_stretch_bitmap(w, hdc, rc[0], rc[1], rc[2] - rc[0], rc[3] - rc[1], bm);
        w16_gdi_delete(w, bm);
        return 1;
    }
    return w16_gdi_play_into(w, hdc, rc, o->pw, o->ph, o->raw + o->poff, o->plen);
}

// OleDraw(lpObject, hdc, lprcBounds, lprcWBounds, hdcFormat) -> OLESTATUS
// Windows: Draws an object's picture into a rectangle of a DC, scaled to fit it.
// w16: The same, from the picture stored with the object: a metafile (as PlayMetaFile draws it), a
// DIB, or a device bitmap (monochrome, the VGA's 16 colours, 24-bit), stretched to the rectangle.
static int api_OleDraw(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint16_t hdc = w16_arg16(a);
    uint32_t bounds = w16_arg32(a);
    w16_arg32(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = OLE_ERROR_OBJECT;
    w->cpu.r[R_DX] = 0;
    if (!o) return 16;
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(bounds >> 16), (uint16_t)bounds, 8, 0);
    if (!p) return -1;
    int16_t rc[4];
    for (int i = 0; i < 4; i++) rc[i] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
    uint16_t ax = 0;
    int shown = draw(w, o, hdc, rc);
    if (w->cpu.fault) return -1;
    ax = shown ? OLE_OK : 8;                     // OLE_ERROR_DRAW
    w->cpu.r[R_AX] = ax;
    w->cpu.r[R_DX] = 0;
    return 16;
}

// OleQueryBounds(lpObject, lpBounds) -> OLESTATUS
// Windows: An object's size, as a rectangle from (0, 0) in MM_HIMETRIC units.
// w16: The same, from its picture.
static int api_OleQueryBounds(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t out = w16_arg32(a);
    w->cpu.r[R_AX] = OLE_ERROR_OBJECT;
    if (!o) return 8;
    int32_t pw = o->pw, ph = o->ph;
    int bw, bh;
    if (o->pformat == CF_DIB && (!pw || !ph) && dib_size(o->raw + o->poff, o->plen, &bw, &bh)) {
        pw = HIMETRIC_OF(bw);
        ph = HIMETRIC_OF(bh);
    }
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(out >> 16), (uint16_t)out, 8, 1);
    if (!p) return -1;
    int16_t rc[4] = { 0, 0, (int16_t)pw, (int16_t)ph };
    memcpy(p, rc, 8);
    w->cpu.r[R_AX] = OLE_OK;
    return 8;
}

// --- objects: what cannot be done without a server -----------------------------------------

// No program here can edit an object: the server is reported as not starting.
// OleActivate(lpObject, verb, fShow, fTakeFocus, hwnd, lpBounds) -> OLESTATUS
// Windows: Opens an object in the program that made it -- Paintbrush for a picture -- to edit or
// play it.
// w16: Answers that the server could not be started (OLE_ERROR_LAUNCH).
// Why: Each Windows program is its own w16 and none can serve another's objects yet; and the user's
// Paintbrush is not run on its behalf.
// Notice: Double-clicking an object, or Edit > Object, shows that the program to edit it cannot be
// started; the object stays as it is.
// Whole: One Windows program starting another and the two talking (DDE, which OLE 1.0 runs on).
// On the roadmap: "w16: one program starting or finding another".
static int api_OleActivate(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    w16_arg16(a); w16_arg16(a); w16_arg16(a); w16_arg16(a); w16_arg32(a);
    w->cpu.r[R_AX] = !o ? OLE_ERROR_OBJECT : o->type == OT_STATIC ? OLE_ERROR_STATIC : OLE_ERROR_LAUNCH;
    w->cpu.r[R_DX] = 0;
    return 16;
}

// OleQueryOpen(lpObject) -> OLESTATUS
// OleClose(lpObject) -> OLESTATUS
// OleUpdate(lpObject) -> OLESTATUS
// Windows: Whether an object's server has it open; close that; bring a linked object's picture up
// to date from its source.
// w16: No server ever has one open (OLE_ERROR_NOT_OPEN); closing and updating succeed with nothing
// to do -- a linked object keeps the picture it was saved with.
static int api_OleQueryOpen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct ole_obj *o = object_of(w16_arg32(a));
    w->cpu.r[R_AX] = !o ? OLE_ERROR_OBJECT : ordinal == 20 ? OLE_ERROR_NOT_OPEN : OLE_OK;
    w->cpu.r[R_DX] = 0;
    return 4;
}

// OleSetHostNames(lpObject, lpszClient, lpszClientObj) -> OLESTATUS
// OleSetTargetDevice(lpObject, hotd) -> OLESTATUS
// Windows: Tell an object's server the document's and the object's names, for its title bar, and
// the printer the picture is for.
// w16: Both succeed: there is no server to tell.
static int api_OleSetHostNames(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct ole_obj *o = object_of(w16_arg32(a));
    if (ordinal == 15) { w16_arg32(a); w16_arg32(a); } else w16_arg16(a);
    w->cpu.r[R_AX] = o ? OLE_OK : OLE_ERROR_OBJECT;
    w->cpu.r[R_DX] = 0;
    return ordinal == 15 ? 12 : 6;
}

// OleGetLinkUpdateOptions(lpObject, lpUpdateOpt) -> OLESTATUS
// OleSetLinkUpdateOptions(lpObject, UpdateOpt) -> OLESTATUS
// Windows: Whether a linked object's picture follows its source automatically or only when asked.
// w16: A linked object says it updates when asked (oleupdate_onrequest), and keeps a new choice;
// any other object is not a link (OLE_ERROR_NOT_LINK).
static int api_OleGetLinkUpdateOptions(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct ole_obj *o = object_of(w16_arg32(a));
    uint32_t out = ordinal == 24 ? w16_arg32(a) : 0;
    if (ordinal == 25) w16_arg16(a);
    w->cpu.r[R_AX] = !o ? OLE_ERROR_OBJECT : o->type != OT_LINK ? OLE_ERROR_NOT_LINK : OLE_OK;
    w->cpu.r[R_DX] = 0;
    if (o && o->type == OT_LINK && ordinal == 24) {
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(out >> 16), (uint16_t)out, 2, 1);
        if (!p) return -1;
        p[0] = 2;                                // oleupdate_onrequest
        p[1] = 0;
    }
    return ordinal == 24 ? 8 : 6;
}

// The picture as a handle of `format` a program can read: a METAFILEPICT
// block and its metafile, or a packed DIB. 0 when it has none such.
static uint16_t picture_handle(struct w16 *w, struct ole_obj *o, unsigned format) {
    if (format == CF_BITMAP && o->pformat == CF_BITMAP) return w16_gdi_bitmap_of_ddb(w, o->raw + o->poff, o->plen);
    if (format != (unsigned)o->pformat || o->plen > 0xFFFF) return 0;
    uint16_t data = w16_global_alloc(w, o->plen, o->raw + o->poff);
    if (!data || format == CF_DIB) return data;
    uint8_t mfp[8] = { 8, 0, (uint8_t)o->pw, (uint8_t)(o->pw >> 8), (uint8_t)o->ph, (uint8_t)(o->ph >> 8),
                       (uint8_t)data, (uint8_t)(data >> 8) };    // MM_ANISOTROPIC, its size, hMF
    uint16_t h = w16_global_alloc(w, sizeof mfp, mfp);
    if (!h) w16_global_free(w, data);
    return h;
}

// OleEnumFormats(lpObject, cfFormat) -> OLECLIPFORMAT
// OleGetData(lpObject, cfFormat, lphData) -> OLESTATUS
// OleSetData(lpObject, cfFormat, hData) -> OLESTATUS
// Windows: The formats an object's data comes in, one after another; the data in one of them; and
// new data for it in one, handed to its server.
// w16: The object's picture is its one format -- CF_METAFILEPICT, CF_DIB or CF_BITMAP -- and
// OleGetData makes a copy of it the program owns. OleSetData is refused (OLE_ERROR_GENERIC): there is no server to
// hand data to.
static int api_OleEnumFormats(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct ole_obj *o = object_of(w16_arg32(a));
    uint16_t format = w16_arg16(a);
    w->cpu.r[R_DX] = 0;
    if (ordinal == 26) {
        w->cpu.r[R_AX] = o && !format ? (uint16_t)o->pformat : 0;
        return 6;
    }
    uint32_t out = ordinal == 28 ? w16_arg32(a) : 0;
    if (ordinal == 29) {
        w16_arg16(a);
        w->cpu.r[R_AX] = o ? OLE_ERROR_GENERIC : OLE_ERROR_OBJECT;
        return 8;
    }
    w->cpu.r[R_AX] = OLE_ERROR_OBJECT;
    if (!o) return 10;
    uint16_t h = picture_handle(w, o, format);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(out >> 16), (uint16_t)out, 2, 1);
    if (!p) return -1;
    p[0] = (uint8_t)h;
    p[1] = (uint8_t)(h >> 8);
    w->cpu.r[R_AX] = h ? OLE_OK : OLE_ERROR_FORMAT;
    return 10;
}

// OleCopyToClipboard(lpObject) -> OLESTATUS
// Windows: Puts an object on the open clipboard -- its native data, its link and its picture -- for
// pasting into another document.
// w16: Its picture, as CF_METAFILEPICT, CF_DIB or CF_BITMAP: what any program can paste, and what
// Write pastes back as a static picture.
static int api_OleCopyToClipboard(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    w->cpu.r[R_AX] = OLE_ERROR_OBJECT;
    w->cpu.r[R_DX] = 0;
    if (!o) return 4;
    uint16_t h = picture_handle(w, o, (unsigned)o->pformat);
    if (h) w16_clip_set(w, (uint16_t)o->pformat, h);
    w->cpu.r[R_AX] = h ? OLE_OK : OLE_ERROR_FORMAT;
    return 4;
}

// --- objects from the clipboard ------------------------------------------------------------

// The clipboard's picture, as the format a static object keeps: a metafile
// (`*fmt` CF_METAFILEPICT, with its size) or a packed DIB. Allocated; NULL none.
static uint8_t *clip_picture(struct w16 *w, int *fmt, uint32_t *len, int32_t *pw, int32_t *ph) {
    uint32_t size;
    uint16_t h = w16_clip_get(w, CF_METAFILEPICT);
    const uint8_t *p = h ? w16_global_ptr(w, h, &size) : 0;
    if (p && size >= 8) {
        uint16_t hmf = (uint16_t)(p[6] | p[7] << 8);
        int32_t xe = (int16_t)(p[2] | p[3] << 8), ye = (int16_t)(p[4] | p[5] << 8);
        uint32_t ms;
        const uint8_t *m = w16_global_ptr(w, hmf, &ms);
        uint8_t *copy = m ? malloc(ms) : 0;
        if (copy) {
            memcpy(copy, m, ms);
            *fmt = CF_METAFILEPICT;
            *len = ms;
            *pw = xe > 0 ? xe : 2540;
            *ph = ye > 0 ? ye : 2540;
            return copy;
        }
    }
    h = w16_clip_get(w, CF_DIB);
    p = h ? w16_global_ptr(w, h, &size) : 0;
    int bw, bh;
    if (p && dib_size(p, size, &bw, &bh)) {
        uint8_t *copy = malloc(size);
        if (!copy) return 0;
        memcpy(copy, p, size);
        *fmt = CF_DIB;
        *len = size;
        *pw = HIMETRIC_OF(bw);
        *ph = HIMETRIC_OF(bh);
        return copy;
    }
    return 0;
}

static int is_static(struct w16 *w, uint32_t proto) {
    char p[32] = "";
    if ((proto >> 16) && cpu16_str(&w->cpu, (uint16_t)(proto >> 16), (uint16_t)proto, p, sizeof p) < 0) return -1;
    return !strcmp(p, "Static");
}

// Only a picture can be pasted as an object: without a server, an embedded or linked one cannot be made.
// OleQueryCreateFromClip(lpszProtocol, renderopt, cfFormat) -> OLESTATUS
// OleCreateFromClip(lpszProtocol, lpClient, lhClientDoc, lpszObjname, lplpObject, renderopt, cfFormat) -> OLESTATUS
// Windows: Whether the clipboard holds an object of a protocol -- "StdFileEditing" (one to edit in
// its server) or "Static" (a picture) -- and make one from it.
// w16: A static object from the clipboard's picture (a metafile, a DIB or a bitmap, a toy-os picture
// included). An embedded object is refused (OLE_ERROR_FORMAT).
// Why: An embedded object is made by its server, which w16 cannot run.
// Notice: Paste and Paste Special paste a picture, never an object you can edit later.
// Whole: Servers. On the roadmap: "w16: one program starting or finding another".
static int api_OleQueryCreateFromClip(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint32_t proto = w16_arg32(a);
    uint32_t client = 0, doc = 0, out = 0;
    if (ordinal == 12) { client = w16_arg32(a); doc = w16_arg32(a); w16_arg32(a); out = w16_arg32(a); }
    w16_arg16(a); w16_arg16(a);
    int st = is_static(w, proto);
    if (st < 0) return -1;
    w->cpu.r[R_AX] = OLE_ERROR_FORMAT;
    w->cpu.r[R_DX] = 0;
    int fmt;
    uint32_t len;
    int32_t pw, ph;
    uint8_t *pic = st ? clip_picture(w, &fmt, &len, &pw, &ph) : 0;
    if (pic && ordinal == 12) {
        struct ole_obj *o = object_new(w);
        if (o && make_static(o, fmt, pic, len, pw, ph)) {
            o->client = client;
            o->doc = doc;
            if (!put_far(w, out, far_obj(o))) { free(pic); return -1; }
            w->cpu.r[R_AX] = OLE_OK;
        } else {
            if (o) object_free(w, o);
            w->cpu.r[R_AX] = OLE_ERROR_MEMORY;
        }
    } else if (pic) {
        w->cpu.r[R_AX] = OLE_OK;
    }
    free(pic);
    return ordinal == 12 ? 24 : 8;
}

// No server can be started, so no object can be made new, from a file or as a link.
// OleQueryLinkFromClip(lpszProtocol, renderopt, cfFormat) -> OLESTATUS
// OleCreateLinkFromClip(lpszProtocol, lpClient, lhClientDoc, lpszObjname, lplpObject, renderopt, cfFormat) -> OLESTATUS
// OleCreate(lpszProtocol, lpClient, lpszClass, lhClientDoc, lpszObjname, lplpObject, renderopt, cfFormat) -> OLESTATUS
// OleCreateFromFile(lpszProtocol, lpClient, lpszClass, lpszFile, lhClientDoc, lpszObjname, lplpObject, renderopt, cfFormat) -> OLESTATUS
// OleCreateLinkFromFile(lpszProtocol, lpClient, lpszClass, lpszFile, lpszItem, lhClientDoc, lpszObjname, lplpObject, renderopt, cfFormat) -> OLESTATUS
// Windows: Make an object: linked to what the clipboard says; new, in a class's server (Insert
// Object); or from a file, embedded or linked.
// w16: Refused: a link or a new object needs its server running (OLE_ERROR_FORMAT for the
// clipboard, OLE_ERROR_CLASS for a class).
// Why: OLE 1.0's servers are other Windows programs, and w16 runs each program alone.
// Notice: Paste Link stays greyed, and Insert Object lists no kinds of object.
// Whole: One Windows program starting another and the two talking (DDE). On the roadmap: "w16:
// one program starting or finding another".
static int api_OleQueryLinkFromClip(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    unsigned bytes = ordinal == 9 ? 8 : ordinal == 11 ? 24 : ordinal == 34 ? 28 : ordinal == 38 ? 32 : 36;
    (void)a;
    w->cpu.r[R_AX] = ordinal == 9 || ordinal == 11 ? OLE_ERROR_FORMAT : OLE_ERROR_CLASS;
    w->cpu.r[R_DX] = 0;
    return (int)bytes;
}

// OleQueryReleaseStatus(lpObject) -> OLESTATUS
// OleQueryReleaseError(lpObject) -> OLESTATUS
// OleQueryReleaseMethod(lpObject) -> OLE_RELEASE_METHOD
// Windows: Whether an object's last asynchronous operation has finished, how it ended, and which
// one it was.
// w16: Finished (OLE_OK), with no error: every operation here finishes before its call returns.
static int api_OleQueryReleaseStatus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct ole_obj *o = object_of(w16_arg32(a));
    w->cpu.r[R_AX] = o ? OLE_OK : OLE_ERROR_OBJECT;
    w->cpu.r[R_DX] = 0;
    return 4;
}

const struct w16_api g_olecli_api[] = {
    { 2, api_OleDelete },
    { 3, api_OleSaveToStream },
    { 4, api_OleLoadFromStream },
    { 6, api_OleClone },
    { 9, api_OleQueryLinkFromClip },
    { 10, api_OleQueryCreateFromClip },
    { 11, api_OleQueryLinkFromClip },
    { 12, api_OleQueryCreateFromClip },
    { 13, api_OleCopyToClipboard },
    { 14, api_OleQueryType },
    { 15, api_OleSetHostNames },
    { 16, api_OleSetHostNames },
    { 18, api_OleQueryBounds },
    { 19, api_OleDraw },
    { 20, api_OleQueryOpen },
    { 21, api_OleActivate },
    { 22, api_OleQueryOpen },
    { 24, api_OleGetLinkUpdateOptions },
    { 25, api_OleGetLinkUpdateOptions },
    { 26, api_OleEnumFormats },
    { 27, api_OleQueryOpen },
    { 28, api_OleEnumFormats },
    { 29, api_OleEnumFormats },
    { 32, api_OleObjectConvert },
    { 34, api_OleQueryLinkFromClip },
    { 35, api_OleQueryReleaseStatus },
    { 36, api_OleQueryReleaseStatus },
    { 37, api_OleQueryReleaseStatus },
    { 38, api_OleQueryLinkFromClip },
    { 39, api_OleQueryLinkFromClip },
    { 40, api_OleDelete },
    { 41, api_OleRegisterClientDoc },
    { 42, api_OleRegisterClientDoc },
    { 43, api_OleRegisterClientDoc },
    { 44, api_OleRegisterClientDoc },
    { 45, api_OleRegisterClientDoc },
    { 47, api_OleEnumObjects },
    { 56, api_OleQuerySize },
};
const unsigned g_olecli_api_n = sizeof g_olecli_api / sizeof g_olecli_api[0];

// --- SHELL's registration database -----------------------------------------------------------

#define HKEY_CLASSES_ROOT 1
#define ERROR_SUCCESS     0
#define ERROR_BADKEY      2

// The registration database has no programs in it: what OLE servers there are, and how files open.
// RegOpenKey(hkey, lpszSubKey, lphkResult) -> LONG
// RegCloseKey(hkey) -> LONG
// RegQueryValue(hkey, lpszSubKey, lpszValue, lpcb) -> LONG
// RegEnumKey(hkey, iSubKey, lpszBuffer, cbBuffer) -> LONG
// Windows: Read the registration database (REG.DAT): its keys under HKEY_CLASSES_ROOT -- each
// document class, the program that opens it, the OLE server that edits it -- and their values.
// w16: The database is empty: the root opens, and every other key is not there (ERROR_BADKEY), so
// a program enumerating OLE servers finds none.
// Why: An entry there names a Windows program to run as a server, and w16 cannot run one program
// for another.
// Notice: Insert Object offers no kinds of object.
// Whole: The database, with servers w16 can start. On the roadmap: "w16: one program starting or
// finding another".
static int api_RegOpenKey(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    w->cpu.r[R_DX] = 0;
    w->cpu.r[R_AX] = ERROR_BADKEY;
    switch (ordinal) {
    case 1: {
        uint32_t key = w16_arg32(a), sub = w16_arg32(a), out = w16_arg32(a);
        char s[8] = "";
        if ((sub >> 16) && cpu16_str(&w->cpu, (uint16_t)(sub >> 16), (uint16_t)sub, s, sizeof s) < 0) s[0] = 'x';
        if (key == HKEY_CLASSES_ROOT && !s[0]) {
            if (!put_far(w, out, HKEY_CLASSES_ROOT)) return -1;
            w->cpu.r[R_AX] = ERROR_SUCCESS;
        }
        return 12;
    }
    case 3:
        w16_arg32(a);
        w->cpu.r[R_AX] = ERROR_SUCCESS;
        return 4;
    default:                                      // 6 RegQueryValue, 7 RegEnumKey
        w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
        return 16;
    }
}

const struct w16_api g_reg_api[] = {
    { 1, api_RegOpenKey },
    { 3, api_RegOpenKey },
    { 6, api_RegOpenKey },
    { 7, api_RegOpenKey },
};
const unsigned g_reg_api_n = sizeof g_reg_api / sizeof g_reg_api[0];
