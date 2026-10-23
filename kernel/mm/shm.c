// SYS_SHM_OPEN / SYS_SHM_UNLINK: named shared memory, and the frames
// behind SYS_MMAP's MAP_SHARED.
//
// WHY IT EXISTS. Two ring-3 processes here could not share a byte. Every
// cross-process mapping was bespoke and kernel-owned -- the sound ring
// to its one owner, a window buffer to the registered compositor -- and
// there was no way for a process to hand memory to another process the
// kernel had no opinion about. A sound daemon is the caller that needed
// one (docs/decisions.md).
//
// THE NAME IS THE RENDEZVOUS, and that is the half people forget: with
// no unix sockets and no fd passing, shared frames alone are useless
// because neither side can say WHICH frames. The namespace is what two
// unrelated processes agree on.
//
// TWO REFERENCE HOLDERS, and both must be counted or the frames leak or
// vanish under a live mapping: an open descriptor, and a mapping. A
// mapping outlives its descriptor (POSIX says close(2) leaves the map
// alone), so the mapping table below is not redundant with the fd
// table -- it is the half that survives the close.
//
// driver-none: a memory object, not a device

#include "sandbox.h"     // a sandbox reaches public objects through its unveil list
#include "sandbox_abi.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "shm.h"
#include "query.h"
#include "query_abi.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "kslots.h"
#include "initcall.h"
#include "ktest.h"
#include <stddef.h>

// One object must admit a 1920x1080 window's two buffers (~16 MiB), and
// `frames` is allocated to the size actually asked for.
#define SHM_BYTES_MAX (32ULL * 1024 * 1024)
#define SHM_PAGES_MAX (SHM_BYTES_MAX / 4096)
#define SHM_GRANTS_MAX 4  // a private object is a channel between two
                           // processes; four is slack, not a design

struct shm_object {
    char     name[SHM_NAME_MAX];
    uint32_t npages;
    int      refs;      // descriptors + mappings; 0 = the slot is free
    int      unlinked;  // no new openers; the frames still live
    // WHO MAY OPEN IT. A named object is the CREATOR's unless it says
    // otherwise: `public` is asked for at creation (SHM_PUBLIC) and is
    // what a beacon wants -- a rendezvous point every client must be
    // able to find. Everything else is a private channel between two
    // processes, and a name anyone could open makes it a private channel
    // in name only. `lsshm` lists every name, so guessing is not even
    // required.
    //
    // WHEN THIS SYSTEM GAINS USERS, none of it is wasted: `pub` is the
    // degenerate case of a MODE (public is 0666, private 0600) and the
    // creator pid gains a uid beside it. The GRANT is the part POSIX has
    // no equivalent for -- it is a capability, and it coexists with a
    // mode the way Linux has both file permissions and fd passing: the
    // mode is the coarse policy and the grant is the fine one. What
    // would have aged badly is defaulting to PUBLIC, because a
    // permission model added afterwards cannot retroactively make
    // existing callers private -- which is why every beacon has to ask.
    //
    // THE GRANT IS BY PID, and a pid can be REUSED. The window is small
    // -- a grant lives only as long as the object, and an object dies
    // with its creator -- but it is real, and a capability that survived
    // its holder is the thing to look for if this ever misbehaves.
    int      pub;
    int      granted[SHM_GRANTS_MAX];
    int      ngrants;

    int      anon;      // not in the NAMESPACE at all: shm_lookup()
                         // refuses it, so no process can name its way to
                         // one. Window buffers are anonymous for exactly
                         // that reason -- this namespace has no
                         // permissions, and a name would be a way to map
                         // somebody else's window.
    int      creator;   // pid, for QUERY_SHM
    uint64_t creator_mm; // its address space, so death can be noticed
    uint64_t *frames;   // npages entries, kmalloc'd by obj_create()
};

struct shm_map {
    uint64_t pml4;   // 0 = free
    uint64_t base;
    uint64_t npages;
    int      idx;
};

// Both tables grow on demand (kslots.h): an index is stable, and a freed
// slot's memory is kept, so a pointer held across a preemption reads a
// dead object rather than freed memory. An object being created is
// live with refs 0 until obj_create() returns, so a walk asks for refs.
static struct kslots g_objs = KSLOTS_INIT(sizeof(struct shm_object));
static struct kslots g_maps = KSLOTS_INIT(sizeof(struct shm_map));

