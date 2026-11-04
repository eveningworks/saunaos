// The image cache's tables (kernel/mm/imgcache.c), driven directly: no
// file is opened -- adopt() takes frames the test filled -- so a path
// only has to be one the cache's policy names. Frames are adopted
// UNLENT, which is what lets a forget free them and these tests leave
// the allocator as they found it.
#include "ktest.h"
#include "imgcache.h"
#include "pmm.h"
#include "string.h"

KTEST("imgcache", "the path policy: the system image only, spelled one way") {
    KTEST_ASSERT(imgcache_path_ok("/lib/libc.so"));
    KTEST_ASSERT(imgcache_path_ok("/bin/wm/apps/notepad"));
    KTEST_ASSERT(imgcache_path_ok("/tests/imgshare_test"));
    KTEST_ASSERT(!imgcache_path_ok("/home/user/a.out"));     // a user's build
    KTEST_ASSERT(!imgcache_path_ok("/library/x"));           // a prefix of a WORD is not one
    KTEST_ASSERT(!imgcache_path_ok("/bin//ls"));             // a second key for one file
    KTEST_ASSERT(!imgcache_path_ok("/bin/../home/x"));
    KTEST_ASSERT(!imgcache_path_ok("/lib/./libc.so"));
    KTEST_ASSERT(!imgcache_path_ok(0));
}

// 600 pages: past the first page table (256 buckets) twice over, and past
// the 512 frames the cache was once capped at.
#define GROW_PAGES 600

KTEST("imgcache", "the cache grows past its first tables, and finds every page again") {
    static const char path[] = "/tests/ktest_imgcache_grow";
    static uint64_t frames[GROW_PAGES];
    struct imgcache_stats before, during, after;
    imgcache_get_stats(&before);

    int got = 0;
    for (int i = 0; i < GROW_PAGES; i++) {
        frames[i] = pmm_alloc_frame(PMM_ZONE_DMA32);
        if (!frames[i]) break;
        *(uint64_t *)(uintptr_t)frames[i] = (uint64_t)i;
        if (imgcache_adopt(path, (uint64_t)i * 4096, frames[i], 0, imgcache_epoch()) != frames[i]) {
            pmm_free_frame(frames[i]);
            break;
        }
        got++;
    }
    KTEST_ASSERT_EQ(got, GROW_PAGES);
    imgcache_get_stats(&during);
    KTEST_ASSERT_EQ(during.frames, before.frames + GROW_PAGES);
    KTEST_ASSERT_EQ(during.files, before.files + 1);

    static uint64_t page[512];
    int found = 0;
    for (int i = 0; i < GROW_PAGES; i++)
        if (imgcache_copy(path, (uint64_t)i * 4096, page) && page[0] == (uint64_t)i) found++;
    KTEST_ASSERT_EQ(found, GROW_PAGES);
    KTEST_ASSERT(!imgcache_copy(path, (uint64_t)GROW_PAGES * 4096, page));   // and a miss misses

    // A second offer of a cached key answers the FIRST frame.
    uint64_t dup = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(dup != 0);
    KTEST_ASSERT_EQ(imgcache_adopt(path, 0, dup, 0, imgcache_epoch()), frames[0]);
    pmm_free_frame(dup);

    imgcache_forget(path);
    imgcache_get_stats(&after);
    KTEST_ASSERT_EQ(after.frames, before.frames);
    KTEST_ASSERT_EQ(after.files, before.files);
    KTEST_ASSERT_EQ(after.unhooked, before.unhooked);      // none was lent: all freed
}

KTEST("imgcache", "a forget during the fill refuses the stale offer") {
    static const char path[] = "/tests/ktest_imgcache_epoch";
    uint64_t f = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(f != 0);
    uint64_t epoch = imgcache_epoch();       // the filler reads the file...
    imgcache_forget(path);                    // ...a write lands meanwhile
    KTEST_ASSERT_EQ(imgcache_adopt(path, 0, f, 1, epoch), 0);
    KTEST_ASSERT_EQ(imgcache_lookup(path, 0), 0);
    pmm_free_frame(f);   // refused, so still the caller's
}

KTEST("imgcache", "forgetting a directory drops what is under it, not a sibling") {
    static const char *const paths[] = {
        "/tests/ktest_imgdir/a", "/tests/ktest_imgdir/sub/b", "/tests/ktest_imgdirx",
    };
    uint64_t f[3];
    for (int i = 0; i < 3; i++) {
        f[i] = pmm_alloc_frame(PMM_ZONE_DMA32);
        KTEST_ASSERT(f[i] != 0);
        KTEST_ASSERT_EQ(imgcache_adopt(paths[i], 0, f[i], 0, imgcache_epoch()), f[i]);
    }
    static uint64_t page[512];
    imgcache_forget("/tests/ktest_imgdir");
    KTEST_ASSERT(!imgcache_copy(paths[0], 0, page));
    KTEST_ASSERT(!imgcache_copy(paths[1], 0, page));
    KTEST_ASSERT(imgcache_copy(paths[2], 0, page));
    imgcache_forget(paths[2]);
    KTEST_ASSERT(!imgcache_copy(paths[2], 0, page));
}
