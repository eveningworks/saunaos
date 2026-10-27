// automountd -- USB sticks, mounted as they arrive and tidied as they go.
//
// udisks2's job on a Linux desktop, at SaunaOS's size: a volume on a
// removable disk that this system can read is mounted at /media/<name>
// when it appears; a pulled stick's mount is unmounted and its folder
// removed; and the desktop's Eject is done HERE (lib/udrive.h's channel,
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
#include "lib/uchan.h"
#include "lib/udrive.h"
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
static void tidy_gone(void) {
    struct query_fsinfo fs;
    char (*points)[64] = 0;
    int n = 0, cap = 0;
    for (unsigned i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if ((fs.flags & (QUERY_FS_MOUNTED | QUERY_FS_GONE)) != (QUERY_FS_MOUNTED | QUERY_FS_GONE))
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
        if (seen(d->dev)) continue;
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

static void serve(void) {
    uchan_server_scan(&g_chan, 0, 0);
    struct udrive_msg m;
    int from;
    memset(&m, 0, sizeof m);
    while ((from = uchan_server_recv(&g_chan, &m, sizeof m)) != 0) {
        m.dev[sizeof m.dev - 1] = 0;
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
        } else {
            reply.result = -EINVAL;
        }
        uchan_server_reply(&g_chan, from, &reply, sizeof reply);
        memset(&m, 0, sizeof m);
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
