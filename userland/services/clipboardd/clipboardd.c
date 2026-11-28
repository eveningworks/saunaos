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
// (media/uclip.c), so a paste costs no syscall and no context switch and
// a menu item can watch the serial every frame for nothing. That is the
// opposite of X11 and Wayland, where the SOURCE app serves the bytes on
// demand -- which is exactly why closing the app you copied from loses
// your clipboard there, and why every desktop ships a clipboard manager
// to paper over it.
//
// ONE PER SESSION: init runs it as the account whose session is on, and
// as `greeter` on the login screen (services.d/clipboardd), restarting it
// at each sign-in and sign-out, as Windows keeps a clipboard per
// session's window station. The page and its objects are SHM_USER, so
// another account's processes cannot open them; root's can (remoted's
// VNC clipboard). As it stops it wipes the page, its objects and the
// account's pictures, so nothing of one person's reaches the next.
//
// ONE REQUEST, AND IT CARRIES NO BYTES: a copy too large for the page
// asks for an object of its size on CLIP_SERVER_NAME, fills it itself and
// commits the page naming it. The daemon MAKES the object so it owns the
// name (shm_unlink is the creator's alone) and the memory outlives the
// copier, and unlinks it once no page names it -- docs/clipboard-design.md,
// option B1. Its memory is charged to this daemon, not the copier, which
// one object in flight per process bounds. Otherwise the loop notices a stuck lock and deletes the
// files of pictures no clipboard names any more (CLIP_IMAGE_DIR_FMT).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/uchan.h"
#include "base/uclip_page.h"
#include "base/ugrow.h"
#include "syscall_abi.h"
#include "proc_info.h"   // who asks: an object is the account's to ask for

#define TICK_MS 1000

static volatile struct clip_page *g_page;
static uint32_t g_uid;             // the account this clipboard is
static char g_imgdir[48];          // CLIP_IMAGE_DIR_FMT, for g_uid
static volatile sig_atomic_t g_stop;   // SIGTERM: a session's end

static void image_dir_ready(int fresh);

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
        g_page->where = CLIP_INLINE;
        g_page->object = 0;
        g_page->serial++;
        // Which slot it was writing is not known: the drag goes too.
        g_page->drag_count = 0;
        g_page->drag_len = 0;
        g_page->drag_where = CLIP_INLINE;
        g_page->drag_object = 0;
        g_page->drag_pid = 0;
        g_page->drag_serial++;
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
        int is_image = g_page->op != CLIP_OP_NONE && g_page->kind == CLIP_KIND_IMAGE &&
                       g_page->count && g_page->where == CLIP_INLINE;
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
    DIR *d = opendir(g_imgdir);
    if (!d) { image_dir_ready(0); return; }   // deleted under it: made again
    struct dirent *e;
    char path[320];
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        int part = n > 5 && !strcmp(e->d_name + n - 5, ".part");
        if (!part && (n < 5 || strcmp(e->d_name + n - 4, ".qoi") != 0)) continue;
        snprintf(path, sizeof path, "%s/%s", g_imgdir, e->d_name);
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

// --- the objects of large copies ------------------------------------

// AN OBJECT NO PAGE HAS NAMED YET is a copy in flight, and is kept for
// as long as its copier may still commit it: until that process asks for
// another (ONE in flight per process, so no client can pin memory by
// asking and never committing) or its channel goes -- and even then only
// once the page does not name it. Never on a timer: a slow fill of a
// large copy outlasted one.
struct object {
    uint32_t n;          // its number: the name is CLIP_OBJECT_FMT
    int      fd;         // held, or the object's last reference goes
    uint64_t len;
    int      pid;        // who asked
    int      conn;       // ...on which channel
    int      named;      // a page has named it: unnamed again, it goes at once
    int      spent;      // its copier asked again or went: it goes unless a page names it
};

static struct object *g_obj;
static int g_nobj, g_capobj;
static uint32_t g_next_obj;
static struct uchan_server g_chan;
static int g_chan_up;

static void object_name(uint32_t n, char *out, size_t cap) {
    snprintf(out, cap, CLIP_OBJECT_FMT, (unsigned)n);
}

static void sweep_objects(void);

