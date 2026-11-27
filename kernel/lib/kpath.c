// See kpath.h for the API and for why three different path resolvers
// existed before it.
//
// The normalizer keeps shell.c's resolve_path() rule that ".." clamps at
// the root, and works on a caller-supplied buffer with an explicit
// capacity, so the Terminal and the shell share it
// (kernel/lib/kpath_test.c covers it).
#include "kpath.h"
#include "string.h"

int k_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

int k_path_join(const char *dir, const char *name, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (!name) name = "";

    // An absolute `name` wins outright -- `cd /tmp` from anywhere means
    // /tmp, it doesn't mean "cwd + /tmp".
    if (k_path_is_absolute(name)) {
        if (k_strlen(name) + 1 > cap) return 0;
        k_strlcpy(out, name, cap);
        return 1;
    }

    if (!dir) dir = "";
    size_t dl = k_strlen(dir);
    // Trim trailing slashes so "/bin/" + "ls" and "/bin" + "ls" agree,
    // and so the root case doesn't produce "//ls".
    while (dl > 0 && dir[dl - 1] == '/') dl--;

    size_t nl = k_strlen(name);
    size_t need = dl + 1 + nl; // dir + '/' + name
    if (need + 1 > cap) return 0;

    for (size_t i = 0; i < dl; i++) out[i] = dir[i];
    out[dl] = '/';
    for (size_t i = 0; i < nl; i++) out[dl + 1 + i] = name[i];
    out[need] = '\0';
    return 1;
}

// `scratch` is compacted in place into the answer without its leading
// '/': each kept segment is moved down to `w`, and ".." backs `w` up
// over the last one. The write never passes the read -- every kept
// segment had at least one '/' before it in the input, which pays for
// the one written -- so no segment stack is needed and depth has no
// bound; only the answer's bytes are checked against `cap`.
static int normalize_in_place(char *scratch, char *out, size_t cap) {
    size_t r = 0, w = 0;
    while (scratch[r]) {
        while (scratch[r] == '/') r++;
        if (!scratch[r]) break;
        size_t start = r;
        while (scratch[r] && scratch[r] != '/') r++;
        size_t seglen = r - start;

        if (seglen == 1 && scratch[start] == '.') continue;
        if (seglen == 2 && scratch[start] == '.' && scratch[start + 1] == '.') {
            // ".." at the root stays at the root rather than escaping
            // above it (see kpath.h).
            while (w > 0 && scratch[w - 1] != '/') w--;
            if (w > 0) w--;   // the separator before it
            continue;
        }
        if (w > 0) scratch[w++] = '/';
        k_memmove(scratch + w, scratch + start, seglen);
        w += seglen;
    }

    if (w + 2 > cap) return 0;   // '/' + the segments + NUL
    out[0] = '/';
    k_memcpy(out + 1, scratch, w);
    out[w + 1] = '\0';
    return 1;
}

// A scratch of at least two segments plus separators -- anything
// smaller cannot hold even "/" joined to a name, and a caller that
// passes one is a bug worth failing on rather than truncating into.
#define KPATH_SCRATCH_MIN 8

static int scratch_ok(const struct kpath_scratch *s) {
    return s && s->buf && s->cap >= KPATH_SCRATCH_MIN;
}

int k_path_normalize(const char *path, char *out, size_t cap,
                     const struct kpath_scratch *scratch) {
    if (!path || !out || !scratch_ok(scratch)) return 0;
    if (k_strlcpy(scratch->buf, path, scratch->cap) >= scratch->cap) return 0;
    return normalize_in_place(scratch->buf, out, cap);
}

int k_path_resolve(const char *base, const char *input, char *out, size_t cap,
                   const struct kpath_scratch *scratch) {
    if (!out || !scratch_ok(scratch)) return 0;
    if (!base) base = "/";

    char  *sb = scratch->buf;
    size_t sc = scratch->cap;
    if (!input || input[0] == '\0') {
        if (k_strlcpy(sb, base, sc) >= sc) return 0;
    } else if (k_path_is_absolute(input)) {
        if (k_strlcpy(sb, input, sc) >= sc) return 0;
    } else {
        if (!k_path_join(base, input, sb, sc)) return 0;
    }
    return normalize_in_place(sb, out, cap);
}

int k_path_component_from_disk(char *name) {
    for (char *c = name; *c; c++)
        if (*c == '/') *c = '_';
    return name[0] && k_strcmp(name, ".") != 0 && k_strcmp(name, "..") != 0;
}

const char *k_path_basename(const char *path) {
    if (!path) return "";
    const char *slash = k_strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int k_path_dirname(const char *path, char *out, size_t cap) {
    if (!path || !out || cap == 0) return 0;

    const char *slash = k_strrchr(path, '/');
    // No separator at all, or the only one is the leading root slash:
    // either way the containing directory is the root.
    if (!slash || slash == path) {
        if (cap < 2) return 0;
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    size_t len = (size_t)(slash - path);
    if (len + 1 > cap) return 0;
    k_memcpy(out, path, len);
    out[len] = '\0';
    return 1;
}
