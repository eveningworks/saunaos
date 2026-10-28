// Path watches: which change fires which watch. The delivery half (the
// event reaching the compositor) is the GUI suite's -- calendar_test and
// shortcut_test change config and assert the desktop adopted it.
#include "ktest.h"
#include "fswatch.h"
#include "fs.h"
#include "errno.h"
#include "klimit.h"
#include "klog.h"
#include "kfmt.h"

// A pid no process holds, so these watches are this test's alone.
#define TEST_PID 0x7ff0

KTEST("fswatch", "a parent is the path up to its last slash, and / for a top-level name") {
    uint64_t self, parent, root, etc, etc2;
    fswatch_hash("/", &root, 0);
    fswatch_hash("/etc", &etc, &parent);
    KTEST_ASSERT_EQ(parent, root);
    fswatch_hash("/etc/desktop.conf", &self, &parent);
    KTEST_ASSERT_EQ(parent, etc);
    fswatch_hash("/etc/", &etc2, 0);          // a trailing slash is ignored
    KTEST_ASSERT_EQ(etc2, etc);
}

// **THE POINT OF THE WHOLE MECHANISM**: a write in /var/tmp must not
// wake a watcher of /etc -- the global counter it replaced did exactly
// that, and the compositor re-read its configs on every write in the
// machine. The /etc write beside it is the control.
KTEST("fswatch", "a change fires the watch on its directory, and no other") {
    int etc = fswatch_add(TEST_PID, "/etc");
    int eff = fswatch_add(TEST_PID, "/etc/effects");
    if (etc <= 0 || eff <= 0) { fswatch_owner_gone(TEST_PID); KTEST_SKIP("watch table full"); }

    uint64_t self, parent;
    fswatch_hash("/var/tmp/bench.dat", &self, &parent);
    fswatch_note(self, parent);
    uint32_t after_var = fswatch_fires(etc) + fswatch_fires(eff);

    fswatch_hash("/etc/desktop.conf", &self, &parent);
    fswatch_note(self, parent);
    uint32_t etc_fires = fswatch_fires(etc), eff_fires = fswatch_fires(eff);

    fswatch_hash("/etc/effects/shatter.conf", &self, &parent);
    fswatch_note(self, parent);
    uint32_t eff_after = fswatch_fires(eff), etc_after = fswatch_fires(etc);
    fswatch_owner_gone(TEST_PID);

    KTEST_ASSERT_EQ(after_var, 0);   // <- the point
    KTEST_ASSERT_EQ(etc_fires, 1);
    KTEST_ASSERT_EQ(eff_fires, 0);
    KTEST_ASSERT_EQ(eff_after, 1);   // a grandchild of /etc is not /etc's
    KTEST_ASSERT_EQ(etc_after, 1);
}

KTEST("fswatch", "the same path twice is one watch, the table grows past 16, and its ceiling refuses, logged") {
    enum { N = 100 };
    int a = fswatch_add(TEST_PID, "/usr/wm/applications");
    int b = fswatch_add(TEST_PID, "/usr/wm/applications/");
    char path[24];
    int made = 0, top = 0, last = 0;
    for (int i = 0; i < N; i++) {
        k_snprintf(path, sizeof path, "/w/%d", i);
        int id = fswatch_add(TEST_PID, path);
        if (id <= 0) break;
        if (id > top) top = id;
        last = id;
        made++;
    }
    // Each of the hundred fires on its own path: the grown slots are
    // walked, not just the first sixteen.
    uint64_t self, parent;
    fswatch_hash("/w/99", &self, &parent);
    fswatch_note(self, parent);
    uint32_t fired = fswatch_fires(last);

    // The ceiling, lowered to what is live now: the next one is refused.
    uint32_t was = klimit_test_set(&fswatch_limit, 1);
    uint64_t mark = klog_total_bytes();
    int past = fswatch_add(TEST_PID, "/w/past");
    int logged = klimit_test_logged(&fswatch_limit, mark);
    int same = fswatch_add(TEST_PID, "/w/0");   // an existing watch is not a new one
    klimit_test_set(&fswatch_limit, was);

    fswatch_owner_gone(TEST_PID);
    int again = fswatch_add(TEST_PID, "/usr/wm/applications");
    fswatch_owner_gone(TEST_PID);

    KTEST_ASSERT(a > 0);
    KTEST_ASSERT_EQ(b, a);
    KTEST_ASSERT_EQ(made, N);
    KTEST_ASSERT(top > 16);
    KTEST_ASSERT_EQ(fired, 1);
    KTEST_ASSERT_EQ(past, -ENOSPC);
    KTEST_ASSERT(logged);
    KTEST_ASSERT(same > 0);
    KTEST_ASSERT(again > 0);          // owner_gone freed every slot
}

// End to end through vfs.c: a real write fires, and a refused one (a
// path under a directory that does not exist) does not.
KTEST("fswatch", "vfs.c reports a successful write and not a failed one") {
    int w = fswatch_add(TEST_PID, "/tmp");
    if (w <= 0) { fswatch_owner_gone(TEST_PID); KTEST_SKIP("watch table full"); }
    int ok = fs_write("/tmp/.ktest_fswatch", "x", 0);
    uint32_t after_ok = fswatch_fires(w);
    int bad = fs_write("/tmp/.no_such_dir/x", "x", 0);
    uint32_t after_bad = fswatch_fires(w);
    fs_delete("/tmp/.ktest_fswatch");
    fswatch_owner_gone(TEST_PID);

    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(after_ok, 1);
    KTEST_ASSERT(!bad);
    KTEST_ASSERT_EQ(after_bad, 1);
}
