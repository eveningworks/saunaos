// OpenRaster -- see lib/uora.h.
#include "lib/uora.h"
#include "lib/uzip.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIMETYPE "image/openraster"
#define MEMBER_MAX (64u * 1024u * 1024u)   // uimg's own file cap
#define XML_MAX (1u << 20)
#define THUMB_MAX 256

static int failf(char *err, int cap, int rc, const char *fmt, ...) {
    if (err && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, (size_t)cap, fmt, ap);
        va_end(ap);
    }
    return rc;
}

// IDENTIFIERS, written into every .ora Paint saves -- the namespace and
// its `toyos:` attribute prefix keep the pre-rename name, because a file
// saved before then must still read back.
#define NS "https://github.com/eveningworks/toy-os/ns/openraster"

void uora_free(struct uora_layer *layers, int n) {
    if (!layers) return;
    for (int i = 0; i < n; i++) {
        uimg_free(&layers[i].img);
        for (int k = 0; k < layers[i].nattrs; k++) free(layers[i].attrs[k].value);
    }
    free(layers);
}

// --- writing ----------------------------------------------------------

// `s` into `out` with XML's five specials escaped, and every byte past
// ASCII or below a space as a character reference -- a name is Latin-1
// here and the file says UTF-8, and a newline in an attribute would be
// read back as a space. 0 when it does not fit.
static int xml_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s; s++) {
        char num[8];
        unsigned char u = (unsigned char)*s;
        int ref = u >= 0x80 || u < 0x20;   // and a newline, which an attribute would turn into a space
        if (ref) snprintf(num, sizeof num, "&#%u;", u);
        const char *rep = ref ? num : *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;"
                        : *s == '"' ? "&quot;" : *s == '\'' ? "&apos;" : 0;
        size_t n = rep ? strlen(rep) : 1;
        if (o + n >= cap) return 0;
        if (rep) memcpy(out + o, rep, n);
        else out[o] = *s;
        o += n;
    }
    out[o] = '\0';
    return 1;
}

static int add_png(struct uzip_writer *w, const char *name, const struct uimg *im, char *err, int errcap) {
    struct uimg a = *im;
    a.has_alpha = 1;   // a layer's transparency is the point of it
    uint8_t *png;
    size_t len;
    int rc = uimg_encode(&a, "png", &png, &len);
    if (rc != 0) return failf(err, errcap, rc, "could not encode %s", name);
    rc = uzip_add_mem(w, name, png, len, 0, err, errcap);   // PNG is deflated already
    free(png);
    return rc;
}

