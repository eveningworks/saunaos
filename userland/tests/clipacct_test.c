// The clipboard is the session's account's: clipboardd runs as it, and
// the page and its objects are SHM_USER. A child started as that account
// pastes a large copy; one started as another account can open neither
// the page nor an object, asks clipboardd for an object in vain and
// copies nothing. A picture lands in the account's own 0700 folder.
// Stopping clipboardd (what a sign-out does) clears the page, so a
// process that held it -- this one, root's, like remoted -- reads nothing
// of it, and uclip finds the next daemon's page by itself.
//
// Runs as root: it starts children as other accounts, and stops and
// starts clipboardd.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "query_abi.h"
#include "rt/sys.h"
#include "syscall_abi.h"
#include "base/uchan.h"
#include "base/uclip_page.h"
#include "base/utest.h"
#include "media/uclip.h"
#include "media/uimg.h"

#define SELF "/tests/clipacct_test"
#define BIG (256 * 1024)   // past the page: an object

static struct uclip g_c;
static volatile struct clip_page *g_pg;   // read-only, as clipboardd made it

static char byte_at(int i) { return (char)('a' + (i * 5 + i / 31) % 26); }

static int copy_big(void) {
    char *t = malloc(BIG);
    if (!t) return 0;
    for (int i = 0; i < BIG; i++) t[i] = byte_at(i);
    int ok = uclip_set_text(t, BIG);
    free(t);
    return ok;
}

static int pasted_big(void) {
    uclip_load(&g_c);
    int n = 0;
    const char *s = uclip_text(&g_c, &n);
    int ok = s && n == BIG;
    for (int i = 0; ok && i < BIG; i++) ok = s[i] == byte_at(i);
    uclip_free(&g_c);
    return ok;
}

static volatile struct clip_page *map_page(void) {
    int fd = sys_shm_open(CLIP_SHM_NAME, 0, 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, sizeof(struct clip_page), SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : (volatile struct clip_page *)p;
}

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

static int run_as(const char *arg, uint32_t uid) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = arg;
    o.flags = SPAWN_CREDS;
    o.uid = uid;
    o.gid = uid;
    int pid = sys_spawn_opts(SELF, &o);
    return pid > 0 ? wait_code(pid) : -3000;
}

static int service(const char *args) {
    int pid = sys_spawn("/bin/service", args, -1);
    return pid > 0 ? wait_code(pid) : -3000;
}

// --- the children --------------------------------------------------------

// The account's own process: the large copy pastes whole.
static int same_account(void) { return pasted_big() ? 0 : 1; }

// Another account's: the first refusal that did not happen, by number.
static int other_account(uint32_t object) {
    int fd = sys_shm_open(CLIP_SHM_NAME, 0, 0);
    if (fd != -EPERM) return 1;
    char name[SHM_NAME_MAX];
    snprintf(name, sizeof name, CLIP_OBJECT_FMT, (unsigned)object);
    fd = sys_shm_open(name, 0, 0);
    if (fd != -EPERM) return 2;
    uclip_load(&g_c);
    int n = 0;
    const char *s = uclip_text(&g_c, &n);
    uclip_free(&g_c);
    if (s && n) return 3;
    if (uclip_set_text("x", 1)) return 4;
    // Straight to clipboardd, past uclip: it must refuse to make one.
    struct uchan_client c;
    if (uchan_client_open(&c, CLIP_SERVER_NAME) != 0) return 5;
    struct clip_req req = { CLIP_REQ_OBJECT, 0, 4096 };
    struct clip_reply rep = { 1, 0 };
    int r = uchan_call(&c, &req, sizeof req, &rep, sizeof rep, 3000);
    uchan_client_close(&c);
    if (r != 0) return 6;
    if (rep.status != -EPERM) return 7;
    return 0;
}

