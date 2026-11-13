// KTESTs for the boot-time apply of staged replacements.
#include "ktest.h"
#include "kapi.h"
#include "update_abi.h"
#include "pending_replace.h"

static const char *read_small(const char *path) {
    static char buf[64];
    return fs_read_into(path, buf, sizeof buf) ? buf : 0;
}

KTEST("update", "a pending list is applied, skips what it must, and is removed") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    // A REAL list belongs to the next boot; running it now would apply
    // somebody's update under a live desktop.
    if (fs_exists(UPDATE_PENDING_PATH)) KTEST_SKIP("a real update is pending");
    // Created only if absent, and REMOVED at the end if this test made
    // it: a directory left on the image is a layout change every later
    // check_layout run reports.
    int made_dir = !fs_exists("/var/lib/update");
    fs_mkdir("/var");
    fs_mkdir("/var/lib");
    fs_mkdir("/var/lib/update");
    fs_delete("/.ktest_pr_a");
    fs_delete("/.ktest_pr_b");
    KTEST_ASSERT(fs_write("/.ktest_pr_a", "old", 0) == 1);
    KTEST_ASSERT(fs_write("/.ktest_pr_a" UPDATE_STAGED_SUFFIX, "new", 0) == 1);
    // b has no staged file: already applied by a boot that lost power.
    KTEST_ASSERT(fs_write("/.ktest_pr_b", "kept", 0) == 1);
    // A relative name and a line naming a staged file are both refused
    // rather than guessed at.
    KTEST_ASSERT(fs_write(UPDATE_PENDING_PATH,
                          "/.ktest_pr_a\n/.ktest_pr_b\nrelative\n/.ktest_pr_a.upd\n", 0) == 1);

    fs_apply_pending_replacements();

    const char *a = read_small("/.ktest_pr_a");
    KTEST_ASSERT(a != 0 && k_strcmp(a, "new") == 0);
    KTEST_ASSERT_EQ(fs_exists("/.ktest_pr_a" UPDATE_STAGED_SUFFIX), 0);
    const char *b = read_small("/.ktest_pr_b");
    KTEST_ASSERT(b != 0 && k_strcmp(b, "kept") == 0);
    KTEST_ASSERT_EQ(fs_exists(UPDATE_PENDING_PATH), 0);

    fs_delete("/.ktest_pr_a");
    fs_delete("/.ktest_pr_b");
    if (made_dir) fs_delete("/var/lib/update");
}

KTEST("update", "a `-` line removes a file, and never a directory") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    if (fs_exists(UPDATE_PENDING_PATH)) KTEST_SKIP("a real update is pending");
    int made_dir = !fs_exists("/var/lib/update");
    fs_mkdir("/var");
    fs_mkdir("/var/lib");
    fs_mkdir("/var/lib/update");
    fs_delete("/.ktest_pr_gone");
    fs_delete("/.ktest_pr_dir");
    KTEST_ASSERT(fs_write("/.ktest_pr_gone", "stale", 0) == 1);
    KTEST_ASSERT(fs_mkdir("/.ktest_pr_dir"));
    // The file goes, the directory is refused, an absent one is done.
    KTEST_ASSERT(fs_write(UPDATE_PENDING_PATH,
                          "-/.ktest_pr_gone\n-/.ktest_pr_dir\n-/.ktest_pr_never\n", 0) == 1);

    fs_apply_pending_replacements();

    KTEST_ASSERT_EQ(fs_exists("/.ktest_pr_gone"), 0);
    KTEST_ASSERT(fs_is_dir("/.ktest_pr_dir"));
    KTEST_ASSERT_EQ(fs_exists(UPDATE_PENDING_PATH), 0);

    fs_delete("/.ktest_pr_dir");
    if (made_dir) fs_delete("/var/lib/update");
}

