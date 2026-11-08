// <ctype.h>, out of line: ASCII only, the header says why. Four are the
// toolkit's own predicates under C's names; the rest are defined here,
// where the C library is their only caller.
#include <ctype.h>
#include <kstring.h>

int isdigit(int c) { return k_isdigit((char)c); }
int isspace(int c) { return k_isspace((char)c); }
int tolower(int c) { return k_tolower(c); }
int toupper(int c) { return k_toupper(c); }

int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 0x20 && c < 0x7F; }
int isgraph(int c) { return c > 0x20 && c < 0x7F; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isascii(int c) { return c >= 0 && c <= 0x7F; }
int toascii(int c) { return c & 0x7F; }