static struct shm_object *obj(int i) { return kslots_at(&g_objs, i); }

// --- objects ---------------------------------------------------------

static int name_ok(const char *n);
static int obj_create(const char *name, uint64_t npages, int pid,
                      uint64_t pml4);

static void obj_free(int idx) {
    struct shm_object *o = obj(idx);
    for (uint32_t i = 0; i < o->npages; i++)
        if (o->frames[i]) pmm_free_frame(o->frames[i]);
    kfree(o->frames);
    k_memset(o, 0, sizeof *o);
    kslots_free(&g_objs, idx);
}

// May `pid` open object `i`? The creator always may; a public object is
// open to everyone; otherwise the creator must have said so.
static int granted_to(int i, int pid) {
    struct shm_object *o = obj(i);
    for (int g = 0; g < o->ngrants; g++) if (o->granted[g] == pid) return 1;
    return 0;
}

static int may_open(int i, int pid) {
    struct shm_object *o = obj(i);
    return o->pub || o->creator == pid || granted_to(i, pid);
}

int shm_lookup(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < kslots_cap(&g_objs); i++) {
        struct shm_object *o = obj(i);
        if (o && o->refs && !o->unlinked && !o->anon && !k_strcmp(o->name, name)) return i;
    }
    return -1;
}

// A NAMED object owned by `owner_pid`, created on that process's behalf.
//
// **THIS IS FOR A CALLER STANDING IN FOR A PROCESS**, and there is one:
// kernel/proc/win_server_test.c's fixture builds windows for pids that
// do not exist, and a window's pixels are its CLIENT's object now -- so
// a fixture with no client has to make one. Not reachable from ring 3,
// where a process creates its own objects and owns them by doing so.
//
// It is not a second creation path: obj_create() is the only one, and
// this only chooses the owner a syscall would have taken from the
// caller.
int shm_create_named(const char *name, uint64_t npages, int owner_pid) {
    if (!name || !name[0] || !name_ok(name)) return -EINVAL;
    if (!npages || npages > SHM_PAGES_MAX) return -EINVAL;
    if (shm_lookup(name) >= 0) return -EEXIST;
    return obj_create(name, npages, owner_pid, 0);
}

// Takes a name out of the namespace, for a caller standing in for a
// process. Same audience as shm_create_named(): a fixture replacing an
// object a real client would have replaced. The frames go with the last
// holder, exactly as SYS_SHM_UNLINK leaves them.
void shm_unlink_named(const char *name) {
    int idx = shm_lookup(name);
    if (idx >= 0) obj(idx)->unlinked = 1;
}

// An object with no name, for a kernel subsystem that maps both sides
// itself and needs the frames refcounted rather than owned by one
// caller. The reference returned is the caller's to shm_put().
int shm_create_anon(uint64_t npages) {
    if (!npages || npages > SHM_PAGES_MAX) return -EINVAL;
    int idx = obj_create("", npages, 0, 0);
    if (idx >= 0) obj(idx)->anon = 1;
    return idx;
}

void shm_get(int idx) {
    struct shm_object *o = obj(idx);
    if (o && o->refs) o->refs++;
}

void shm_put(int idx) {
    struct shm_object *o = obj(idx);
    if (!o || !o->refs) return;
    if (--o->refs == 0) obj_free(idx);
}

int shm_creator(int idx) {
    struct shm_object *o = obj(idx);
    return o && o->refs ? o->creator : 0;
}

uint64_t shm_total_bytes(void) {
    uint64_t pages = 0;
    for (int i = 0; i < kslots_cap(&g_objs); i++) {
        struct shm_object *o = obj(i);
        if (o && o->refs) pages += o->npages;
    }
    return pages * 4096;
}

uint64_t shm_npages(int idx) {
    struct shm_object *o = obj(idx);
    return o && o->refs ? o->npages : 0;
}

uint64_t shm_frame(int idx, uint64_t page) {
    struct shm_object *o = obj(idx);
    if (!o || !o->refs || page >= o->npages) return 0;
    return o->frames[page];
}

// --- the mapping table -----------------------------------------------

int shm_map_add(uint64_t pml4, int idx, uint64_t base, uint64_t npages) {
    int i = kslots_alloc(&g_maps);
    if (i < 0) return -ENOMEM;
    struct shm_map *m = kslots_at(&g_maps, i);
    m->idx = idx;
    m->base = base;
    m->npages = npages;
    m->pml4 = pml4;
    shm_get(idx);
    return 0;
}