// AN EMPTY LIST IS NOTHING PENDING, and is removed: the updater refuses
// every update while a list exists ("restart to finish it first"), and an
// update that staged only a kernel -- or a crash between the list's create
// and its first line -- left one that no boot ever deleted.
KTEST("update", "an empty pending list is removed, not left to block every update") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    if (fs_exists(UPDATE_PENDING_PATH)) KTEST_SKIP("a real update is pending");
    int made_dir = !fs_exists("/var/lib/update");
    fs_mkdir("/var");
    fs_mkdir("/var/lib");
    fs_mkdir("/var/lib/update");
    KTEST_ASSERT(fs_write(UPDATE_PENDING_PATH, "", 0) == 1);
    KTEST_ASSERT(fs_exists(UPDATE_PENDING_PATH));

    fs_apply_pending_replacements();

    KTEST_ASSERT_EQ(fs_exists(UPDATE_PENDING_PATH), 0);
    if (made_dir) fs_delete("/var/lib/update");
}

// A LINE THAT CANNOT BE APPLIED MUST NOT KEEP THE LIST FOREVER. A target
// that is a directory never accepts a rename, so a kept list blocked every
// update for good. The list is set aside for a person to read; the next
// `update` compares every file and stages again whatever is still wrong.
KTEST("update", "a list with a line that cannot apply is set aside, not kept") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    if (fs_exists(UPDATE_PENDING_PATH)) KTEST_SKIP("a real update is pending");
    int made_dir = !fs_exists("/var/lib/update");
    fs_mkdir("/var");
    fs_mkdir("/var/lib");
    fs_mkdir("/var/lib/update");
    fs_delete(UPDATE_PENDING_FAILED_PATH);
    fs_delete("/.ktest_pr_d" UPDATE_STAGED_SUFFIX);
    fs_delete("/.ktest_pr_d");
    KTEST_ASSERT(fs_mkdir("/.ktest_pr_d"));
    KTEST_ASSERT(fs_write("/.ktest_pr_d" UPDATE_STAGED_SUFFIX, "new", 0) == 1);
    KTEST_ASSERT(fs_write(UPDATE_PENDING_PATH, "/.ktest_pr_d\n", 0) == 1);

    fs_apply_pending_replacements();

    KTEST_ASSERT_EQ(fs_exists(UPDATE_PENDING_PATH), 0);   // no longer blocking
    KTEST_ASSERT(fs_exists(UPDATE_PENDING_FAILED_PATH));  // ...but kept to be read
    KTEST_ASSERT(fs_is_dir("/.ktest_pr_d"));              // and nothing guessed at
    fs_delete(UPDATE_PENDING_FAILED_PATH);
    fs_delete("/.ktest_pr_d" UPDATE_STAGED_SUFFIX);
    fs_delete("/.ktest_pr_d");
    if (made_dir) fs_delete("/var/lib/update");
}

// A LAST LINE WITH NO NEWLINE IS A TORN WRITE, not an instruction: a cut
// mid-write leaves `-/bin/ls` of an intended `-/bin/lsof`, and applying it
// removes a file nobody named.
KTEST("update", "an unterminated last line is not applied") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    if (fs_exists(UPDATE_PENDING_PATH)) KTEST_SKIP("a real update is pending");
    int made_dir = !fs_exists("/var/lib/update");
    fs_mkdir("/var");
    fs_mkdir("/var/lib");
    fs_mkdir("/var/lib/update");
    fs_delete("/.ktest_pr_t");
    KTEST_ASSERT(fs_write("/.ktest_pr_t", "keep me", 0) == 1);
    KTEST_ASSERT(fs_write(UPDATE_PENDING_PATH, "-/.ktest_pr_t", 0) == 1);   // no newline

    fs_apply_pending_replacements();

    KTEST_ASSERT(fs_exists("/.ktest_pr_t"));
    fs_delete("/.ktest_pr_t");
    fs_delete(UPDATE_PENDING_PATH);
    fs_delete(UPDATE_PENDING_FAILED_PATH);
    if (made_dir) fs_delete("/var/lib/update");
}