// The object `len` bytes long a copier asked for: its number, or -errno.
static int64_t make_object(uint64_t len, int pid, int conn) {
    if (!len) return -EINVAL;
    // ITS LAST COPY IS SPENT, committed or abandoned -- and the sweep,
    // which reads the page, is what tells the two apart.
    for (int i = 0; i < g_nobj; i++) if (g_obj[i].pid == pid) g_obj[i].spent = 1;
    sweep_objects();
    if (!UGROW(g_obj, g_capobj, g_nobj + 1)) return -ENOMEM;
    uint32_t n = ++g_next_obj ? g_next_obj : ++g_next_obj;   // never 0
    char name[SHM_NAME_MAX];
    object_name(n, name, sizeof name);
    // THE ACCOUNT'S like the page: whoever may paste may map it, and a
    // sandbox reaches it through its unveil of "shm:clipboard".
    int fd = sys_shm_open(name, len, SHM_CREATE | SHM_EXCL | SHM_USER);
    if (fd < 0) {
        printf("clipboardd: no object of %llu bytes for pid %d (%d)\n",
               (unsigned long long)len, pid, fd);
        return fd;
    }
    if (len > CLIP_OBJECT_LOG_BYTES)
        printf("clipboardd: pid %d copies %llu MiB\n", pid,
               (unsigned long long)(len >> 20));
    g_obj[g_nobj++] = (struct object){ n, fd, len, pid, conn, 0, 0 };
    return n;
}

static void retire(int i) {
    char name[SHM_NAME_MAX];
    object_name(g_obj[i].n, name, sizeof name);
    sys_shm_unlink(name);
    sys_close(g_obj[i].fd);
    g_obj[i] = g_obj[--g_nobj];
}

// The objects the page names now, the clipboard's and the drag slot's;
// 0 for none. -1 when it never settled.
static int named_objects(uint32_t *clip, uint32_t *drag) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t seq = __atomic_load_n(&g_page->seq, __ATOMIC_ACQUIRE);
        if (seq & 1u) continue;
        *clip = g_page->op != CLIP_OP_NONE && g_page->where == CLIP_OBJECT ? g_page->object : 0;
        *drag = g_page->drag_count && g_page->drag_where == CLIP_OBJECT ? g_page->drag_object : 0;
        if (__atomic_load_n(&g_page->seq, __ATOMIC_ACQUIRE) == seq) return 0;
    }
    return -1;
}

// THE PAGE DECIDES: an object it names now is kept, whatever else is
// true of it; one it named before, or whose copier is spent, goes. One in
// flight stays (struct object). Nothing goes when the page never settled.
static void sweep_objects(void) {
    if (!g_nobj) return;
    uint32_t clip, drag;
    if (named_objects(&clip, &drag) < 0) return;
    for (int i = g_nobj - 1; i >= 0; i--) {
        if (g_obj[i].n == clip || g_obj[i].n == drag) { g_obj[i].named = 1; continue; }
        if (g_obj[i].named || g_obj[i].spent) retire(i);
    }
}

// A copier whose channel went cannot commit any more: its objects are
// spent -- kept by the sweep only while a page names one (a copier that
// committed and exited at once).
static void forget_departed(void) {
    int gone[16];
    int n;
    while ((n = uchan_server_scan(&g_chan, gone, 16)) > 0) {
        for (int k = 0; k < n && k < 16; k++)
            for (int i = 0; i < g_nobj; i++)
                if (g_obj[i].conn == gone[k]) g_obj[i].spent = 1;
        if (n < 16) break;
    }
    sweep_objects();
}

// THE ACCOUNT'S, OR ROOT'S, to ask for: an object is memory charged to
// this daemon, and one another account's process asked for is one it
// could not open anyway (SHM_USER).
static int same_account(int pid) {
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++)
        if (pi.pid == pid && pi.tgid == pid) return pi.euid == g_uid || pi.euid == 0;
    return 0;
}

static void serve(void) {
    forget_departed();
    struct clip_req req;
    int from;
    memset(&req, 0, sizeof req);
    while ((from = uchan_server_recv(&g_chan, &req, sizeof req)) != 0) {
        struct clip_reply rep = { -EINVAL, 0 };
        int pid = uchan_server_peer_pid(&g_chan, from);
        if (!same_account(pid)) rep.status = -EPERM;
        else if (req.what == CLIP_REQ_OBJECT) {
            int64_t r = make_object(req.len, pid, from);
            if (r > 0) { rep.status = 0; rep.object = (uint32_t)r; }
            else rep.status = (int32_t)r;
        }
        uchan_server_reply(&g_chan, from, &rep, sizeof rep);
        memset(&req, 0, sizeof req);
    }
}

static void on_term(int sig) { (void)sig; g_stop = 1; }

// Every picture in the account's folder, whatever its age: at start
// (a crashed run's) and at the end of the session.
static void empty_images(void) {
    DIR *d = opendir(g_imgdir);
    if (!d) return;
    struct dirent *e;
    char path[320];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", g_imgdir, e->d_name);
        unlink(path);
    }
    closedir(d);
}