// Drops mapping `i` and the reference it held.
static void map_drop(int i, struct shm_map *m) {
    int idx = m->idx;
    k_memset(m, 0, sizeof *m);
    kslots_free(&g_maps, i);
    shm_put(idx);
}

// Every mapping of `pml4` that STARTS inside the range. munmap refuses
// a range spanning more than one region, so a mapping is either wholly
// in or wholly out; a partial trim of a shared mapping would need the
// frames split between two entries and is refused at the syscall.
void shm_unmap_range(uint64_t pml4, uint64_t base, uint64_t npages) {
    uint64_t end = base + npages * 4096ULL;
    for (int i = 0; i < kslots_cap(&g_maps); i++) {
        struct shm_map *m = kslots_at(&g_maps, i);
        if (!m || m->pml4 != pml4 || m->base < base || m->base >= end) continue;
        map_drop(i, m);
    }
}

void shm_process_gone(uint64_t pml4) {
    for (int i = 0; i < kslots_cap(&g_maps); i++) {
        struct shm_map *m = kslots_at(&g_maps, i);
        if (m && m->pml4 == pml4) map_drop(i, m);
    }
    // A DEAD CREATOR'S NAME GOES WITH IT, and the reason is a deadlock
    // rather than tidiness: a server holds a reference to each client's
    // object, so the object outlives the client -- and while it lives
    // the NAME is still in the namespace, which is the only way the
    // server has to notice the client is gone. It never does. The next
    // process to reuse that pid then unlinks a name it cannot free and
    // creates a SECOND object behind it, the server keeps mixing the
    // first, and the new client's ring is read by nobody (docs/bugs.md).
    //
    // The frames still go at the last reference; only the name goes now.
    for (int i = 0; i < kslots_cap(&g_objs); i++) {
        struct shm_object *o = obj(i);
        if (o && o->refs && o->creator_mm == pml4) o->unlinked = 1;
    }
}

// --- the syscalls ----------------------------------------------------

// A name is one path segment: no '/', no '.' run that could climb, and
// not empty. It never reaches the filesystem, so this is about keeping
// the namespace flat and printable rather than about traversal.
static int name_ok(const char *n) {
    if (!n[0]) return 0;
    for (const char *p = n; *p; p++)
        if (*p == '/' || *p < 0x20 || (unsigned char)*p > 0x7E) return 0;
    return !(n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])));
}

static int obj_create(const char *name, uint64_t npages, int pid,
                      uint64_t pml4) {
    int i = kslots_alloc(&g_objs);
    if (i < 0) return -ENOMEM;
    struct shm_object *o = obj(i);   // zeroed
    o->frames = kmalloc(npages * sizeof *o->frames);
    if (!o->frames) { kslots_free(&g_objs, i); return -ENOMEM; }
    k_memset(o->frames, 0, npages * sizeof *o->frames);
    for (uint64_t p = 0; p < npages; p++) {
        uint64_t f = pmm_alloc_frame(PMM_ZONE_ANY);
        if (!f) {
            for (uint64_t q = 0; q < p; q++) pmm_free_frame(o->frames[q]);
            kfree(o->frames);
            k_memset(o, 0, sizeof *o);
            kslots_free(&g_objs, i);
            return -ENOMEM;
        }
        // Zeroed at creation, not on fault: a shared frame has no
        // per-mapper first touch to hang the zeroing on, and one
        // process must never read another's freed memory.
        k_memset((void *)(uintptr_t)f, 0, 4096);
        o->frames[p] = f;
    }
    k_strlcpy(o->name, name, sizeof o->name);
    o->npages = (uint32_t)npages;
    o->creator = pid;
    o->creator_mm = pml4;
    o->refs = 1; // the descriptor about to be installed
    return i;
}

