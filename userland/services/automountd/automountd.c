// automountd -- USB sticks, mounted as they arrive and tidied as they go;
// disc images, mounted when a program asks.
//
// udisks2's job on a Linux desktop, at SaunaOS's size: a volume on a
// removable disk that this system can read is mounted at /media/<name>
// when it appears; a pulled stick's mount is unmounted and its folder
// removed; and the desktop's Eject is done HERE (base/udrive.h's channel,
// "drives"), because unmounting and ejecting wait on the disk and the
// compositor must never block.
//
// ONLY A VOLUME IT HAS NOT SEEN IS MOUNTED. A stick a person unmounted
// by hand stays unmounted until it is plugged in again -- the service
// mounts arrivals, it does not insist.
//
// It wakes on device changes through its wakeword (SYS_DEV_WATCH), the
// same word the channel wakes, so one wait serves both.
#include "rt/sys.h"
#include "query_abi.h"
#include "sandbox_abi.h"
#include "proc_info.h"
#include "base/uchan.h"
#include "base/udrive.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct uchan_server g_chan;
// The volumes seen while plugged in, by device name -- so a replug, which
// is a new device, mounts again and a manual unmount is respected.
static char (*g_seen)[16];
static int g_nseen, g_capseen;

static int seen(const char *dev) {
    for (int i = 0; i < g_nseen; i++) if (!strcmp(g_seen[i], dev)) return 1;
    return 0;
}

static void remember(const char *dev) {
    if (g_nseen == g_capseen) {
        int cap = g_capseen ? g_capseen * 2 : 8;
        char (*g)[16] = realloc(g_seen, (size_t)cap * sizeof *g_seen);
        if (!g) return;
        g_seen = g;
        g_capseen = cap;
    }
    snprintf(g_seen[g_nseen++], 16, "%s", dev);
}

// Pulled sticks' mounts: each unmounted and its /media folder removed.
// One a file still holds stays, and is tried again on the next wake --
// the backstop timeout included, since closing a file raises no event.
// A disk that opted out of automounting is its owner's to unmount too:
// tidying it took a KTEST's mount out from under it, so its next write
// landed in the directory beneath (udisks likewise cleans up only what it
// manages, and the kernel keeps a pulled disk's other mounts).
static void tidy_gone(void) {
    struct query_fsinfo fs;
    char (*points)[64] = 0;
    int n = 0, cap = 0;
    for (unsigned i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if ((fs.flags & (QUERY_FS_MOUNTED | QUERY_FS_GONE)) != (QUERY_FS_MOUNTED | QUERY_FS_GONE) ||
            (fs.flags & QUERY_FS_NO_AUTOMOUNT))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            char (*g)[64] = realloc(points, (size_t)cap * sizeof *points);
            if (!g) break;
            points = g;
        }
        snprintf(points[n++], sizeof points[0], "%s", fs.point);
    }
    // Collected first: an unmount renumbers the records being walked.
    for (int i = 0; i < n; i++) {
        udrive_eject_images_under(points[i]);   // their files went with the stick
        if (sys_umount(points[i]) != 0) continue;
        printf("automountd: %s was pulled out -- unmounted\n", points[i]);
        if (!strncmp(points[i], "/media/", 7)) rmdir(points[i]);
    }
    free(points);
}

// Mounts every volume that has appeared since the last pass, and forgets
// the ones that have gone.
static void reconcile(void) {
    tidy_gone();
    struct udrive *v;
    int n = udrive_list(&v);
    if (n < 0) return;
    // Forget what is no longer plugged in, so plugging it back mounts it.
    for (int i = 0; i < g_nseen;) {
        int still = 0;
        for (int k = 0; k < n && !still; k++) still = !strcmp(v[k].dev, g_seen[i]);
        if (still) { i++; continue; }
        memmove(g_seen[i], g_seen[i + 1], (size_t)(g_nseen - i - 1) * sizeof *g_seen);
        g_nseen--;
    }
    for (int k = 0; k < n; k++) {
        struct udrive *d = &v[k];
        if (d->image || seen(d->dev)) continue;
        remember(d->dev);
        if (d->point[0] || !d->mountable || d->no_automount) continue;
        char point[64];
        if (udrive_mount(d, point, sizeof point) == 0)
            printf("automountd: %s (%s) mounted at %s\n", d->name, d->dev, point);
        else
            printf("automountd: %s (%s) would not mount: %s\n", d->name, d->dev, strerror(errno));
    }
    free(v);
}

// THE PATH EACH CLIENT IS SENDING, joined from its PATH_PART pieces
// (base/udrive.h): grown to fit, up to a path's bound.
#define PATH_LIMIT 4096                          // FS_PATH_MAX: what a path may be
static struct { int pid; char *s; size_t cap; } *g_paths;
static int g_npaths;

static char *path_of(int pid) {
    for (int i = 0; i < g_npaths; i++) if (g_paths[i].pid == pid) return g_paths[i].s;
    return 0;
}

