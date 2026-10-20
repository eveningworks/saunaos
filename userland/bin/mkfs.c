// mkfs -- put an empty filesystem on a partition.
//
//     mkfs [-t TYPE] PARTITION confirm
//
// The format runs on a second thread while this one draws its stage line
// from MKFS_PROGRESS (lib/ustageline.h, as `fsck` does). Arguments come
// through lib/uargs, as for every such program.
//
// THE INSTALLER'S OPERATION, and deliberately not `fsformat`. That one
// reformats the volume this machine is RUNNING FROM: it unmounts
// everything, formats, and re-probes the world. This one writes a
// filesystem onto some OTHER partition and disturbs nothing.
//
// It used to be unable to run at all: a backend kept its volume in
// module-level state, so formatting ANY device repointed whatever was
// mounted -- twice a crash that took /bin with it. format() and wipe()
// run on a scratch state now (fs_ops.h), and `/bin/install` is the
// caller this exists for.
//
// `confirm` IS A WORD YOU TYPE, not a flag, following `fsformat tfs3
// confirm` and SYS_MKPART's MKPART_CONFIRM. There is no privilege model
// in this OS to gate a destructive storage operation with, so the stand-
// in is that the person asking spells it out. It is a speed bump, and
// the ABI comment says so rather than letting anybody mistake it for a
// permission check.
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "rt/sys.h"
#include "mount_abi.h"
#include "lib/uargs.h"
#include "lib/ustageline.h"
#include <stdio.h>
#include <string.h>

#define REDRAW_MS 200

static const char *g_type = "tfs3";

static const struct uargs_opt OPTS[] = {
    { "type", 't', "TYPE", "the filesystem to write: tfs3 (the default) or fat32", 0, &g_type },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "mkfs",
    .usage = "[-t TYPE] PARTITION confirm",
    .summary = "Put an empty filesystem on PARTITION (as lsblk names it: ahci0p1),\n"
               "erasing what is on it. The word `confirm` says that is meant.",
    .opts = OPTS,
};

static struct mkfs_request g_req;
static int g_rc, g_err;
static volatile int g_done;

static void *worker(void *arg) {
    (void)arg;
    g_rc = sys_mkfs(&g_req);
    g_err = g_rc < 0 ? sys_errno() : 0;
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    const char *part = a.argc > 0 ? a.argv[0] : 0;
    if (!part) return uargs_error(&PROG, "which partition?");
    if (a.argc > 2 || (a.argc == 2 && strcmp(a.argv[1], "confirm")))
        return uargs_error(&PROG, "unexpected argument '%s'", a.argv[a.argc > 2 ? 2 : 1]);
    if (a.argc < 2) {
        printf("mkfs: this ERASES %s. Add the word `confirm` if that is "
               "what you want:\n    mkfs -t %s %s confirm\n", part, g_type, part);
        return 1;
    }

    memset(&g_req, 0, sizeof g_req);
    strlcpy(g_req.source, part, sizeof g_req.source);
    strlcpy(g_req.fstype, g_type, sizeof g_req.fstype);
    g_req.flags = MKFS_CONFIRM;

    pthread_t th;
    if (pthread_create(&th, 0, worker, 0) == 0) {
        int tty = isatty(2), width = 0;
        while (!__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
            usleep(REDRAW_MS * 1000);
            struct fs_check_progress p;
            if (tty && !sys_mkfs_progress(part, &p)) ustageline_draw("mkfs", &p, &width);
        }
        pthread_join(th, 0);
        ustageline_clear(&width);
    } else {
        worker(0);   // no thread: no line, same answer
    }

    if (g_rc < 0) {
        // THE REASON IS IN sys_errno(), NOT IN THE RETURN VALUE. Every
        // wrapper here converts a negative errno to -1 and stashes the
        // code (rt/sys.c's err()), so comparing the return against
        // -EINVAL matches nothing -- and -1 IS -EPERM, so a chain of
        // such comparisons reports every failure as the LAST thing it
        // happens to equal.
        int e = g_err;
        const char *why =
            e == EBUSY  ? "it is mounted -- unmount it first"
          : e == EINVAL ? "no such partition, or no such filesystem type"
          : e == EPERM  ? "refused without confirmation"
          : e == EIO    ? "the format itself failed -- see dmesg"
          : strerror(e);
        printf("mkfs: %s: %s\n", part, why);
        return 1;
    }
    printf("mkfs: %s is now an empty %s filesystem\n", part, g_type);
    return 0;
}
