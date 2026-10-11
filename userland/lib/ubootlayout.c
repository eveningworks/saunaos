// See ubootlayout.h.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rt/sys.h"
#include "lib/ufileop.h"
#include "lib/ubootcfg.h"    // UBOOTCFG_MAX: refused rather than cut above it
#include "lib/ubootlayout.h"

#define LEGACY_CFG UBOOTLAYOUT_LEGACY "/grub/grub.cfg"

// ~30 KB -- file scope, like every other ufileop caller.
static struct ufileop g_op;
static struct sys_dirent g_ents[SYS_LISTDIR_MAX];

int ubootlayout_legacy(void) {
    struct stat st;
    return stat(LEGACY_CFG, &st) == 0;
}

const char *ubootlayout_dir(void) {
    return ubootlayout_legacy() ? UBOOTLAYOUT_LEGACY : UBOOTLAYOUT_DIR;
}

// `multiboot2 /boot/kernel.bin words` -> `multiboot2 /kernel.bin words`:
// a path GRUB resolves on the boot volume loses the `/boot` it only had
// because the files were one level down. Other lines are kept byte for
// byte -- the file is the machine's own (bootcfg, the Boot Manager).
static int flatten(const char *in, int len, char *out, int cap) {
    static const char *const CMDS[] = { "multiboot2", "multiboot", "module2", "linux", "initrd" };
    int o = 0;
    for (int i = 0; i < len; ) {
        int e = i;
        while (e < len && in[e] != '\n') e++;
        if (e < len) e++;
        int p = i;
        while (p < e && (in[p] == ' ' || in[p] == '\t')) p++;
        int cut = -1;
        for (unsigned k = 0; k < sizeof CMDS / sizeof CMDS[0]; k++) {
            int n = (int)strlen(CMDS[k]);
            if (e - p > n && !strncmp(in + p, CMDS[k], (size_t)n) &&
                (in[p + n] == ' ' || in[p + n] == '\t')) {
                int a = p + n;
                while (a < e && (in[a] == ' ' || in[a] == '\t')) a++;
                if (e - a > 6 && !strncmp(in + a, "/boot/", 6)) cut = a;
                break;
            }
        }
        for (int j = i; j < e; j++) {
            if (j == cut) j += 5;           // drop "/boot", keep the "/"
            if (o >= cap) return -1;
            out[o++] = in[j];
        }
        i = e;
    }
    return o;
}

static int flatten_file(const char *path) {
    static char in[UBOOTCFG_MAX], out[UBOOTCFG_MAX];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, in, sizeof in);
    char extra;
    int over = n == (long)sizeof in && read(fd, &extra, 1) == 1;
    close(fd);
    if (n < 0 || over) return -1;
    int m = flatten(in, (int)n, out, sizeof out);
    if (m < 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    int rc = write(fd, out, (size_t)m) == m && fsync(fd) == 0 ? 0 : -1;
    close(fd);
    return rc;
}

static const struct ufileop_policy QUIET = { 0 };

// The names in /boot/boot worth carrying: everything but `.`/`..`, and
// never a `boot` inside it, which would be copied onto itself.
static int legacy_names(void) {
    int n = sys_listdir(UBOOTLAYOUT_LEGACY, g_ents, SYS_LISTDIR_MAX);
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *s = g_ents[i].name;
        if (!strcmp(s, ".") || !strcmp(s, "..") || !strcmp(s, "boot")) continue;
        g_ents[k++] = g_ents[i];
    }
    return n < 0 ? -1 : k;
}

int ubootlayout_copy_up(const char **why) {
    int n = legacy_names();
    if (n < 0) { *why = "cannot list " UBOOTLAYOUT_LEGACY; return -1; }
    for (int i = 0; i < n; i++) {
        char src[96], dst[96];
        snprintf(src, sizeof src, UBOOTLAYOUT_LEGACY "/%s", g_ents[i].name);
        snprintf(dst, sizeof dst, UBOOTLAYOUT_DIR "/%s", g_ents[i].name);
        if (ufileop_copy(src, dst, &g_op, &QUIET) != UFILEOP_OK) {
            *why = "copying " UBOOTLAYOUT_LEGACY " up to /boot failed";
            return -1;
        }
    }
    // The .bak too: `bootcfg undo` would otherwise swap the old paths back in.
    struct stat st;
    if (flatten_file(UBOOTLAYOUT_DIR "/grub/grub.cfg") < 0 ||
        (stat(UBOOTLAYOUT_DIR "/grub/grub.cfg.bak", &st) == 0 &&
         flatten_file(UBOOTLAYOUT_DIR "/grub/grub.cfg.bak") < 0)) {
        *why = "could not point grub.cfg's kernels at /boot";
        return -1;
    }
    sys_sync();
    return 0;
}

void ubootlayout_drop_flat(void) {
    int n = legacy_names();
    for (int i = 0; i < n; i++) {
        char dst[96];
        snprintf(dst, sizeof dst, UBOOTLAYOUT_DIR "/%s", g_ents[i].name);
        ufileop_remove(dst, 1, &g_op, &QUIET);
    }
    sys_sync();
}

int ubootlayout_drop_legacy(const char **why) {
    if (ufileop_remove(UBOOTLAYOUT_LEGACY, 1, &g_op, &QUIET) != UFILEOP_OK) {
        *why = "could not remove " UBOOTLAYOUT_LEGACY;
        return -1;
    }
    sys_sync();
    return 0;
}
