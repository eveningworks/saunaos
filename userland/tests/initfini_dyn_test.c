// initfini_dyn_test -- constructors and destructors in a DYNAMIC program:
// the executable's own (run by libc.so, from crt0's bounds) and two
// libraries' (run by /lib/ld-toy.so when libc asks). initfini_test.c has
// why the program spawns itself to see what happens after main().
//
// The order checked is glibc's (measured; userland/libs/libc/start.c):
//
//   the program's preinit array
//   libinit_a.so's constructor, then libinit_b.so's -- B NEEDS A, and the
//     program names A first, so load order is A, B and only a walk of the
//     dependencies (not a reversed load order) gets this right
//   the program's constructor, main, a dlopen whose constructor has run
//     when it returns
//   at exit: main's atexit handler, the program's destructor, then B's,
//     then A's -- the reverse of initialisation
//
// Parent side: dl_iterate_phdr() reports the executable first, named "",
// and attributes libinit_a.so's code to libinit_a.so by address.
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>
#include "rt/sys.h"
#include "base/utest.h"

int initfini_a_ready(void);
int initfini_b_marker(void);

static void out(const char *s) { sys_write(1, s, strlen(s)); }

static int g_child;

static void pre(int argc, char **argv, char **envp) {
    (void)envp;
    g_child = argc == 2 && strcmp(argv[1], "child") == 0;
    if (g_child) out("preinit ");
}
__attribute__((section(".preinit_array"), used))
static void (*g_pre)(int, char **, char **) = pre;

// The libraries write whatever the process is; only the child's pipe is read.
__attribute__((constructor)) static void exe_ctor(void) {
    if (g_child) out(initfini_a_ready() ? "exe-ctor " : "exe-ctor(LIBS-NOT-YET) ");
}
__attribute__((destructor)) static void exe_dtor(void) { if (g_child) out("exe-dtor "); }

static void h_exit(void) { out("atexit "); }

static int child(void) {
    out("main ");
    void *h = dlopen("/lib/libplug.so", RTLD_NOW);
    int *flag = h ? (int *)dlsym(h, "plug_constructed") : NULL;
    out(!h ? "dlopen(FAILED) " : flag && *flag ? "dlopen(ctor-ran) " : "dlopen(CTOR-NOT-RUN) ");
    atexit(h_exit);
    return 0;
}

static const char WANT[] =
    "preinit A-ctor B-ctor(A-first) exe-ctor main dlopen(ctor-ran) atexit "
    "exe-dtor B-dtor A-dtor ";

struct seen { int objects, first_is_exe, exe_eh_frame; const char *owner_of_b; };

static int on_object(struct dl_phdr_info *info, size_t size, void *data) {
    struct seen *s = data;
    if (size < sizeof *info) return 1;
    s->objects++;
    if (s->objects == 1 && info->dlpi_name && !info->dlpi_name[0] && info->dlpi_addr == 0) {
        s->first_is_exe = 1;
        for (int i = 0; i < info->dlpi_phnum; i++)
            if (info->dlpi_phdr[i].p_type == PT_GNU_EH_FRAME && info->dlpi_phdr[i].p_vaddr)
                s->exe_eh_frame = 1;
    }
    // Whose executable segment holds initfini_b_marker? Only a right
    // bias puts the run-time address inside the right object's range.
    uintptr_t b = (uintptr_t)initfini_b_marker;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X) &&
            b >= info->dlpi_addr + p->p_vaddr && b < info->dlpi_addr + p->p_vaddr + p->p_memsz)
            s->owner_of_b = info->dlpi_name;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (g_child) return child();

    utest_begin("initfini_dyn_test", "constructors and destructors (dynamic)", UTEST_VERDICT_FILE);

    struct seen s = {0};
    dl_iterate_phdr(on_object, &s);
    utest_checkf(s.objects >= 4, "dl_iterate_phdr reports the program and its libraries (%d)", s.objects);
    utest_check(s.first_is_exe, "...the executable first, named \"\", at bias 0");
    utest_check(s.exe_eh_frame, "...with a PT_GNU_EH_FRAME");
    utest_checkf(s.owner_of_b && strcmp(s.owner_of_b, "libinit_b.so") == 0,
                 "...and libinit_b.so's code inside libinit_b.so's segment (%s)",
                 s.owner_of_b ? s.owner_of_b : "nobody");

    int p[2];
    if (sys_pipe(p) != 0) {
        utest_check(0, "could not make a pipe");
        return utest_end();
    }
    const char *self = (argc > 0 && argv[0][0] == '/') ? argv[0] : "/tests/initfini_dyn_test";
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
                "libraries' constructors in dependency order, before the program's; "
                "destructors in reverse");
    if (strcmp(buf, WANT) != 0) {
        utest_notef("child: \"%s\"", buf);
        utest_notef("want:  \"%s\"", WANT);
    }
    return utest_end();
}
