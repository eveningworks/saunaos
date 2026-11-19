#ifndef ULIB_UFONTFALLBACK_H
#define ULIB_UFONTFALLBACK_H

#ifdef __cplusplus
extern "C" {
#endif

// The FONT FALLBACK LIST: the faces tried, in order, for a character the
// interface face has no glyph for -- Android's fonts.xml shape, one ordered
// list, the first face with the character drawing it. Read once per process
// from /etc/fonts/fallback.conf; the same list is compiled in for a machine
// without the file or a process that cannot read it (a sandbox).
//
//   # a comment
//   noto-sans              a face: NAME.ttf, .otf or .ttc, the first found
//                          in /usr/share/fonts/fallback, then /usr/share/fonts
//   noto-sans-cjk-regular:2  member 2 of a collection (.ttc): fontconfig's
//                          `:index=`, FreeType's face_index; no `:N` is 0
//   emoji twemoji          the face an emoji goes to FIRST (base/uemoji.h)
//
// A name is a file stem, never a path; a line that is neither form is
// skipped and said once in the log.
#define UFONT_FALLBACK_CONF "/etc/fonts/fallback.conf"
#define UFONT_FALLBACK_DIR  "/usr/share/fonts/fallback"
#define UFONT_DIR           "/usr/share/fonts"

struct ufont_fallback_face {
    char *path;     // where the file is (the fallback folder's when it is there)
    int emoji;      // 1 for the emoji face
    int index;      // the member of a collection; 0 for a lone font
};

struct ufont_fallback {
    struct ufont_fallback_face *faces;
    int n;
};

// The list, loaded on first use and kept for the process. Never NULL; an
// empty list (n 0) only when memory ran out.
const struct ufont_fallback *ufont_fallback(void);

// Parses `text` (the file's contents) into `out`, resolving each name with
// `exists` (a stat in the program; a lookup in a test). 1, or 0 when memory
// ran out. Exposed for the host test; ufont_fallback() is the one to call.
int ufont_fallback_parse(const char *text, int (*exists)(const char *path),
                         struct ufont_fallback *out);

#ifdef __cplusplus
}
#endif

#endif
