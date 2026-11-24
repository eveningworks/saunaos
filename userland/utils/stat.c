// stat -- what the filesystem knows about one path.
//
// The inode line says whether the number is REAL or synthetic, because
// on a backend without inodes it is the record's table slot: stable, and
// meaningless to compare against anything but another stat of the same
// volume. Saying which is the difference between a fact and a number.
#include "rt/sys.h"
#include <time.h>
#include "base/cmd.h"
#include "base/udate.h"
#include <stdio.h>
#include <locale.h>
#include <pwd.h>
#include <grp.h>

// In the locale's spelling, local time -- what the kernel stores is a
// UTC epoch (api/fs.h), and base/udate.h converts.
static void put_time(const char *label, const struct rtc_time *t) {
    char when[48], buf[80];
    udate_format(when, sizeof when, t, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
    snprintf(buf, sizeof buf, "  %s%s\n", label, when);
    sys_print(buf);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (argc != 2) {
        cmd_usage("stat <path>");
        return 1;
    }
    struct sys_statx sx;
    if (sys_statx(argv[1], &sx) < 0) {
        cmd_fail("stat", argv[1]);
        return 1;
    }

    const struct sys_stat st = sx.st;
    char buf[160];
    sys_print("  path:     "); sys_print(argv[1]); sys_print("\n");
    sys_print("  type:     ");
    sys_print(st.is_dir ? "directory\n" : "file\n");
    if (!st.is_dir) {
        snprintf(buf, sizeof buf, "  size:     %llu bytes\n",
                 (unsigned long long)st.size);
        sys_print(buf);
    }
    // Permission bits in octal, and whether they came off the DISK or
    // are the default for the type -- the same distinction the inode
    // line below draws, and for the same reason: a reader has to be
    // able to tell a fact from a fallback.
    snprintf(buf, sizeof buf, "  mode:     %04o%s\n", st.mode & 07777,
             (st.flags & SYS_STAT_MODE) ? "" : " (default -- this "
                                               "filesystem stores none)");
    sys_print(buf);
    // The owner by name where the account has one; a volume without
    // owners (FAT, a share) reports whoever it was mounted for.
    {
        struct passwd *pw = getpwuid(sx.uid);
        struct group *gr = getgrgid(sx.gid);
        snprintf(buf, sizeof buf, "  owner:    %u (%s)  group: %u (%s)\n", (unsigned)sx.uid,
                 pw ? pw->pw_name : "no name", (unsigned)sx.gid, gr ? gr->gr_name : "no name");
        sys_print(buf);
    }
    if (st.nlink > 1) {
        snprintf(buf, sizeof buf, "  links:    %u\n", (unsigned)st.nlink);
        sys_print(buf);
    }
    snprintf(buf, sizeof buf, "  inode:    %llu%s\n", (unsigned long long)st.ino,
             (st.flags & SYS_STAT_INODES) ? "" : " (synthetic)");
    sys_print(buf);
    put_time("created:  ", &st.created);
    put_time("modified: ", &st.modified);
    return 0;
}
