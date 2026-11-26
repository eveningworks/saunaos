// whoami -- the name of the effective user: getpwuid(geteuid()), GNU's.
#include "base/uargs.h"
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

static const struct uargs_prog PROG = {
    "whoami", "",
    "Print the name of the user this process runs as.",
    0, 0, 0, 0, 0, 0, 0,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "extra operand '%s'", a.argv[0]);
    uid_t u = geteuid();
    struct passwd *pw = getpwuid(u);
    if (!pw) {
        fprintf(stderr, "whoami: cannot find name for user ID %u\n", (unsigned)u);
        return 1;
    }
    printf("%s\n", pw->pw_name);
    return 0;
}