// Another account takes the name while clipboardd is down: a page with the
// magic and a payload of its own, held until the parent says done.
#define SQUAT_DONE "/tmp/clipacct.done"
static int squatter(void) {
    int fd = sys_shm_open(CLIP_SHM_NAME, sizeof(struct clip_page), SHM_CREATE | SHM_EXCL | SHM_PUBLIC);
    if (fd < 0) return 1;
    void *p = sys_mmap(0, sizeof(struct clip_page), SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    if (p == (void *)-1) return 2;
    struct clip_page *pg = p;
    pg->op = CLIP_OP_COPY;
    pg->kind = CLIP_KIND_TEXT;
    pg->count = 1;
    memcpy(pg->data, "squatted", 9);
    pg->len = 9;
    pg->serial = 77;
    __atomic_store_n(&pg->magic, CLIP_MAGIC, __ATOMIC_RELEASE);
    struct stat st;
    for (int i = 0; i < 200 && stat(SQUAT_DONE, &st) != 0; i++) sys_sleep_ms(50);
    return 0;
}

// The account's process while the squatter holds the name: nothing pasted.
static int fooled(void) {
    uclip_load(&g_c);
    int n = 0;
    const char *s = uclip_text(&g_c, &n);
    int seen = s && n && strstr(s, "squatted");
    uclip_free(&g_c);
    return seen ? 1 : 0;
}

// --- the parent ----------------------------------------------------------

static int page_record(struct query_shm *out) {
    struct query_shm r;
    QUERY_FOREACH(QUERY_SHM, r, i) {
        if (!strcmp(r.name, CLIP_SHM_NAME) && !(r.flags & QUERY_SHM_UNLINKED)) { *out = r; return 1; }
    }
    return 0;
}

// Up to ~5 s for a condition clipboardd brings about.
#define WAIT_FOR(cond) ({ int _ok = 0; \
    for (int _i = 0; _i < 50 && !(_ok = (cond)); _i++) { sys_sleep_ms(100); } \
    _ok; })

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "same")) return same_account();
    if (argc >= 3 && !strcmp(argv[1], "other")) return other_account((uint32_t)atoi(argv[2]));
    if (argc >= 2 && !strcmp(argv[1], "squat")) return squatter();
    if (argc >= 2 && !strcmp(argv[1], "fooled")) return fooled();

    utest_begin("clipacct_test", "the clipboard is the session account's, and ends with it",
                UTEST_VERDICT_FILE);
    if (geteuid() != 0) return utest_skip("needs root");
    g_pg = map_page();
    if (!g_pg || g_pg->magic != CLIP_MAGIC) return utest_skip("clipboardd is not running");
    uint32_t uid = g_pg->uid;
    utest_checkf(uid != 0, "the clipboard is an account's, not root's (uid %u)", (unsigned)uid);

    struct query_shm rec;
    utest_check(page_record(&rec) && (rec.flags & QUERY_SHM_ACCOUNT) && rec.uid == uid,
                "the page opens to that account alone (SHM_USER)");

    // --- the account, and another --------------------------------------
    utest_check(copy_big(), "root copies 256 KiB for the session");
    uint32_t obj = g_pg->object;
    utest_check(g_pg->where == CLIP_OBJECT && obj, "...into an object");
    int code = run_as("same", uid);
    utest_checkf(code == 0, "a process of the account pastes it whole (%d)", code);
    char arg[32];
    snprintf(arg, sizeof arg, "other %u", (unsigned)obj);
    code = run_as(arg, uid + 1);
    utest_checkf(code == 0, "another account's opens neither page nor object, and copies nothing "
                 "(first miss %d)", code);

    // --- a picture --------------------------------------------------------
    uint32_t px[16];
    for (int i = 0; i < 16; i++) px[i] = 0xFF336699u;
    struct uimg im = { 4, 4, px, 0 };
    utest_check(uclip_set_image(&im), "a picture is copied");
    char dir[48];
    snprintf(dir, sizeof dir, CLIP_IMAGE_DIR_FMT, (unsigned)uid);
    struct stat st;
    memset(&st, 0, sizeof st);
    int dir_ok = stat(dir, &st) == 0;
    utest_checkf(dir_ok && S_ISDIR(st.st_mode) && st.st_uid == uid &&
                 (st.st_mode & 0777) == 0700,
                 "...into the account's own folder, mode 0700 (%s, uid %u, %o)",
                 dir, (unsigned)st.st_uid, (unsigned)(st.st_mode & 0777));
    char pic[128] = "";
    uclip_load(&g_c);
    const char *p = uclip_image(&g_c);
    if (p) snprintf(pic, sizeof pic, "%s", p);
    uclip_free(&g_c);
    utest_checkf(!strncmp(pic, dir, strlen(dir)) && stat(pic, &st) == 0,
                 "...and the page names a file in it (%s)", pic);

    // --- the session ends -------------------------------------------------
    utest_check(uclip_set_text("left behind", 11), "a copy before the daemon stops");
    utest_check(service("stop clipboardd") == 0, "clipboardd stops, as at a sign-out");
    utest_check(WAIT_FOR(g_pg->magic == 0), "...and the page it leaves says so");
    int blank = g_pg->len == 0 && g_pg->op == CLIP_OP_NONE;
    for (int i = 0; blank && i < 64; i++) blank = g_pg->data[i] == 0;
    utest_check(blank, "...with nothing of the copy in it, for a process that still maps it");
    utest_check(stat(pic, &st) != 0, "...and the picture is deleted");
    utest_check(service("start clipboardd") == 0, "clipboardd starts again");
    volatile struct clip_page *next = 0;
    utest_check(WAIT_FOR((next = map_page()) && next->magic == CLIP_MAGIC), "...with a page of its own");
    uclip_load(&g_c);
    int n = 0;
    const char *s = uclip_text(&g_c, &n);
    utest_check(!(s && n) && uclip_count(&g_c) == 0, "the next session's clipboard is empty");
    uclip_free(&g_c);
    utest_check(uclip_set_text("after", 5), "a copy after it");
    utest_check(next && next->len == 6 && !memcmp((const void *)next->data, "after", 5),
                "...lands in the new page: uclip let go of the one that ended");

    // --- killed outright: the magic stays, the name goes ------------------
    utest_check(uclip_set_text("before the kill", 15), "a copy before clipboardd is killed");
    int dpid = (int)next->daemon_pid;
    utest_checkf(dpid > 1 && sys_kill(dpid, SIGKILL) == 0, "clipboardd (pid %d) is killed outright", dpid);
    volatile struct clip_page *after_kill = 0;
    utest_check(WAIT_FOR((after_kill = map_page()) && after_kill != next &&
                         after_kill->magic == CLIP_MAGIC && after_kill->daemon_pid != (uint32_t)dpid),
                "...and init starts another");
    sys_sleep_ms(1200);   // sleep-ok: past uclip's RECHECK_MS, the cadence under test
    uclip_load(&g_c);
    s = uclip_text(&g_c, &n);
    utest_check(!(s && n && strstr(s, "before the kill")),
                "a root process no longer reads the killed one's page, magic and all");
    uclip_free(&g_c);

    // --- another account takes the name first ----------------------------
    unlink(SQUAT_DONE);
    utest_check(service("stop clipboardd") == 0, "clipboardd stops again");
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = "squat";
    o.flags = SPAWN_CREDS;
    o.uid = o.gid = uid + 1;
    int sq = sys_spawn_opts(SELF, &o);
    utest_check(sq > 0 && WAIT_FOR((next = map_page()) && next->magic == CLIP_MAGIC && next->serial == 77),
                "another account takes the name while it is down");
    sys_sleep_ms(1200);   // sleep-ok: past RECHECK_MS, so the stopped page is let go of
    uclip_load(&g_c);
    s = uclip_text(&g_c, &n);
    utest_check(!(s && n && strstr(s, "squatted")), "...and root does not paste its page");
    uclip_free(&g_c);
    utest_check(!uclip_set_text("for the squatter", 16), "...nor copy into it");
    code = run_as("fooled", uid);
    utest_checkf(code == 0, "...nor does the account's own process (%d)", code);
    FILE *f = fopen(SQUAT_DONE, "w");
    if (f) fclose(f);
    utest_check(sq > 0 && wait_code(sq) == 0, "the squatter lets go");
    unlink(SQUAT_DONE);
    utest_check(service("start clipboardd") == 0 &&
                WAIT_FOR((next = map_page()) && next->magic == CLIP_MAGIC), "clipboardd is back");
    return utest_end();
}
