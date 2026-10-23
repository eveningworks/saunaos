#include "lib/ufiletype.h"
#include <strings.h>
#include <stdio.h>
#include <string.h>

// One row per extension. Kept short: a type nobody meets here is better
// called "File" than guessed at.
static const struct { const char *ext, *name, *icon; } TYPES[] = {
    { "txt",     "Text",            "file-text" },
    { "log",     "Log",             "file-text" },
    { "crash",   "Crash report",    "file-doc" },
    { "md",      "Markdown",        "file-doc" },
    { "c",       "C source",        "file-text" },
    { "h",       "C header",        "file-text" },
    { "py",      "Python script",   "file-text" },
    { "sh",      "Shell script",    "file-text" },
    { "conf",    "Settings file",   "file-config" },
    { "ids",     "Hardware list",   "file-text" },
    { "desktop", "App shortcut",    "file-app" },
    { "jpg",     "JPEG image",      "file-image" },
    { "jpeg",    "JPEG image",      "file-image" },
    { "png",     "PNG image",       "file-image" },
    { "qoi",     "QOI image",       "file-image" },
    { "bmp",     "Bitmap image",    "file-image" },
    { "gif",     "GIF image",       "file-image" },
    { "ppm",     "PPM image",       "file-image" },
    { "wav",     "WAV audio",       "file-audio" },
    { "mp3",     "MP3 audio",       "file-audio" },
    { "flac",    "FLAC audio",      "file-audio" },
    { "mid",     "MIDI music",      "file-audio" },
    { "sf2",     "SoundFont",       "file-audio" },
    { "mpg",     "MPEG video",      "file-video" },
    { "mpeg",    "MPEG video",      "file-video" },
    { "avi",     "AVI video",       "file-video" },
    { "ttf",     "Font",            "file-font" },
    { "zip",     "ZIP archive",     "file-archive" },
    { "tar",     "Tar archive",     "file-archive" },
    { "tgz",     "Tar archive",     "file-archive" },
    { "gz",      "Gzip file",       "file-archive" },
    { "so",      "Shared library",  "file" },
    { "wad",     "Game data",       "file" },
};

static int lookup(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    // A leading dot is a hidden name, not an extension.
    if (!dot || dot == name || !dot[1]) return -1;
    // ONE TYPE, TWO DOTS: a .tar.gz is a tar archive, not a gzip file.
    const char *ext = dot + 1;
    if (!strcasecmp(ext, "gz") && dot - name > 4 && !strncasecmp(dot - 4, ".tar", 4)) ext = "tar";
    for (int i = 0; i < (int)(sizeof TYPES / sizeof TYPES[0]); i++)
        if (!strcasecmp(ext, TYPES[i].ext)) return i;
    return -1;
}

const char *ufiletype_name(const char *name, int is_dir) {
    if (is_dir) return "Folder";
    int i = lookup(name ? name : "");
    return i < 0 ? "File" : TYPES[i].name;
}

// A FOLDER OF A KNOWN KIND, by its NAME -- Plasma's folder-music and
// Explorer's special folders, which key on the well-known names the same
// way. The names cover the home places and this system's own
// /usr/share; anything else is the plain folder.
static const struct { const char *name, *icon; } FOLDERS[] = {
    { "music",      "folder-music" },     { "sounds",     "folder-sounds" },
    { "pictures",   "folder-pictures" },  { "wallpapers", "folder-pictures" },
    { "images",     "folder-pictures" },  { "photos",     "folder-pictures" },
    { "screenshots","folder-pictures" },  { "documents",  "folder-documents" },
    { "doc",        "folder-documents" }, { "docs",       "folder-documents" },
    { "fonts",      "folder-fonts" },     { "icons",      "folder-icons" },
    { "cursors",    "folder-cursors" },   { "terminal",   "folder-terminal" },
    { "services",   "folder-services" },  { "hwdata",     "folder-hwdata" },
    { "soundfonts", "folder-soundfonts" },{ "home",       "folder-home" },
};

const char *ufiletype_icon(const char *name, int is_dir) {
    if (is_dir) {
        for (int i = 0; name && i < (int)(sizeof FOLDERS / sizeof FOLDERS[0]); i++)
            if (!strcasecmp(name, FOLDERS[i].name)) return FOLDERS[i].icon;
        return "folder";
    }
    int i = lookup(name ? name : "");
    return i < 0 ? "file" : TYPES[i].icon;
}

enum ufiletype_kind ufiletype_kind(const char *name, int is_dir) {
    if (is_dir) return UFILETYPE_FOLDER;
    const char *ic = ufiletype_icon(name, 0);
    if (!strcmp(ic, "file-image")) return UFILETYPE_PICTURE;
    if (!strcmp(ic, "file-audio")) return UFILETYPE_SOUND;
    if (!strcmp(ic, "file-video")) return UFILETYPE_VIDEO;
    if (!strcmp(ic, "file-text") || !strcmp(ic, "file-doc") || !strcmp(ic, "file-config"))
        return UFILETYPE_DOCUMENT;
    if (!strcmp(ic, "file-archive")) return UFILETYPE_ARCHIVE;
    if (!strcmp(ic, "file-app")) return UFILETYPE_PROGRAM;
    return UFILETYPE_OTHER;
}

const char *ufiletype_kind_name(enum ufiletype_kind k) {
    static const char *const NAMES[UFILETYPE_KINDS] = {
        "Folders", "Pictures", "Music and sounds", "Videos", "Documents", "Archives", "Programs", "Other",
    };
    return (unsigned)k < UFILETYPE_KINDS ? NAMES[k] : "Other";
}

void ufiletype_display_name(const char *name, int is_dir, int hide_ext, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s", name);
    if (!hide_ext || is_dir || !strcmp(ufiletype_name(name, 0), "File")) return;
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}
