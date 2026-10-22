// Removable drives -- see lib/udrive.h.
#include "lib/udrive.h"
#include "lib/uchan.h"
#include "lib/human.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "mount_abi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// The volume `dev` is mounted at, into `point`; 1 when it is.
static int mounted_at(const char *dev, char *point, size_t cap, uint64_t *total, uint64_t *used) {
    struct query_fsinfo fs;
    for (unsigned i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_MOUNTED) || strcmp(fs.device, dev) != 0) continue;
        if (point) snprintf(point, cap, "%s", fs.point);
        if (total) *total = fs.total_bytes;
        if (used) *used = fs.used_bytes;
        return 1;
    }
    return 0;
}

int udrive_list(struct udrive **out) {
    *out = 0;
    int n = 0, cap = 0;
    struct udrive *v = 0;
    struct query_blkdev b;
    // A removable DISK's volumes: its partitions, or the disk itself when
    // nothing sits on it (QUERY_BLKDEV gives a partitioned disk no fstype).
    for (unsigned i = 0; sys_query_record(QUERY_BLKDEV, i, &b, sizeof b) > 0; i++) {
        if (!(b.fs_flags & QUERY_BLK_REMOVABLE)) continue;
        int is_disk = b.parent[0] == 0;
        if (is_disk) {
            // A disk with partitions is listed through them.
            struct query_blkdev c;
            int has_parts = 0;
            for (unsigned j = 0; sys_query_record(QUERY_BLKDEV, j, &c, sizeof c) > 0 && !has_parts; j++)
                has_parts = strcmp(c.parent, b.name) == 0;
            if (has_parts) continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            struct udrive *g = realloc(v, (size_t)cap * sizeof *v);
            if (!g) { free(v); return -1; }
            v = g;
        }
        struct udrive *d = &v[n++];
        memset(d, 0, sizeof *d);
        snprintf(d->disk, sizeof d->disk, "%s", is_disk ? b.name : b.parent);
        snprintf(d->dev, sizeof d->dev, "%s", b.name);
        snprintf(d->fstype, sizeof d->fstype, "%s", b.fstype);
        d->size = b.sectors * 512;
        d->mountable = (b.fs_flags & QUERY_BLK_FS_MOUNTABLE) != 0;
        d->no_automount = (b.fs_flags & QUERY_BLK_NO_AUTOMOUNT) != 0;
        // The model is the disk's: a partition carries none of its own.
        if (b.model[0]) snprintf(d->model, sizeof d->model, "%s", b.model);
        else {
            struct query_blkdev p;
            for (unsigned j = 0; sys_query_record(QUERY_BLKDEV, j, &p, sizeof p) > 0; j++)
                if (!strcmp(p.name, d->disk)) { snprintf(d->model, sizeof d->model, "%s", p.model); break; }
        }
        snprintf(d->name, sizeof d->name, "%s",
                 b.label[0] ? b.label : d->model[0] ? d->model : "USB drive");
        mounted_at(d->dev, d->point, sizeof d->point, &d->total, &d->used);
    }
    *out = v;
    return n;
}

int udrive_media_point(const struct udrive *d, char *out, size_t cap) {
    // What a path cannot hold, and what a shell without quoting trips on.
    char safe[64];
    size_t k = 0;
    for (const char *p = d->name; *p && k + 1 < sizeof safe; p++) {
        unsigned char c = (unsigned char)*p;
        safe[k++] = (c == '/' || c == ' ' || c < 0x20 || c == '\\') ? '-' : (char)c;
    }
    safe[k] = 0;
    if (!k || !strcmp(safe, ".") || !strcmp(safe, "..")) snprintf(safe, sizeof safe, "%s", d->dev);
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(out, cap, "/media/%s", safe);
        else snprintf(out, cap, "/media/%s-%d", safe, n);
        struct stat st;
        if (stat(out, &st) != 0) return 0;     // free
        // An empty folder nothing is mounted on is ours to reuse.
        struct query_fsinfo fs;
        int taken = 0;
        for (unsigned i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0 && !taken; i++)
            taken = (fs.flags & QUERY_FS_MOUNTED) && !strcmp(fs.point, out);
        if (!taken) return 0;
    }
    errno = EEXIST;
    return -1;
}

