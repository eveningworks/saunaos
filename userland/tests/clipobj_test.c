// The clipboard past its 64 KiB page (docs/clipboard-design.md, B1): a
// copy too large for the page goes into an object clipboardd makes, and
// a paste maps it. 2,000 file paths and 1 MiB of text round trip whole;
// a copier that exits before the paste leaves its clipboard behind; a
// sandbox that unveiled only "shm:clipboard" pastes a large one; and the
// object a newer copy replaced is unlinked. Spawned: it spawns itself as
// the copier and as the sandboxed paster.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "media/uclip.h"
#include "base/utest.h"

#define SELF "/tests/clipobj_test"
#define FILES 2000
#define TEXT_BYTES (1024 * 1024)

static struct uclip g_c;

// A fake path, long enough that FILES of them pass the page's 64 KiB.
static void path_of(int i, char *out, size_t cap) {
    snprintf(out, cap, "/tmp/clipobj/a-folder-with-a-longish-name/file-%04d.txt", i);
}

// The text, a function of its offset: a byte out of place is a byte wrong.
static char text_at(int i) { return (char)('a' + (i * 7 + i / 97) % 26); }

static char *make_text(int n) {
    char *t = malloc((size_t)n);
    if (t) for (int i = 0; i < n; i++) t[i] = text_at(i);
    return t;
}

static int text_whole(const char *s, int n, int want) {
    if (!s || n != want) return 0;
    for (int i = 0; i < n; i++) if (s[i] != text_at(i)) return 0;
    return 1;
}

// The page's own record of where the payload is -- what the test reads
// to name the object, since the library hides it.
static volatile struct clip_page *g_pg;

static int map_page(void) {
    int fd = sys_shm_open(CLIP_SHM_NAME, 0, 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, sizeof(struct clip_page), SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return 0;
    g_pg = p;
    return g_pg->magic == CLIP_MAGIC;
}

static int object_exists(uint32_t n) {
    char name[SHM_NAME_MAX];
    snprintf(name, sizeof name, CLIP_OBJECT_FMT, (unsigned)n);
    int fd = sys_shm_open(name, 0, 0);
    if (fd < 0) return 0;
    sys_close(fd);
    return 1;
}

// Waits up to ~5 s for object `n` to go: clipboardd sweeps once a second.
static int object_goes(uint32_t n) {
    for (int i = 0; i < 50; i++) {
        if (!object_exists(n)) return 1;
        sys_sleep_ms(100);
    }
    return 0;
}

static int run_child(const char *arg) {
    int pid = sys_spawn(SELF, arg, -1);
    if (pid <= 0) return -1;
    int code = -1;
    sys_waitpid(pid, &code);
    return code;
}

// --- the children --------------------------------------------------------

// Copies TEXT_BYTES and exits: the clipboard must outlive it.
static int copier(void) {
    char *t = make_text(TEXT_BYTES / 2);
    return t && uclip_set_text(t, TEXT_BYTES / 2) ? 0 : 1;
}

// Sees only the clipboard's namespace, and must paste a large one whole --
// or, as the control, only some other name, and must not.
static int sandboxed(const char *see) {
    if (unveil(see, "r") != 0 || unveil(NULL, NULL) != 0) return 2;
    uclip_load(&g_c);
    int n = 0;
    const char *s = uclip_text(&g_c, &n);
    int ok = text_whole(s, n, TEXT_BYTES / 2) && g_c.own_map;
    uclip_free(&g_c);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "copier")) return copier();
    if (argc >= 2 && !strcmp(argv[1], "sandboxed")) return sandboxed("shm:" CLIP_SHM_NAME);
    if (argc >= 2 && !strcmp(argv[1], "unseen")) return sandboxed("shm:snd.server");

    utest_begin("clipobj_test", "the clipboard past its page, through clipboardd's objects",
                UTEST_VERDICT_FILE);
    if (!map_page()) return utest_skip("clipboardd is not running");

    // --- 2,000 files ---------------------------------------------------
    char p[128];
    uclip_begin(&g_c, UCLIP_COPY);
    int added = 0;
    for (int i = 0; i < FILES; i++) { path_of(i, p, sizeof p); added += uclip_add(&g_c, p); }
    utest_checkf(added == FILES, "%d paths staged (%d)", FILES, added);
    utest_check(uclip_commit(&g_c), "...and committed");
    utest_check(g_pg->where == CLIP_OBJECT && g_pg->len > CLIP_BYTES,
                "...into an object, past the page's inline area");
    uint32_t files_obj = g_pg->object;
    uclip_load(&g_c);
    int same = uclip_count(&g_c) == FILES && g_c.own_map != 0;
    for (int i = 0; same && i < FILES; i++) {
        path_of(i, p, sizeof p);
        const char *got = uclip_path(&g_c, i);
        same = got && !strcmp(got, p);
    }
    utest_check(same, "a paste reads every path back, in order, from the mapped object");
    utest_check(uclip_path(&g_c, 7) && uclip_path(&g_c, 3) && uclip_path(&g_c, FILES - 1) &&
                !uclip_path(&g_c, FILES), "...and out of order, and stops at the end");
    uclip_free(&g_c);
    utest_check(g_c.own == 0 && g_c.data && !*g_c.data, "uclip_free() empties the snapshot");

    // --- 1 MiB of text replaces them, and their object goes ---------------
    char *t = make_text(TEXT_BYTES);
    utest_check(t && uclip_set_text(t, TEXT_BYTES), "1 MiB of text is copied");
    free(t);
    uint32_t text_obj = g_pg->object;
    utest_check(g_pg->where == CLIP_OBJECT && text_obj != files_obj, "...into an object of its own");
    uclip_load(&g_c);
    int n = 0;
    const char *s = uclip_text(&g_c, &n);
    utest_checkf(text_whole(s, n, TEXT_BYTES), "...and pastes byte-exact (%d bytes)", n);
    uclip_free(&g_c);
    utest_check(object_goes(files_obj), "the object it replaced is unlinked");
    utest_check(object_exists(text_obj), "...and its own is kept");

    // --- the copier exits before the paste --------------------------------
    int code = run_child("copier");
    utest_checkf(code == 0, "a child copies 512 KiB and exits (%d)", code);
    uint32_t child_obj = g_pg->object;
    uclip_load(&g_c);
    s = uclip_text(&g_c, &n);
    utest_check(text_whole(s, n, TEXT_BYTES / 2), "its copy pastes whole after it exited");
    uclip_free(&g_c);
    utest_check(object_goes(text_obj), "...and the 1 MiB object it replaced went");

    // --- a sandbox that sees only the clipboard ---------------------------
    code = run_child("sandboxed");
    utest_checkf(code == 0, "a process that unveiled only shm:clipboard pastes it whole (%d)", code);
    code = run_child("unseen");
    utest_checkf(code == 1, "...and one that unveiled another name pastes nothing (%d)", code);

    // --- a small copy goes back into the page ------------------------------
    utest_check(uclip_set_text("small", 5), "a small copy");
    utest_check(g_pg->where == CLIP_INLINE, "...is written into the page itself");
    uclip_load(&g_c);
    s = uclip_text(&g_c, &n);
    utest_check(s && n == 5 && !memcmp(s, "small", 5) && !g_c.own_map, "...and pastes from it");
    uclip_free(&g_c);
    utest_check(object_goes(child_obj), "...and the last object goes with the copy it held");

    return utest_end();
}