int sys_shm_open(struct syscall_ctx *c) {
    struct shm_open_msg m;
    char name[SHM_NAME_MAX];
    int64_t ret;

    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if (!vmm_copy_string_from_user(c->pml4, name, (uint64_t)(uintptr_t)m.name,
                                   sizeof name)) { ret = -EFAULT; goto out; }
    if (!name_ok(name)) { ret = -EINVAL; goto out; }
    if (m.flags & ~(SHM_CREATE | SHM_EXCL | SHM_PUBLIC | SHM_FROM)) { ret = -EINVAL; goto out; }
    // FROM names who must ALREADY have made it; creating is the opposite.
    if ((m.flags & SHM_FROM) && ((m.flags & SHM_CREATE) || m.from_pid < 1)) {
        ret = -EINVAL; goto out;
    }

    int pid = scheduler_current_tgid();   // the PROCESS: a thread opens what its process may
    int idx = shm_lookup(name);
    if (idx >= 0) {
        if ((m.flags & SHM_CREATE) && (m.flags & SHM_EXCL)) { ret = -EEXIST; goto out; }
        // REFUSED, not "no such name": a caller told ENOENT would
        // conclude the name is free and create a SECOND object behind
        // it -- which is exactly the failure that reads as a server
        // mixing the wrong client's ring. EPERM because that is this
        // errno table's "not allowed to ask"; there is no EACCES here
        // and one caller does not earn one.
        if (!may_open(idx, pid)) { ret = -EPERM; goto out; }
        if ((m.flags & SHM_FROM) && obj(idx)->creator != m.from_pid) { ret = -EPERM; goto out; }
        // A SANDBOXED process reaches a public object -- a service's
        // beacon -- only through its unveil list; its own objects and
        // ones granted to it are capabilities already (sandbox_abi.h).
        if (obj(idx)->pub && obj(idx)->creator != pid && !granted_to(idx, pid) &&
            !sandbox_shm_public(name)) {
            ret = -ENOENT; goto out;
        }
        shm_get(idx);
    } else {
        if (!(m.flags & SHM_CREATE)) { ret = -ENOENT; goto out; }
        // A beacon is a NAME others will trust, so making one is the
        // sandbox's PUBLISH: a sandboxed page must not squat a service.
        if ((m.flags & SHM_PUBLIC) &&
            sandbox_require(PROMISE_PUBLISH, "shm_open of a public object", c->regs))
            return 0;
        uint64_t npages = (m.length + 4095) / 4096;
        if (!npages || npages > SHM_PAGES_MAX) { ret = -EINVAL; goto out; }
        idx = obj_create(name, npages, pid, c->pml4);
        if (idx < 0) { ret = idx; goto out; }
        obj(idx)->pub = (m.flags & SHM_PUBLIC) ? 1 : 0;
    }

    // A DESCRIPTION plus a descriptor naming it, fs_syscalls.c's pairing:
    // fd_desc_alloc() hands back the ONE reference and fd_install() only
    // points at it, so the unref belongs on the failure path alone.
    int di = fd_desc_alloc(&shm_fd_ops, idx);
    if (di < 0) { shm_put(idx); ret = -ENFILE; goto out; }
    int fd = fd_install(c->pml4, di);
    if (fd < 0) { fd_desc_unref(di); ret = -EMFILE; goto out; }
    ret = fd;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// Lets `pid` open an object this process created. The creator only --
// a grant is the owner's to give, and a third party handing out access
// to somebody else's channel is the whole thing this prevents.
int sys_shm_grant(struct syscall_ctx *c) {
    char name[SHM_NAME_MAX];
    int64_t ret;
    if (!vmm_copy_string_from_user(c->pml4, name, c->a0, sizeof name)) {
        ret = -EFAULT; goto out;
    }
    int idx = shm_lookup(name);
    if (idx < 0) { ret = -ENOENT; goto out; }
    if (obj(idx)->creator != scheduler_current_tgid()) { ret = -EPERM; goto out; }

    int target = (int)c->a1;
    if (target < 1) { ret = -EINVAL; goto out; }
    for (int g = 0; g < obj(idx)->ngrants; g++)
        if (obj(idx)->granted[g] == target) { ret = 0; goto out; }  // already
    if (obj(idx)->ngrants >= SHM_GRANTS_MAX) { ret = -ENOSPC; goto out; }
    obj(idx)->granted[obj(idx)->ngrants++] = target;
    ret = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_shm_unlink(struct syscall_ctx *c) {
    char name[SHM_NAME_MAX];
    int64_t ret;

    if (!vmm_copy_string_from_user(c->pml4, name, c->a0, sizeof name)) {
        ret = -EFAULT; goto out;
    }
    int idx = shm_lookup(name);
    if (idx < 0) { ret = -ENOENT; goto out; }
    // THE CREATOR'S NAME TO TAKE BACK, nobody else's. Otherwise any
    // process could unlink a running server's beacon and publish its own
    // under the name -- every later client would trust the impostor. A
    // dead creator's names are already gone (shm_process_gone()), so the
    // "previous run's leftover" every server unlinks at start is still
    // its own to clear or not there at all.
    if (obj(idx)->creator != scheduler_current_tgid()) { ret = -EPERM; goto out; }
    // The name goes now; the frames go with the last holder. An object
    // somebody is still using survives its own unlink, which is what
    // makes "no new openers" expressible.
    obj(idx)->unlinked = 1;
    ret = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// --- QUERY_SHM: the namespace, so a server can find its clients ------

static int shm_q_count(void) {
    int n = 0;
    for (int i = 0; i < kslots_cap(&g_objs); i++) {
        struct shm_object *o = obj(i);
        n += o && o->refs;
    }
    return n;
}

static int shm_q_fill(int index, void *out) {
    int n = 0;
    for (int i = 0; i < kslots_cap(&g_objs); i++) {
        struct shm_object *o = obj(i);
        if (!o || !o->refs) continue;
        if (n++ != index) continue;
        struct query_shm *r = out;
        k_memset(r, 0, sizeof *r);
        k_strlcpy(r->name, o->anon ? "(anon)" : o->name, sizeof r->name);
        r->bytes = (uint64_t)o->npages * 4096ULL;
        r->refs = (uint32_t)o->refs;
        r->creator_pid = (int32_t)o->creator;
        r->flags = o->unlinked ? QUERY_SHM_UNLINKED : 0;
        return 1;
    }
    return 0;
}

// No addressable fields: every record here is per OBJECT, and a flat
// `shm.bytes` would have to pick one of them. The record is read whole,
// as QUERY_DRIVER's is.
static const struct query_provider shm_q_provider = {
    .cls = QUERY_SHM,
    .name = "shm",
    .record_size = sizeof(struct query_shm),
    .flags = QUERY_F_LIST,
    .count = shm_q_count,
    .fill = shm_q_fill,
    .fields = NULL,
    .field_count = 0,
};

static void shm_query_init(void) { query_register(&shm_q_provider); }
INITCALL(shm_query_init, INIT_QUERY);

// --- tests -----------------------------------------------------------

KTEST("shm", "the tables grow: 400 objects and 800 mappings at once") {
    // They were SHM_MAX 288 and SHM_MAPS_MAX 640, kernel-wide; every
    // window buffer is one object and two mappings.
    enum { NOBJ = 400, NMAP = 800 };
    static int idx[NOBJ];
    const uint64_t pml4 = 0xFEED000ULL;   // never a real address space
    int made = 0, mapped = 0;
    for (int i = 0; i < NOBJ; i++) {
        idx[i] = shm_create_anon(1);
        if (idx[i] < 0) break;
        made++;
    }
    for (int k = 0; k < NMAP && made == NOBJ; k++)
        if (shm_map_add(pml4, idx[k % NOBJ], 0x400000000ULL + (uint64_t)k * 4096, 1) == 0)
            mapped++;
    int distinct = made < 2 || shm_frame(idx[0], 0) != shm_frame(idx[NOBJ - 1], 0);
    shm_process_gone(pml4);                       // drops every mapping
    int held = made == NOBJ && shm_npages(idx[NOBJ - 1]) == 1;   // ours still
    for (int i = 0; i < made; i++) shm_put(idx[i]);
    int gone = made == 0 || shm_npages(idx[0]) == 0;

    KTEST_ASSERT_EQ(made, NOBJ);
    KTEST_ASSERT_EQ(mapped, NMAP);
    KTEST_ASSERT(distinct);
    KTEST_ASSERT(held);
    KTEST_ASSERT(gone);
}

KTEST("shm", "an object outlives its unlink while somebody holds it") {
    int idx = obj_create("ktest-shm", 2, 0, 0);
    KTEST_ASSERT(idx >= 0);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 2);
    KTEST_ASSERT(shm_frame(idx, 0) != 0 && shm_frame(idx, 1) != 0);
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm"), idx);

    shm_get(idx);              // a second holder
    obj(idx)->unlinked = 1;
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm"), -1); // no new openers
    KTEST_ASSERT_EQ((int)shm_npages(idx), 2);     // still alive

    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 2);
    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);     // last holder freed it
}