int udrive_mount(const struct udrive *d, char *point_out, size_t cap) {
    char point[64];
    if (udrive_media_point(d, point, sizeof point) < 0) return -1;
    mkdir("/media", 0755);
    if (mkdir(point, 0755) < 0 && errno != EEXIST) return -1;
    struct mount_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", d->dev);
    snprintf(req.point, sizeof req.point, "%s", point);
    if (sys_mount(&req) < 0) {
        int e = errno;
        rmdir(point);
        errno = e;
        return -1;
    }
    if (point_out) snprintf(point_out, cap, "%s", point);
    return 0;
}

// Is `device` the disk itself, or one of its partitions ("usb0p1")?
static int on_disk(const char *device, const char *disk) {
    size_t n = strlen(disk);
    if (strncmp(device, disk, n) != 0) return 0;
    return device[n] == 0 || (device[n] == 'p' && device[n + 1] >= '0' && device[n + 1] <= '9');
}

int udrive_eject(const char *disk) {
    // ASK FIRST: the kernel refuses a disk that is not removable before it
    // looks at what is mounted, so a mistaken name never unmounts /boot on
    // the way to a refusal.
    if (sys_eject(disk) == 0) return 0;
    if (errno != EBUSY) return -1;
    for (int guard = 0; guard < 64; guard++) {
        struct query_fsinfo fs;
        int found = 0;
        for (unsigned i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
            if (!(fs.flags & QUERY_FS_MOUNTED) || !on_disk(fs.device, disk)) continue;
            found = 1;
            char point[64];
            snprintf(point, sizeof point, "%s", fs.point);
            if (sys_umount(point) < 0) return -1;   // errno: EBUSY, a file is open
            if (!strncmp(point, "/media/", 7)) rmdir(point);
            break;
        }
        if (!found) break;
    }
    return sys_eject(disk);
}

void udrive_describe(const struct udrive *d, char *out, size_t cap) {
    char size[24], free_s[24];
    human_size_iec(size, sizeof size, d->point[0] && d->total ? d->total : d->size);
    if (!d->mountable) { snprintf(out, cap, "%s \xb7 nothing readable", size); return; }
    // As the filesystems are written, not as the kernel names them.
    const char *fs = !strcmp(d->fstype, "fat32") ? "FAT32" : !strcmp(d->fstype, "exfat") ? "exFAT"
                   : !strcmp(d->fstype, "tfs3") ? "TFS3" : d->fstype[0] ? d->fstype : "unknown";
    if (!d->point[0]) { snprintf(out, cap, "%s %s \xb7 not mounted", size, fs); return; }
    human_size_iec(free_s, sizeof free_s, d->used < d->total ? d->total - d->used : 0);
    snprintf(out, cap, "%s %s \xb7 %s free", size, fs, free_s);
}

int udrive_disk_of(const char *dev, char *disk, size_t cap) {
    struct query_blkdev b;
    for (unsigned i = 0; sys_query_record(QUERY_BLKDEV, i, &b, sizeof b) > 0; i++) {
        if (strcmp(b.name, dev) != 0) continue;
        snprintf(disk, cap, "%s", b.parent[0] ? b.parent : b.name);
        return 0;
    }
    return -1;
}

// ---- asking the service -----------------------------------------------

static struct uchan_client g_client;
static int g_client_open;

static int ask(uint32_t verb, const char *dev) {
    struct uchan_client *c = &g_client;
    if (g_client_open && !uchan_client_server_alive(c)) { uchan_client_close(c); g_client_open = 0; }
    if (!g_client_open && uchan_client_open(c, UDRIVE_SERVICE) < 0) return -1;
    g_client_open = 1;
    struct udrive_msg m;
    memset(&m, 0, sizeof m);
    m.verb = verb;
    snprintf(m.dev, sizeof m.dev, "%s", dev);
    return uchan_send(c, &m, sizeof m);
}

int udrive_ask_eject(const char *disk) { return ask(UDRIVE_EJECT, disk); }
int udrive_ask_mount(const char *dev) { return ask(UDRIVE_MOUNT, dev); }

int udrive_take_reply(struct udrive_msg *out) {
    if (!g_client_open || !uchan_client_recv(&g_client, out, sizeof *out)) return 0;
    out->dev[sizeof out->dev - 1] = 0;
    return 1;
}
