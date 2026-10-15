// clipboardd -- the system clipboard, as a ring-3 service.
//
// WHY IT EXISTS. The clipboard used to be a buffer inside
// kernel/proc/win_server.c, reached by a syscall. That put untrusted
// user data and a piece of desktop POLICY -- what a clipboard is, what
// a cut means, how big one may be -- in ring 0, which is the one thing
// CLAUDE.md says ring 0 must not contain. Every system that has thought
// about it agrees: macOS has `pboard`, Android's ClipboardManager lives
// in system_server, and Windows' copy-into-system-memory design is the
// outlier that sat in win32k.sys and has been moving out ever since.
//
// The argument that kept it in the kernel was LIFETIME: the compositor
// is a process this OS kills on purpose, and a clipboard a Force Quit
// could empty would be a poor one. Putting it in a SUPERVISED service
// answers that -- the clipboard outlives every program that copied into
// it and outlives the compositor, which is the case that mattered. It
// does NOT outlive this process: a restart zeroes a fresh page, the
// same way an X server dying loses a clipboard.
//
// WHAT IT ACTUALLY DOES, WHICH IS LESS THAN A SERVER. It does not carry
// bytes. It creates the shared page, owns its lifetime, and breaks a
// lock whose owner died; clients read and write the page themselves
// (lib/uclip.c), so a paste costs no syscall and no context switch and
// a menu item can watch the serial every frame for nothing. That is the
// opposite of X11 and Wayland, where the SOURCE app serves the bytes on
// demand -- which is exactly why closing the app you copied from loses
// your clipboard there, and why every desktop ships a clipboard manager
// to paper over it.
//
// IT BLOCKS ON TIME, NEVER ON ITS CLIENTS. There is nothing to serve,
// so the loop exists to notice a stuck lock -- and to delete the files of
// pictures no clipboard names any more (CLIP_IMAGE_DIR).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/uclip_page.h"
#include "syscall_abi.h"

#define TICK_MS 1000

static volatile struct clip_page *g_page;

// One place, so a machine with a stuck clipboard says so in the log
// rather than simply not copying.
static void break_stale_lock(void) {
    uint32_t held = __atomic_load_n(&g_page->lock, __ATOMIC_ACQUIRE);
    if (!held) return;

    uint64_t now = sys_monotonic_ns() / 1000000ull;
    uint64_t taken = g_page->lock_ms;
    if (taken == 0 || now < taken || now - taken < CLIP_LOCK_STALE_MS) return;

    // A SEQ LEFT ODD is a half-written clipboard, and every reader
    // retries against it forever. Put it back to even first, then
    // release the lock -- the other order lets a writer in while
    // readers are still being told the page is mid-update.
    uint32_t seq = __atomic_load_n(&g_page->seq, __ATOMIC_ACQUIRE);
    if (seq & 1u) {
        // The payload it was midway through is not trustworthy, so the
        // clipboard becomes EMPTY rather than half of something.
        g_page->op = CLIP_OP_NONE;
        g_page->kind = CLIP_KIND_FILES;
        g_page->count = 0;
        g_page->len = 0;
        g_page->serial++;
        __atomic_store_n(&g_page->seq, seq + 1u, __ATOMIC_RELEASE);
        printf("clipboardd: pid %u died mid-copy; clipboard emptied\n",
                (unsigned)g_page->lock_pid);
    } else {
        printf("clipboardd: broke a lock held by pid %u\n",
                (unsigned)g_page->lock_pid);
    }
    g_page->lock_ms = 0;
    g_page->lock_pid = 0;
    __atomic_store_n(&g_page->lock, 0u, __ATOMIC_RELEASE);
}

// THE PICTURE FILES: every one but the page's is deleted once it is a
// few seconds old -- a copier renames its file into place a moment
// BEFORE the page names it, and must not lose it in between.
#define IMAGE_GRACE_S 10

static int page_image(char *out, size_t cap) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t seq = __atomic_load_n(&g_page->seq, __ATOMIC_ACQUIRE);
        if (seq & 1u) continue;
        int is_image = g_page->op != CLIP_OP_NONE && g_page->kind == CLIP_KIND_IMAGE && g_page->count;
        size_t i = 0;
        if (is_image)
            for (; i + 1 < cap && i < CLIP_BYTES && g_page->data[i]; i++) out[i] = g_page->data[i];
        out[i] = '\0';
        if (__atomic_load_n(&g_page->seq, __ATOMIC_ACQUIRE) == seq) return is_image;
    }
    out[0] = '\0';
    return -1;   // never settled: sweep nothing this time
}

static void sweep_images(void) {
    static uint32_t swept_serial = ~0u;
    static time_t swept_at;
    time_t now = time(0);
    // Once per new clipboard, and again after the grace for what it
    // left too young to go.
    if (g_page->serial == swept_serial && now - swept_at < IMAGE_GRACE_S * 2) return;
    char keep[256];
    if (page_image(keep, sizeof keep) < 0) return;
    DIR *d = opendir(CLIP_IMAGE_DIR);
    if (!d) return;
    struct dirent *e;
    char path[320];
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        int part = n > 5 && !strcmp(e->d_name + n - 5, ".part");
        if (!part && (n < 5 || strcmp(e->d_name + n - 4, ".qoi") != 0)) continue;
        snprintf(path, sizeof path, "%s/%s", CLIP_IMAGE_DIR, e->d_name);
        struct stat st;
        // A .part is a copy still being written -- or one whose writer
        // died, which a minute tells apart.
        int grace = part ? IMAGE_GRACE_S * 6 : IMAGE_GRACE_S;
        if (!strcmp(path, keep) || stat(path, &st) != 0 || now - st.st_mtime < grace) continue;
        unlink(path);
    }
    closedir(d);
    swept_serial = g_page->serial;
    swept_at = now;
}

int main(void) {
    // SHM_EXCL is the whole "is one already running" check: a second
    // daemon would hand out a second, empty clipboard to whoever
    // happened to map it, which reads as the first one losing data.
    sys_shm_unlink(CLIP_SHM_NAME);
    // PUBLIC: every app reads the clipboard, which is what a
    // clipboard IS -- shared by design rather than by omission.
    int fd = sys_shm_open(CLIP_SHM_NAME, sizeof(struct clip_page),
                           SHM_CREATE | SHM_EXCL | SHM_PUBLIC);
    if (fd < 0) {
        printf("clipboardd: cannot create \"%s\" -- another instance?\n",
                CLIP_SHM_NAME);
        return 1;
    }

    void *p = sys_mmap(0, sizeof(struct clip_page),
                        SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    if (p == (void *)-1) {
        printf("clipboardd: cannot map the clipboard page\n");
        sys_shm_unlink(CLIP_SHM_NAME);
        return 1;
    }

    g_page = (volatile struct clip_page *)p;
    g_page->seq = 0;
    g_page->lock = 0;
    g_page->lock_pid = 0;
    g_page->lock_ms = 0;
    g_page->op = CLIP_OP_NONE;
    g_page->kind = CLIP_KIND_FILES;
    g_page->count = 0;
    g_page->len = 0;
    g_page->serial = 0;
    // LAST, and that is the handshake: a client ignores a page whose
    // magic is unset, so every other field is in place before it may
    // look at any of them.
    __atomic_store_n(&g_page->magic, CLIP_MAGIC, __ATOMIC_RELEASE);

    printf("clipboardd: clipboard ready (%u KB)\n",
            (unsigned)(CLIP_BYTES / 1024));
    sys_notify_ready();

    for (;;) {
        sys_sleep_ms(TICK_MS);
        break_stale_lock();
        sweep_images();
    }
}
