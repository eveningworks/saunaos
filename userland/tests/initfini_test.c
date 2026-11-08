// initfini_test -- constructors, destructors and the exit list in a
// STATIC program (initfini_dyn_test is the dynamic half).
//
// THE PROGRAM TESTS ITS OWN CHILD, because half of what is checked
// happens after main() returns: destructors and exit handlers run when
// there is no main() left to assert anything. So the parent spawns this
// binary with "child", reads everything the child wrote to the pipe --
// every constructor, handler and destructor writes one word -- and
// compares the whole sequence against the order C, C++ and ELF specify:
//
//   preinit array (given argc/argv/envp, as glibc gives them)
//   constructor(101), constructor(102), constructor  -- priority order
//   main
//   exit handlers, newest first: one registered BY a handler during exit
//     runs next, 40 __cxa_atexit handlers (past the 32 that need no
//     heap) count down, one registered by a constructor runs after
//     main's
//   destructor, destructor(102), destructor(101)     -- after every handler
//
// The words go out through sys_write(), unbuffered, so a word cannot
// be lost or reordered by stdio's exit-time flush. Also checked in the
// parent: dl_iterate_phdr() reports this one object, with the headers
// link.ld maps.
#include <stdlib.h>
#include <string.h>
#include <link.h>
#include "rt/sys.h"
#include "base/utest.h"

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

static void out(const char *s) { sys_write(1, s, strlen(s)); }

// --- the child ---------------------------------------------------------

static int g_child;   // constructors run in the parent too; only the child speaks
static int g_ctors;

static void pre(int argc, char **argv, char **envp) {
    g_child = argc == 2 && strcmp(argv[1], "child") == 0;
    if (g_child) out(envp ? "preinit(args) " : "preinit(NO-ENVP) ");
}
__attribute__((section(".preinit_array"), used))
static void (*g_pre)(int, char **, char **) = pre;

static void h_ctor(void) { out("atexit-ctor "); }

__attribute__((constructor(101))) static void c101(void) { if (g_child) out("c101 "); g_ctors++; }
__attribute__((constructor(102))) static void c102(void) {
    if (!g_child) return;
    out("c102 ");
    g_ctors++;
    atexit(h_ctor);
}
__attribute__((constructor)) static void cplain(void) { if (g_child) out("cplain "); g_ctors++; }

__attribute__((destructor(101))) static void d101(void) { if (g_child) out("d101 "); }
__attribute__((destructor(102))) static void d102(void) { if (g_child) out("d102 "); }
__attribute__((destructor)) static void dplain(void) { if (g_child) out("dplain "); }

#define NCXA 40
static int g_cxa_next = NCXA - 1;   // the index that must run next
static int g_cxa_bad;
static int g_cxa_dso;

static void h_cxa(void *arg) {
    int i = (int)(intptr_t)arg;
    if (i != g_cxa_next) g_cxa_bad = 1;
    g_cxa_next--;
    if (i == 0) out(g_cxa_bad ? "cxa40(OUT-OF-ORDER) " : "cxa40 ");
}
static void h_a(void) { out("atexit-a "); }
static void h_late(void) { out("atexit-late "); }
static void h_b(void) { out("atexit-b "); atexit(h_late); }

static int child(void) {
    out(g_ctors == 3 ? "main " : "main(CTORS-MISSING) ");
    atexit(h_a);
    int ok = 1;
    for (int i = 0; i < NCXA; i++)
        if (__cxa_atexit(h_cxa, (void *)(intptr_t)i, &g_cxa_dso) != 0) ok = 0;
    if (!ok) out("cxa-REFUSED ");
    atexit(h_b);
    return 0;
}

// --- the parent ----------------------------------------------------------

static const char WANT[] =
    "preinit(args) c101 c102 cplain main atexit-b atexit-late cxa40 atexit-a "
    "atexit-ctor dplain d102 d101 ";

struct phdr_seen { int objects, load_at_base, eh_frame, named_empty; };

static int on_object(struct dl_phdr_info *info, size_t size, void *data) {
    struct phdr_seen *s = data;
    s->objects++;
    if (size < sizeof *info) return 0;
    if (info->dlpi_name && info->dlpi_name[0] == '\0') s->named_empty = 1;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && p->p_vaddr == 0x8000000000ULL && p->p_offset == 0)
            s->load_at_base = 1;
        // The header must hold what the section it names holds: the
        // eh_frame_hdr's version byte is 1.
        if (p->p_type == PT_GNU_EH_FRAME && p->p_vaddr >= 0x8000000000ULL && p->p_memsz >= 4 &&
            *(const unsigned char *)(uintptr_t)(info->dlpi_addr + p->p_vaddr) == 1)
            s->eh_frame = 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (g_child) return child();

    utest_begin("initfini_test", "constructors, destructors, exit handlers (static)", UTEST_VERDICT_FILE);

    struct phdr_seen s = {0};
    int r = dl_iterate_phdr(on_object, &s);
    utest_checkf(r == 0 && s.objects == 1, "dl_iterate_phdr reports one object (%d)", s.objects);
    utest_check(s.named_empty, "...named \"\", the executable");
    utest_check(s.load_at_base, "...whose first PT_LOAD maps the headers at the image base");
    utest_check(s.eh_frame, "...and whose PT_GNU_EH_FRAME points at a real .eh_frame_hdr");

    int p[2];
    if (sys_pipe(p) != 0) {
        utest_check(0, "could not make a pipe");
        return utest_end();
    }
    const char *self = (argc > 0 && argv[0][0] == '/') ? argv[0] : "/tests/initfini_test";
    int pid = sys_spawn(self, "child", p[1]);
    utest_check(pid > 0, "spawned itself as the child");
    sys_close(p[1]);
    static char buf[512];
    int n = 0;
    for (;;) {
        int64_t got = sys_read(p[0], buf + n, (uint64_t)(sizeof buf - 1 - n));
        if (got <= 0) break;
        n += (int)got;
        if (n >= (int)sizeof buf - 1) break;
    }
    buf[n] = '\0';
    sys_close(p[0]);
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    utest_checkf(code == 0, "the child exited 0 (%d)", code);
    utest_check(strcmp(buf, WANT) == 0,
                "preinit, constructors by priority, main, handlers newest first, destructors last");
    if (strcmp(buf, WANT) != 0) {
        utest_notef("child: \"%s\"", buf);
        utest_notef("want:  \"%s\"", WANT);
    }
    return utest_end();
}
