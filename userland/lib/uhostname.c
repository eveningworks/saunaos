// See uhostname.h.
#include <string.h>

#include "lib/uhostname.h"
#include "lib/usetting.h"

int uhostname_valid(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > UHOSTNAME_MAX || name[0] == '-' || name[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-'))
            return 0;
    }
    return 1;
}

void uhostname_get(char *out, int cap) {
    char v[UHOSTNAME_MAX + 2];
    if (usetting_get(UHOSTNAME_SETTING, v, sizeof v) && uhostname_valid(v))
        strlcpy(out, v, (size_t)cap);
    else
        strlcpy(out, UHOSTNAME_DEFAULT, (size_t)cap);
}
