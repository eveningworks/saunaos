// Tests for config_file.c's registry, a fixed table that refused what
// did not fit without a word until 6fb721a.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: a capped table passes under
// its cap, so forty are registered on top of the built-ins; and one that
// freed its old array on growth would pass every lookup made after it,
// so a pointer taken before the growth is read again afterwards -- the
// old array is kept as memory to read, not kept in step.
#include "ktest.h"
#include "config_file.h"
#include "kfmt.h"
#include "string.h"

KTEST("config", "the registry grows past forty descriptors, and a rescan drops them") {
    enum { N = 40 };
    int before = config_file_count();
    const struct config_file *sys = config_file_find("system");
    char sys_path[CONFIG_PATH_MAX];
    k_strlcpy(sys_path, sys ? sys->path : "", sizeof sys_path);

    int added = 0;
    for (int i = 0; i < N; i++) {
        char name[CONFIG_NAME_MAX], path[CONFIG_PATH_MAX];
        k_snprintf(name, sizeof name, "ktestcf%d", i);
        k_snprintf(path, sizeof path, "/tmp/ktestcf%d.conf", i);
        added += config_file_register(name, path, "registry growth", 0);
    }
    const struct config_file *last = config_file_find("ktestcf39");
    int found_last = last && !k_strcmp(last->path, "/tmp/ktestcf39.conf");
    int count_after = config_file_count();
    // Taken before the table grew; its memory must still say the same.
    int old_ptr_ok = sys && !k_strcmp(sys->path, sys_path);

    config_files_scan();   // drops every non-built-in, then reads /etc/config.d
    int dropped = !config_file_find("ktestcf0") && !config_file_find("ktestcf39");

    KTEST_ASSERT(sys != 0);
    KTEST_ASSERT_EQ(added, N);
    KTEST_ASSERT_EQ(count_after, before + N);
    KTEST_ASSERT(found_last);
    KTEST_ASSERT(old_ptr_ok);
    KTEST_ASSERT(dropped);
}
