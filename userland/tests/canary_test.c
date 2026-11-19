// canary_test -- the stack-protector canary is random per exec, and the
// two places code reads it agree.
//
// Code built with GCC's default guard compares against %fs:0x28 (each
// thread's TCB); rt/, ld-toy and programs built before the default
// changed compare against __stack_chk_guard. Both must hold the canary
// the kernel's AT_RANDOM bytes make, in every thread, and a new exec
// must get a new one. The parent spawns this program twice (static:
// rt/tls.c seeds it) and canary_dyn_test once (dynamic: ld-toy seeds
// it), each printing canary_probe.h's line, and compares the three.
//
// What a broken version would still pass: nothing here sees whether an
// instrumented function actually CHECKS the value -- stack_smash_test is
// the one that overflows a buffer and must be caught.
//
// It must be SPAWNED, not `run` -- it has children and threads.
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "base/utest.h"
#include "canary_probe.h"

#define FALLBACK 0x5A3CC5AA9900DEB1ULL   // rt/stack_chk.c's built-in value

struct view { uint64_t tls, global, thread, random; };

static int run_probe(const char *path, struct view *v) {
    int p[2];
    if (sys_pipe(p) != 0) return 0;
    int pid = sys_spawn(path, "--print", p[1]);
    sys_close(p[1]);
    char buf[160];
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
    char *s = buf;
    v->tls = strtoull(s, &s, 16);
    v->global = strtoull(s, &s, 16);
    v->thread = strtoull(s, &s, 16);
    v->random = strtoull(s, &s, 16);
    return pid > 0 && code == 0 && n > 0;
}

static void check_view(const char *who, const struct view *v) {
    utest_checkf(v->tls != 0 && v->tls != FALLBACK && v->tls != (FALLBACK & ~0xFFULL),
                 "%s: the canary is not zero or the built-in fallback (%016llx)",
                 who, (unsigned long long)v->tls);
    utest_checkf((v->tls & 0xFF) == 0, "%s: its low byte is zero", who);
    utest_checkf(v->tls == v->random, "%s: it is the one AT_RANDOM makes (%016llx)",
                 who, (unsigned long long)v->random);
    utest_checkf(v->global == v->tls, "%s: __stack_chk_guard holds the same value (%016llx)",
                 who, (unsigned long long)v->global);
    utest_checkf(v->thread == v->tls, "%s: a second thread's %%fs:0x28 is the same (%016llx)",
                 who, (unsigned long long)v->thread);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--print")) return probe_print();
    utest_begin("canary_test", "the stack canary is random per exec, in TLS and the global",
                UTEST_VERDICT_FILE);
    struct view a, b, d;
    int ra = run_probe("/tests/canary_test", &a);
    int rb = run_probe("/tests/canary_test", &b);
    int rd = run_probe("/tests/canary_dyn_test", &d);
    utest_check(ra && rb, "a static child printed its canary twice");
    utest_check(rd, "a dynamic child printed its canary");
    if (ra) check_view("static", &a);
    if (rd) check_view("dynamic", &d);
    if (ra && rb) utest_check(a.tls != b.tls, "two execs of one program get different canaries");
    if (ra && rd) utest_check(a.tls != d.tls, "...and so do a static and a dynamic program");
    return utest_end();
}