KTEST("shm", "two frames of one object are distinct and zeroed") {
    int idx = obj_create("ktest-shm2", 2, 0, 0);
    KTEST_ASSERT(idx >= 0);
    uint64_t a = shm_frame(idx, 0), b = shm_frame(idx, 1);
    KTEST_ASSERT(a && b && a != b);
    for (int i = 0; i < 4096; i++) KTEST_ASSERT_EQ(((uint8_t *)(uintptr_t)a)[i], 0);
    // A write through one frame is not visible in the other -- the
    // check that would fail if a whole object were one aliased page.
    ((uint8_t *)(uintptr_t)a)[7] = 0xAB;
    KTEST_ASSERT_EQ(((uint8_t *)(uintptr_t)b)[7], 0);
    shm_put(idx);
}

KTEST("shm", "an object can be bigger than one allocation of frames") {
    // The size a window buffer needs. 96 pages is past the 64 the fixed
    // frame array used to cap every object at, so this fails outright
    // against the old shape rather than merely being slower.
    const uint64_t pages = 96;
    int idx = obj_create("ktest-shm-big", pages, 0, 0);
    KTEST_ASSERT(idx >= 0);
    KTEST_ASSERT_EQ((int)shm_npages(idx), (int)pages);
    // Every page is a distinct frame, checked at both ends and across
    // the old boundary -- an off-by-one in the allocation loop would
    // leave the tail zero and read as "no frame here".
    KTEST_ASSERT(shm_frame(idx, 0) != 0);
    KTEST_ASSERT(shm_frame(idx, 63) != 0);
    KTEST_ASSERT(shm_frame(idx, 64) != 0);
    KTEST_ASSERT(shm_frame(idx, pages - 1) != 0);
    KTEST_ASSERT(shm_frame(idx, pages) == 0);
    KTEST_ASSERT(shm_frame(idx, 0) != shm_frame(idx, pages - 1));
    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);
}

