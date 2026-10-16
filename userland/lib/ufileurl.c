// ufileurl -- see lib/ufileurl.h.
#include "lib/ufileurl.h"
#include <string.h>
#include <strings.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ufileurl_to_path(const char *url, char *out, size_t cap) {
    if (strncasecmp(url, "file:", 5) != 0) return -1;
    const char *p = url + 5;
    if (!strncmp(p, "//", 2)) {
        p += 2;
        // The authority: empty or "localhost" is this machine, anything
        // else a remote host, which a local read must not pretend to be.
        const char *slash = strchr(p, '/');
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl && !(hl == 9 && !strncasecmp(p, "localhost", 9))) return -1;
        p += hl;
    }
    if (*p != '/') return -1;
    size_t n = 0;
    for (; *p && *p != '?' && *p != '#'; p++) {
        char c = *p;
        if (c == '%') {
            int hi = hexval(p[1]), lo = hi < 0 ? -1 : hexval(p[2]);
            if (lo < 0 || (hi | lo) == 0) return -1;
            c = (char)(hi * 16 + lo);
            p += 2;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    if (!cap) return -1;
    out[n] = '\0';
    return 0;
}

const char *ufileurl_mime(const char *path) {
    static const struct { const char *ext, *type; } T[] = {
        { "html", "text/html" }, { "htm", "text/html" }, { "xhtml", "application/xhtml+xml" },
        { "css", "text/css" }, { "js", "application/javascript" },
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
        { "gif", "image/gif" }, { "bmp", "image/bmp" }, { "ico", "image/x-icon" },
        { "svg", "image/svg+xml" }, { "qoi", "image/qoi" },
        { "txt", "text/plain" }, { "md", "text/plain" }, { "conf", "text/plain" },
    };
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
            if (!strcasecmp(dot + 1, T[i].ext)) return T[i].type;
    return "text/plain";
}