// THE ACCOUNT'S OWN FOLDER, 0700: /tmp is everyone's, so a name someone
// else took first is refused -- said here, and each copier refuses it too.
// `fresh`: at start, when what is in it is a crashed run's. Said once.
static void image_dir_ready(int fresh) {
    static int said;
    mkdir(g_imgdir, 0700);
    struct stat st;
    if (stat(g_imgdir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != g_uid) {
        if (!said++)
            printf("clipboardd: %s is not this account's -- pictures will not be copied\n", g_imgdir);
        return;
    }
    if (st.st_mode & 077) chmod(g_imgdir, 0700);
    if (fresh) empty_images();
}

// THE SESSION ENDS: nothing of this clipboard may reach the next one. The
// magic goes FIRST, so a process that outlives the session (remoted, as
// root) maps the next page instead of reading this one, then the payload,
// the objects and the pictures.
// UNDER THE PAGE'S LOCK, so a copy either landed before it or sees the
// magic gone and is refused (uclip's lock_page()); a holder stuck past
// the spins is a dead one, and the wipe goes ahead.
static void wipe(void) {
    uint32_t me = (uint32_t)sys_getpid();
    int locked = 0;
    for (long i = 0; i < CLIP_LOCK_SPINS && !locked; i++) {
        uint32_t free_ = 0;
        locked = __atomic_compare_exchange_n(&g_page->lock, &free_, me, 0,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_page->magic, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_page->seq, 1, __ATOMIC_ACQ_REL);   // odd: mid-update
    g_page->op = CLIP_OP_NONE;
    g_page->kind = CLIP_KIND_FILES;
    g_page->count = g_page->len = 0;
    g_page->where = CLIP_INLINE;
    g_page->object = 0;
    g_page->serial++;
    g_page->drag_count = g_page->drag_len = 0;
    g_page->drag_where = CLIP_INLINE;
    g_page->drag_object = 0;
    g_page->drag_serial++;
    memset((void *)g_page->data, 0, sizeof g_page->data);
    memset((void *)g_page->drag_data, 0, sizeof g_page->drag_data);
    __atomic_add_fetch(&g_page->seq, 1, __ATOMIC_ACQ_REL);   // even again
    __atomic_store_n(&g_page->lock, 0u, __ATOMIC_RELEASE);
    while (g_nobj) retire(0);   // unlinked: only a paste under way still holds one
    empty_images();
    sys_shm_unlink(CLIP_SHM_NAME);
    printf("clipboardd: the session's clipboard cleared\n");
}

int main(void) {
    // SHM_EXCL is the whole "is one already running" check: a second
    // daemon would hand out a second, empty clipboard to whoever
    // happened to map it, which reads as the first one losing data.
    g_uid = (uint32_t)geteuid();
    snprintf(g_imgdir, sizeof g_imgdir, CLIP_IMAGE_DIR_FMT, (unsigned)g_uid);
    signal(SIGTERM, on_term);
    sys_shm_unlink(CLIP_SHM_NAME);
    // THE ACCOUNT'S: every one of its programs reads the clipboard,
    // which is what a clipboard IS, and no other account's does.
    int fd = sys_shm_open(CLIP_SHM_NAME, sizeof(struct clip_page),
                           SHM_CREATE | SHM_EXCL | SHM_USER);
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
    g_page->uid = g_uid;
    g_page->daemon_pid = (uint32_t)sys_getpid();
    g_page->seq = 0;
    g_page->lock = 0;
    g_page->lock_pid = 0;
    g_page->lock_ms = 0;
    g_page->op = CLIP_OP_NONE;
    g_page->kind = CLIP_KIND_FILES;
    g_page->count = 0;
    g_page->len = 0;
    g_page->serial = 0;
    g_page->where = CLIP_INLINE;
    g_page->object = 0;
    // LAST, and that is the handshake: a client ignores a page whose
    // magic is unset, so every other field is in place before it may
    // look at any of them.
    __atomic_store_n(&g_page->magic, CLIP_MAGIC, __ATOMIC_RELEASE);

    // WITHOUT ITS CHANNEL the page still works: copies that fit it go
    // on, and a larger one is refused and says so.
    g_chan_up = uchan_server_open(&g_chan, CLIP_SERVER_NAME) == 0;
    if (!g_chan_up)
        printf("clipboardd: cannot open \"%s\" -- large copies will be refused\n",
               CLIP_SERVER_NAME);

    image_dir_ready(1);

    printf("clipboardd: clipboard ready (%u KB in the page, larger in objects)\n",
            (unsigned)(CLIP_BYTES / 1024));
    sys_notify_ready();

    while (!g_stop) {
        if (g_chan_up) {
            uchan_server_wait(&g_chan, TICK_MS);
            serve();
        } else {
            sys_sleep_ms(TICK_MS);
        }
        break_stale_lock();
        sweep_images();
        sweep_objects();
    }
    wipe();
    return 0;
}