KTEST("shm", "a name is refused before a slot is spent on it") {
    KTEST_ASSERT_EQ(name_ok(""), 0);
    KTEST_ASSERT_EQ(name_ok("a/b"), 0);
    KTEST_ASSERT_EQ(name_ok(".."), 0);
    KTEST_ASSERT_EQ(name_ok("snd.3"), 1);
}

KTEST("shm", "a dead creator's NAME goes, so a server notices") {
    // The deadlock this exists to break: a server holds a reference to
    // each client's object, so the object outlives the client -- and
    // while it lives the name is in the namespace, which is the only
    // thing the server can watch. Without this the name never goes.
    int idx = obj_create("ktest-shm4", 1, 42, 0xBEEF000);
    KTEST_ASSERT(idx >= 0);
    shm_get(idx);                       // stand in for the server's hold
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm4"), idx);

    shm_process_gone(0xBEEF000);        // the creator dies
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm4"), -1); // the name is gone...
    KTEST_ASSERT_EQ((int)shm_npages(idx), 1);      // ...the frames are not

    // And the name is free at once, rather than after the last holder:
    // a process reusing that pid must not collide with the corpse.
    int second = obj_create("ktest-shm4", 1, 42, 0xCAFE000);
    KTEST_ASSERT(second >= 0 && second != idx);
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm4"), second);

    shm_put(idx);                       // the creator's own reference
    KTEST_ASSERT_EQ((int)shm_npages(idx), 1); // the server still holds it
    shm_put(idx);                       // the server lets go
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);
    shm_put(second);
}

KTEST("shm", "a dead address space drops its mappings") {
    int idx = obj_create("ktest-shm3", 1, 0, 0);
    KTEST_ASSERT(idx >= 0);
    KTEST_ASSERT_EQ(shm_map_add(0xDEAD000, idx, 0x9000000000ULL, 1), 0);
    KTEST_ASSERT_EQ(obj(idx)->refs, 2);
    shm_process_gone(0xDEAD000);
    KTEST_ASSERT_EQ(obj(idx)->refs, 1);
    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);
}

// --- a shared-memory object as a descriptor ----------------------------
//
// Never read or written through -- the only thing to do with one is
// SYS_MMAP it -- so it has no read or write op and both calls refuse it.

static void shm_fd_open(struct open_file *f, int idx) { f->shm.idx = idx; }
// A MAPPING outlives its descriptor, so this drops one reference and not
// necessarily the object -- POSIX's close-then-keep-the-map.
static void shm_fd_release(struct open_file *f) { shm_put(f->shm.idx); }

const struct fd_ops shm_fd_ops = {
    .name = "shm", .open = shm_fd_open, .release = shm_fd_release,
};