static void path_part(int pid, const struct udrive_path_part *p) {
    int i = 0;
    while (i < g_npaths && g_paths[i].pid != pid) i++;
    if (i == g_npaths) {
        void *g = realloc(g_paths, sizeof *g_paths * (size_t)(g_npaths + 1));
        if (!g) return;
        g_paths = g;
        g_paths[g_npaths++] = (typeof(*g_paths)){ pid, 0, 0 };
    }
    size_t end = (size_t)p->offset + UDRIVE_PART_BYTES;
    if (end > PATH_LIMIT) return;
    if (end + 1 > g_paths[i].cap) {
        char *g = realloc(g_paths[i].s, end + 1);
        if (!g) return;
        g_paths[i].s = g;
        g_paths[i].cap = end + 1;
    }
    if (p->offset == 0) memset(g_paths[i].s, 0, g_paths[i].cap);
    memcpy(g_paths[i].s + p->offset, p->part, UDRIVE_PART_BYTES);
    g_paths[i].s[end] = 0;
}

// A DISC IMAGE IS MOUNTED WITH THIS SERVICE'S REACH, not the asker's: a
// sandboxed process (unveil, pledge) could otherwise read a file it cannot
// see, through the mount. So only an unsandboxed process may ask, which
// is what SYS_MOUNT itself demands (PROMISE_ADMIN) -- the File Manager,
// Retro Launcher, w16 before its sandbox closes.
static int unsandboxed(int pid) {
    struct proc_info info;
    for (int i = 0; sys_proc_info(i, &info) == 0; i++)
        if (info.pid == pid) return info.promises == PROMISE_ALL;
    return 0;
}

static void serve(void) {
    uchan_server_scan(&g_chan, 0, 0);
    union { struct udrive_msg m; struct udrive_path_part p; } in;
    struct udrive_msg m;
    int from;
    memset(&in, 0, sizeof in);
    while ((from = uchan_server_recv(&g_chan, &in, sizeof in)) != 0) {
        if (in.m.verb == UDRIVE_PATH_PART) {
            path_part(from, &in.p);
            memset(&in, 0, sizeof in);
            continue;
        }
        m = in.m;
        m.dev[sizeof m.dev - 1] = 0;
        const char *path = path_of(from);
        struct udrive_msg reply;
        memset(&reply, 0, sizeof reply);
        reply.verb = m.verb;
        memcpy(reply.dev, m.dev, sizeof reply.dev);
        if (m.verb == UDRIVE_EJECT) {
            reply.result = udrive_eject(m.dev) == 0 ? 0 : -errno;
            printf("automountd: eject %s: %s\n", m.dev,
                   reply.result ? strerror(-reply.result) : "done, it can be pulled out");
        } else if (m.verb == UDRIVE_MOUNT) {
            struct udrive *v;
            int n = udrive_list(&v);
            reply.result = -ENOENT;
            for (int k = 0; k < n; k++) {
                if (strcmp(v[k].dev, m.dev)) continue;
                reply.result = v[k].point[0] ? 0 : udrive_mount(&v[k], 0, 0) == 0 ? 0 : -errno;
            }
            if (n > 0) free(v);
        } else if (m.verb == UDRIVE_MOUNT_IMAGE) {
            char point[64] = "";
            reply.result = !path ? -EINVAL : !unsandboxed(from) ? -EPERM
                         : udrive_mount_image(path, point, sizeof point) == 0 ? 0 : -errno;
            printf("automountd: disc image %s: %s%s\n", path ? path : "(no path sent)",
                   reply.result ? strerror(-reply.result) : "mounted at ", reply.result ? "" : point);
        } else if (m.verb == UDRIVE_EJECT_IMAGE) {
            reply.result = !path ? -EINVAL : !unsandboxed(from) ? -EPERM
                         : udrive_eject_image(path) == 0 ? 0 : -errno;
        } else {
            reply.result = -EINVAL;
        }
        // TO THE INBOX, where udrive_take_reply() reads: uchan_server_reply()
        // fills the slot only uchan_call() waits on, and no answer arrived.
        uchan_server_send(&g_chan, from, &reply, sizeof reply);
        memset(&in, 0, sizeof in);
    }
}

int main(void) {
    if (uchan_server_open(&g_chan, UDRIVE_SERVICE) < 0) {
        printf("automountd: the \"%s\" channel is taken -- is another automountd running?\n",
               UDRIVE_SERVICE);
        return 1;
    }
    unsigned last = (unsigned)sys_dev_watch(1);
    reconcile();
    for (;;) {
        // The 5 s ceiling is a backstop, not the mechanism: a device
        // change bumps the wakeword, which ends the wait at once.
        uchan_server_wait(&g_chan, 5000);
        serve();
        unsigned now = (unsigned)sys_dev_watch(2);
        if (now != last) {
            last = now;
            reconcile();
        } else {
            tidy_gone();   // one a busy file held before may be free now
        }
    }
}