int uora_save(const char *path, const struct uora_layer *layers, int n, const struct uimg *merged,
              char *err, int errcap) {
    if (n < 1 || n > UORA_LAYERS_MAX) return failf(err, errcap, -EINVAL, "%d layers", n);
    int w = layers[0].img.w, h = layers[0].img.h;
    for (int i = 0; i < n; i++)
        if (layers[i].img.w != w || layers[i].img.h != h || !layers[i].img.px)
            return failf(err, errcap, -EINVAL, "layer %d is not the picture's size", i);
    // The stack, TOP FIRST.
    size_t cap = 512 + (size_t)n * (UORA_NAME_MAX * 6 + 160);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < layers[i].nattrs; k++)
            cap += 40 + (layers[i].attrs[k].value ? strlen(layers[i].attrs[k].value) * 6 : 0);
    char *xml = malloc(cap);
    if (!xml) return failf(err, errcap, -ENOMEM, "out of memory");
    size_t at = (size_t)snprintf(xml, cap,
                                 "<?xml version='1.0' encoding='UTF-8'?>\n"
                                 "<image version=\"0.0.3\" w=\"%d\" h=\"%d\" xmlns:toyos=\"" NS "\">\n <stack>\n",
                                 w, h);
    for (int i = n - 1; i >= 0; i--) {
        char name[UORA_NAME_MAX * 6];
        if (!xml_escape(layers[i].name, name, sizeof name)) name[0] = '\0';
        int op = layers[i].opacity < 0 ? 0 : layers[i].opacity > 255 ? 255 : layers[i].opacity;
        at += (size_t)snprintf(xml + at, cap - at,
                               "  <layer name=\"%s\" src=\"data/layer%d.png\" x=\"0\" y=\"0\" "
                               "opacity=\"%d.%03d\" visibility=\"%s\" composite-op=\"svg:src-over\"",
                               name, i, op / 255, op % 255 * 1000 / 255, layers[i].hidden ? "hidden" : "visible");
        for (int k = 0; k < layers[i].nattrs; k++) {
            const struct uora_attr *a = &layers[i].attrs[k];
            size_t vl = a->value ? strlen(a->value) * 6 + 1 : 1;
            char *v = malloc(vl);
            if (!v || !xml_escape(a->value ? a->value : "", v, vl)) { free(v); free(xml); return failf(err, errcap, -ENOMEM, "out of memory"); }
            at += (size_t)snprintf(xml + at, cap - at, " toyos:%s=\"%s\"", a->name, v);
            free(v);
        }
        at += (size_t)snprintf(xml + at, cap - at, "/>\n");
    }
    at += (size_t)snprintf(xml + at, cap - at, " </stack>\n</image>\n");

    struct uzip_writer zw;
    int rc = uzip_create_begin(&zw, path, err, errcap);
    if (rc == 0) rc = uzip_add_mem(&zw, "mimetype", MIMETYPE, strlen(MIMETYPE), 0, err, errcap);
    if (rc == 0) rc = uzip_add_mem(&zw, "stack.xml", xml, at, 6, err, errcap);
    for (int i = 0; rc == 0 && i < n; i++) {
        char name[32];
        snprintf(name, sizeof name, "data/layer%d.png", i);
        rc = add_png(&zw, name, &layers[i].img, err, errcap);
    }
    if (rc == 0 && merged) rc = add_png(&zw, "mergedimage.png", merged, err, errcap);
    if (rc == 0 && merged) {
        // At most 256 on its long side, the spec's bound, never enlarged.
        int tw = w, th = h;
        if (tw > THUMB_MAX || th > THUMB_MAX) {
            if (tw >= th) { th = (int)((long long)th * THUMB_MAX / tw); tw = THUMB_MAX; }
            else { tw = (int)((long long)tw * THUMB_MAX / th); th = THUMB_MAX; }
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
        }
        struct uimg t = { 0 };
        if (uimg_scale(merged, tw, th, &t) == 0) {
            rc = add_png(&zw, "Thumbnails/thumbnail.png", &t, err, errcap);
            uimg_free(&t);
        }
    }
    free(xml);
    if (rc == 0) return uzip_create_end(&zw, err, errcap);
    uzip_create_abort(&zw);
    return rc;
}

// --- reading ----------------------------------------------------------

// The value of attribute `name` in the tag [p, end), unescaped, into out.
// 1 found, 0 absent, -1 malformed or too long.
static int attr(const char *p, const char *end, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *q = p; q + nl + 2 < end; q++) {
        if ((q == p || q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n' || q[-1] == '\r') &&
            !strncmp(q, name, nl) && q[nl] == '=' && (q[nl + 1] == '"' || q[nl + 1] == '\'')) {
            char quote = q[nl + 1];
            const char *v = q + nl + 2;
            size_t o = 0;
            while (v < end && *v != quote) {
                char c = *v++;
                if (c == '&') {
                    const char *semi = memchr(v, ';', (size_t)(end - v));
                    if (!semi || semi - v > 8) return -1;
                    size_t el = (size_t)(semi - v);
                    if (el == 3 && !strncmp(v, "amp", 3)) c = '&';
                    else if (el == 2 && !strncmp(v, "lt", 2)) c = '<';
                    else if (el == 2 && !strncmp(v, "gt", 2)) c = '>';
                    else if (el == 4 && !strncmp(v, "quot", 4)) c = '"';
                    else if (el == 4 && !strncmp(v, "apos", 4)) c = '\'';
                    else if (el >= 2 && v[0] == '#') {
                        long cp = v[1] == 'x' ? strtol(v + 2, 0, 16) : strtol(v + 1, 0, 10);
                        c = cp > 0 && cp < 256 ? (char)cp : '?';   // Latin-1, as every string drawn here
                    } else {
                        return -1;
                    }
                    v = semi + 1;
                }
                if (o + 1 >= cap) return -1;
                out[o++] = c;
            }
            if (v >= end) return -1;
            out[o] = '\0';
            return 1;
        }
    }
    return 0;
}

