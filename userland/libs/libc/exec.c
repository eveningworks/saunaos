// The exec family over SYS_EXEC. execve() is the syscall; execv() adds
// `environ`, as execv does everywhere; execvp() walks PATH for a name
// with no slash, as sh does; execl(), execlp() and execle() are the
// three with their arguments in the call, gathered into an argv. None
// returns on success.
#include <errno.h>
#include <paths.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"

int execve(const char *path, char *const argv[], char *const envp[]) {
    return sys_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return sys_execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/')) return execv(file, argv);
    const char *path = getenv("PATH");
    if (!path || !*path) path = _PATH_DEFPATH;
    int saw_noent = 0;
    for (const char *p = path; ; ) {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        char *full = malloc(dlen + 1 + strlen(file) + 1);
        if (!full) { errno = ENOMEM; return -1; }
        memcpy(full, p, dlen);
        full[dlen] = '/';
        strcpy(full + dlen + 1, file);
        execv(full, argv);              // returns only on failure
        int err = errno;                // before free(), which may set it
        free(full);
        errno = err;
        if (err == ENOENT) saw_noent = 1;
        else return -1;                 // a real refusal: stop looking
        if (!end) break;
        p = end + 1;
    }
    errno = saw_noent ? ENOENT : errno;
    return -1;
}

// The list after `arg0`, up to its NULL, as a NULL-ended argv; with
// `envp_out`, the pointer after that NULL too (execle's). The list is
// as long as the caller made it, so the array is allocated to fit.
// A NULL `arg0` is the whole list (`execl(path, (char *)0)`): nothing
// after it is read as an argument.
static char **gather(const char *arg0, va_list ap, char ***envp_out) {
    va_list n;
    va_copy(n, ap);
    size_t count = arg0 ? 1 : 0;
    while (arg0 && va_arg(n, char *)) count++;
    va_end(n);
    char **argv = malloc((count + 1) * sizeof *argv);
    if (!argv) { errno = ENOMEM; return NULL; }
    argv[0] = (char *)arg0;
    for (size_t i = 1; i <= count && arg0; i++) argv[i] = va_arg(ap, char *);
    if (envp_out) *envp_out = va_arg(ap, char **);
    return argv;
}

int execl(const char *path, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = gather(arg0, ap, NULL);
    va_end(ap);
    if (!argv) return -1;
    execv(path, argv);
    free(argv);
    return -1;
}

int execlp(const char *file, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = gather(arg0, ap, NULL);
    va_end(ap);
    if (!argv) return -1;
    execvp(file, argv);
    free(argv);
    return -1;
}

int execle(const char *path, const char *arg0, ...) {
    va_list ap;
    char **envp;
    va_start(ap, arg0);
    char **argv = gather(arg0, ap, &envp);
    va_end(ap);
    if (!argv) return -1;
    execve(path, argv, envp);
    free(argv);
    return -1;
}

pid_t getppid(void) {
    int me = getpid();
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++)
        if (pi.pid == me) return pi.ppid;
    return 0;
}
