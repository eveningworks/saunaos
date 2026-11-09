#ifndef ULIB_UINITIALS_H
#define ULIB_UINITIALS_H

// A NAME'S INITIALS, for a tile that stands in for a missing icon: one
// or two letters -- "Web Browser" -> "WB", "Windows 3.x support" -> "W3",
// "Web page engine" -> "W" -- what Windows' Start menu and Android draw
// for an app with no icon, and an address book for a contact with no
// photo. The first word's first character, then the second word's when
// it starts with a capital or a digit; ASCII letters are upper-cased and
// a non-ASCII character is copied whole (UTF-8), never split.
#include <stddef.h>

// Into `out` (at most `cap` bytes, NUL-terminated); "" for an empty name.
void uinitials(const char *name, char *out, size_t cap);

#endif