// "0.5" -> 128, of 255: digits, an optional fraction, clamped to 0..1.
static int parse_opacity(const char *s) {
    long whole = 0, frac = 0, scale = 1;
    while (*s >= '0' && *s <= '9') whole = whole * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && scale < 1000000) { frac = frac * 10 + (*s++ - '0'); scale *= 10; }
    }
    if (whole >= 1) return 255;
    return (int)((frac * 255 + scale / 2) / scale);
}

static int to_int(const char *s, int *out) {
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s || *e || v < -1000000 || v > 1000000) return 0;
    *out = (int)v;
    return 1;
}

int uora_load(const char *path, struct uora_layer **out, int *n, int *w, int *h, char *err, int errcap) {
    *out = 0;
    *n = 0;
    unsigned char *mt = 0, *xml = 0;
    size_t len;
    int rc = uzip_read(path, "mimetype", 64, &mt, &len, err, errcap);
    if (rc < 0) return rc;
    int is_ora = len == strlen(MIMETYPE) && !memcmp(mt, MIMETYPE, len);
    free(mt);
    if (!is_ora) return failf(err, errcap, -EINVAL, "not an OpenRaster picture");
    rc = uzip_read(path, "stack.xml", XML_MAX, &xml, &len, err, errcap);
    if (rc < 0) return rc;
    const char *x = (const char *)xml, *xend = x + len;

    // <image w h>
    const char *img = 0;
    for (const char *q = x; q + 6 < xend; q++)
        if (!strncmp(q, "<image", 6) && (q[6] == ' ' || q[6] == '\n' || q[6] == '\t')) { img = q; break; }
    const char *img_end = img ? memchr(img, '>', (size_t)(xend - img)) : 0;
    char v[UORA_NAME_MAX * 6];
    int cw = 0, ch = 0;
    if (!img_end || attr(img, img_end, "w", v, sizeof v) != 1 || !to_int(v, &cw) ||
        attr(img, img_end, "h", v, sizeof v) != 1 || !to_int(v, &ch) || cw < 1 || ch < 1 ||
        (long long)cw * ch > (64ll << 20) / 4) {
        free(xml);
        return failf(err, errcap, -EINVAL, "stack.xml has no picture size");
    }
    struct uora_layer *L = calloc(UORA_LAYERS_MAX, sizeof *L);
    if (!L) { free(xml); return failf(err, errcap, -ENOMEM, "out of memory"); }
    int count = 0;
    rc = 0;
    // Each <layer .../>, in the file's top-first order.
    for (const char *q = img_end; rc == 0 && q + 6 < xend; q++) {
        if (strncmp(q, "<layer", 6) || !(q[6] == ' ' || q[6] == '\n' || q[6] == '\t' || q[6] == '/')) continue;
        const char *te = memchr(q, '>', (size_t)(xend - q));
        if (!te) { rc = failf(err, errcap, -EINVAL, "stack.xml ends inside a layer"); break; }
        if (count == UORA_LAYERS_MAX) { rc = failf(err, errcap, -E2BIG, "more than %d layers", UORA_LAYERS_MAX); break; }
        struct uora_layer *l = &L[count];
        char src[UZIP_NAME_MAX];
        if (attr(q, te, "src", src, sizeof src) != 1) { rc = failf(err, errcap, -EINVAL, "a layer has no src"); break; }
        // A NAME LONGER THAN OURS IS CUT, not refused: the picture matters
        // more than its label (Krita's are unbounded).
        if (attr(q, te, "name", v, sizeof v) == 1) {
            size_t nl = strlen(v) < sizeof l->name - 1 ? strlen(v) : sizeof l->name - 1;
            memcpy(l->name, v, nl);
            l->name[nl] = '\0';
        }
        else snprintf(l->name, sizeof l->name, "Layer %d", count + 1);
        l->opacity = attr(q, te, "opacity", v, sizeof v) == 1 ? parse_opacity(v) : 255;
        l->hidden = attr(q, te, "visibility", v, sizeof v) == 1 && !strcmp(v, "hidden");
        // Our own attributes, any of them, by their prefix.
        for (const char *s = q; rc == 0 && s + 7 < te && l->nattrs < UORA_ATTRS_MAX; s++) {
            if (strncmp(s, "toyos:", 6) || !(s[-1] == ' ' || s[-1] == '\t' || s[-1] == '\n' || s[-1] == '\r')) continue;
            const char *eq = memchr(s, '=', (size_t)(te - s));
            if (!eq || eq - s - 6 <= 0 || (size_t)(eq - s - 6) >= sizeof l->attrs[0].name) continue;
            char full[40];
            snprintf(full, sizeof full, "%.*s", (int)(eq - s), s);
            size_t vcap = (size_t)(te - s) + 1;
            char *val = malloc(vcap);
            if (!val) { rc = failf(err, errcap, -ENOMEM, "out of memory"); break; }
            if (attr(q, te, full, val, vcap) != 1) { free(val); continue; }
            struct uora_attr *a = &l->attrs[l->nattrs++];
            snprintf(a->name, sizeof a->name, "%s", full + 6);
            a->value = val;
            s = eq;
        }
        if (rc < 0) break;
        int ox = 0, oy = 0;
        if (attr(q, te, "x", v, sizeof v) == 1 && !to_int(v, &ox)) ox = 0;
        if (attr(q, te, "y", v, sizeof v) == 1 && !to_int(v, &oy)) oy = 0;
        unsigned char *png;
        size_t plen;
        rc = uzip_read(path, src, MEMBER_MAX, &png, &plen, err, errcap);
        if (rc < 0) break;
        struct uimg part = { 0 };
        int dr = uimg_decode(png, plen, &part);
        free(png);
        if (dr != 0) { rc = failf(err, errcap, -EINVAL, "%s is not a picture this reads", src); break; }
        // ON THE CANVAS: placed at its offset, the rest transparent.
        l->img.w = cw;
        l->img.h = ch;
        l->img.has_alpha = 1;
        l->img.px = calloc((size_t)cw * ch, 4);
        if (!l->img.px) { uimg_free(&part); rc = failf(err, errcap, -ENOMEM, "out of memory"); break; }
        for (int y = 0; y < part.h; y++) {
            int ty = y + oy;
            if (ty < 0 || ty >= ch) continue;
            for (int xx = 0; xx < part.w; xx++) {
                int tx = xx + ox;
                if (tx >= 0 && tx < cw) l->img.px[(size_t)ty * cw + tx] = part.px[(size_t)y * part.w + xx];
            }
        }
        uimg_free(&part);
        count++;
        q = te;
    }
    free(xml);
    if (rc == 0 && count == 0) rc = failf(err, errcap, -EINVAL, "stack.xml names no layers");
    if (rc < 0) { uora_free(L, count + 1); return rc; }
    // Bottom first.
    for (int i = 0, j = count - 1; i < j; i++, j--) { struct uora_layer t = L[i]; L[i] = L[j]; L[j] = t; }
    *out = L;
    *n = count;
    *w = cw;
    *h = ch;
    return 0;
}
