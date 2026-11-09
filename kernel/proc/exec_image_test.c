// The image a spawn or an exec builds: read from the file AS IT LOADS,
// so a program bigger than any free run of frames still loads, and a
// failure says which errno it was rather than ENOENT for everything.
//
// The fixture is a real file in /tmp (ramfs): one read-only PT_LOAD
// whose every page starts with its own index, so a window refilled at
// the wrong offset is caught, and a file bigger than the kernel heap has
// free -- a whole-file buffer would have to grow the heap by one run
// that size, which the contiguous cap refuses.
#include "ktest.h"
#include "sched_internal.h"
#include "fault_inject.h"
#include "vmm.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "errno.h"

#define BIG_PATH  "/tmp/.ktest_bigexec"
#define IMAGE_VA  0x8000000000ULL
#define PAGE      4096u
#define CHUNK     (64u * 1024u)

struct x_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct x_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

// Writes a valid ELF of `pages` payload pages at file offset 4096. 1 = written.
static int write_fixture(uint32_t pages) {
    uint8_t *buf = kmalloc(CHUNK);
    if (!buf) return 0;
    k_memset(buf, 0, PAGE);
    struct x_ehdr *e = (struct x_ehdr *)buf;
    e->ident[0] = 0x7F; e->ident[1] = 'E'; e->ident[2] = 'L'; e->ident[3] = 'F';
    e->ident[4] = 2;
    e->type = 2; e->machine = 62; e->version = 1;
    e->entry = IMAGE_VA;
    e->phoff = sizeof *e;
    e->ehsize = sizeof *e;
    e->phentsize = sizeof(struct x_phdr);
    e->phnum = 1;
    struct x_phdr *p = (struct x_phdr *)(buf + sizeof *e);
    p->type = 1; p->flags = 4;   // PT_LOAD, read-only
    p->offset = PAGE;
    p->vaddr = IMAGE_VA;
    p->filesz = p->memsz = (uint64_t)pages * PAGE;
    p->align = PAGE;
    fs_delete(BIG_PATH);
    int ok = fs_write_range(BIG_PATH, 0, buf, PAGE) == 1;
    for (uint32_t pg = 0; ok && pg < pages; pg += CHUNK / PAGE) {
        uint32_t n = pages - pg < CHUNK / PAGE ? pages - pg : CHUNK / PAGE;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t *w = (uint32_t *)(buf + i * PAGE);
            for (uint32_t j = 0; j < PAGE / 4; j++) w[j] = (pg + i) ^ (j << 20);
        }
        ok = fs_write_range(BIG_PATH, PAGE + (uint64_t)pg * PAGE, buf, n * PAGE) == 1;
    }
    kfree(buf);
    return ok;
}

static int page_holds(uint64_t as, uint32_t pg) {
    uint64_t phys = vmm_user_phys(as, IMAGE_VA + (uint64_t)pg * PAGE);
    if (!phys) return 0;
    const uint32_t *w = (const uint32_t *)(uintptr_t)phys;
    return w[0] == pg && w[PAGE / 4 - 1] == (pg ^ ((PAGE / 4 - 1) << 20));
}

KTEST("exec", "a program bigger than the heap's free memory loads with no run over 64 frames") {
    // Bigger than what the heap has free, so a whole-file buffer could not
    // come from memory the heap already holds; at least 4 MiB.
    uint64_t spare = heap_total_bytes() - heap_used_bytes();
    uint32_t pages = (uint32_t)((spare + (4u << 20)) / PAGE);
    KTEST_ASSERT(write_fixture(pages));

    fault_cap_contiguous(64);   // 256 KiB: no run longer than the read window
    uint64_t as = 0;
    int rc = sched_test_build_image(BIG_PATH, &as);
    fault_cap_contiguous(0);
    int first = rc == 0 && page_holds(as, 0);
    int mid = rc == 0 && page_holds(as, pages / 2 + 3);
    int last = rc == 0 && page_holds(as, pages - 1);
    sched_test_drop_image(as);
    fs_delete(BIG_PATH);

    KTEST_ASSERT_EQ(rc, 0);
    KTEST_ASSERT(first);
    KTEST_ASSERT(mid);
    KTEST_ASSERT(last);
}

KTEST("exec", "an image that cannot be built says why: ENOENT, ENOEXEC") {
    uint64_t as = 0;
    KTEST_ASSERT_EQ(sched_test_build_image("/tmp/.ktest_no_such_program", &as), -ENOENT);
    KTEST_ASSERT_EQ(as, 0);

    fs_delete(BIG_PATH);
    KTEST_ASSERT(fs_write(BIG_PATH, "#!not an interpreter line, nor an ELF\n", 0));
    KTEST_ASSERT_EQ(sched_test_build_image(BIG_PATH, &as), -ENOEXEC);   // not an ELF
    fs_delete(BIG_PATH);
    KTEST_ASSERT(fs_touch(BIG_PATH));
    KTEST_ASSERT_EQ(sched_test_build_image(BIG_PATH, &as), -ENOEXEC);   // empty
    fs_delete(BIG_PATH);
}
