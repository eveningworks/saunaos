// mount -- attach a filesystem at a path, or list what is attached.
//
// With no arguments it LISTS, which is what `mount` does on every Unix
// and is the form people actually type. The list is read from
// QUERY_FSINFO, the same record `df` formats differently -- one fact,
// two views, and no syscall of its own for listing.
//
// THE SOURCE IS A PARTITION NUMBER, NOT A PATH. toy-os has no /dev, so
// there is nothing to name a volume with; `mount 2 /boot` means "the
// second partition of the boot disk". `parttable` prints the numbers.
// A backend that needs no volume is named directly instead
// (`mount -t ramfs none /mnt`), which is how a scratch filesystem is
// made. See abi/mount_abi.h.
//
// A NETWORK SHARE is `-t smb //server/share`: there is no kernel SMB
// client, so this starts the process that is one (`smbfsd --url`), which
// mounts the point itself. Saved shares are /etc/mounts.d's, not this.
//
// WHAT IT DELIBERATELY DOES NOT DO: no /etc/fstab, no `-a`, no
// remount-in-place, no per-filesystem option string. The first three
// want a config file this OS does not have; the fourth wants options no
// backend here has. To change a mount's flags: `umount` it and mount it
// again, which is two commands and no new mechanism.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include "knum.h"
#include <stdio.h>
#include <string.h>

static const char *USAGE =
    "mount [-r] [-t <fstype>] [-o size=<n>[K|M|G]] <device|partition|none> <mountpoint>\n"
    "       mount -t smb [-o user=<name>] //<server>/<share> <mountpoint>\n"
    "       mount                  list what is mounted";

// `-o size=` and nothing else. Linux's -o is a comma-separated string a
// filesystem parses for itself; this takes ONE option because there is
// one, and a parser for a list of one would be the second
// implementation of nothing (abi/mount_abi.h makes the same argument
// about the ABI). Returns MiB, or -1 if it is not a size.
//
// Rounds UP to a whole MiB, which is the ABI's unit: `-o size=1K` is a
// mount somebody asked to be small, and answering 0 would silently mean
// "the backend decides" -- the opposite.
static long parse_size_mib(const char *opt) {
    if (strncmp(opt, "size=", 5) != 0) return -1;
    uint64_t total;
    if (!k_parse_size(opt + 5, 1024 * 1024, &total)) return -1;   // a bare number is MiB
    unsigned long long mib = (total + (1024 * 1024 - 1)) / (1024 * 1024);
    if (mib > 0xFFFFFFFFULL) return -1;
    return (long)mib;
}

static int list(void) {
    struct query_fsinfo fs;
    char line[160];
    snprintf(line, sizeof line, "%-8s %-14s %-8s %s\n",
             "device", "on", "type", "options");
    sys_print(line);

    for (int i = 0; ; i++) {
        int n = sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs);
        if (n <= 0) break;
        if (!(fs.flags & QUERY_FS_MOUNTED)) continue;
        if (fs.flags & QUERY_FS_REMOTE) {
            // A share: its source instead of a device, and what its
            // server last said rather than a durability that means nothing.
            static const char *const st[] = { "", "idle", "connecting", "connected",
                                              "sign-in needed", "unreachable", "failed", "no server" };
            uint64_t s = fs.remote_state < 8 ? fs.remote_state : 0;
            snprintf(line, sizeof line, "%-8s %-14s %-8s ro,%s\n",
                     fs.source, fs.point, fs.name, st[s]);
        } else {
            snprintf(line, sizeof line, "%-8s %-14s %-8s %s%s\n",
                     fs.device[0] ? fs.device : "-", fs.point, fs.name,
                     (fs.flags & QUERY_FS_RDONLY) ? "ro" : "rw",
                     (fs.flags & QUERY_FS_PERSISTENT) ? "" : ",volatile");
        }
        sys_print(line);
    }
    return 0;
}

// `-t smb`: start a worker for the share and wait (briefly) for its
// mount to appear -- it mounts at once and connects on first use.
static int mount_smb(const char *url, const char *point, const char *user) {
    char args[512];
    int n = snprintf(args, sizeof args, "--url %s %s%s%s", url, point,
                     user ? " --user " : "", user ? user : "");
    if (n >= (int)sizeof args || strchr(url, ' ') || strchr(point, ' ')) {
        cmd_usage(USAGE);
        return 1;
    }
    int pid = sys_spawn("/bin/smbfsd", args, -1);
    if (pid < 0) { cmd_fail("mount", "/bin/smbfsd"); return 1; }
    for (int tries = 0; tries < 30; tries++) {
        struct query_fsinfo fs;
        for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++)
            if ((fs.flags & QUERY_FS_REMOTE) && !strcmp(fs.point, point)) return 0;
        int code;
        if (sys_waitpid_nohang(pid, &code) == pid) break;   // it gave up, and said why
        sys_sleep_ms(100);
    }
    sys_eprint("mount: the share was not mounted (see above)\n");
    return 1;
}

int main(int argc, char **argv) {
    struct mount_request req;
    memset(&req, 0, sizeof req);
    const char *user = NULL;

    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            req.flags |= SYS_MNT_RDONLY;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) { cmd_usage(USAGE); return 1; }
            snprintf(req.fstype, sizeof req.fstype, "%s", argv[i]);
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { cmd_usage(USAGE); return 1; }
            if (strncmp(argv[i], "user=", 5) == 0) { user = argv[i] + 5; continue; }
            long mib = parse_size_mib(argv[i]);
            if (mib < 0) { cmd_usage(USAGE); return 1; }
            req.size_mib = (unsigned)mib;
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }

    if (i == argc) return list();
    if (argc - i != 2) { cmd_usage(USAGE); return 1; }
    if (strcmp(req.fstype, "smb") == 0) return mount_smb(argv[i], argv[i + 1], user);
    if (user) { cmd_usage(USAGE); return 1; }    // user= is a share's option only

    // `none` is the conventional Unix spelling for "this filesystem has
    // no volume", and it reads better in a command line than an empty
    // argument would.
    if (strcmp(argv[i], "none") != 0) {
        snprintf(req.source, sizeof req.source, "%s", argv[i]);
    }
    snprintf(req.point, sizeof req.point, "%s", argv[i + 1]);

    if (sys_mount(&req) < 0) {
        cmd_fail("mount", argv[i + 1]);
        return 1;
    }
    return 0;
}
