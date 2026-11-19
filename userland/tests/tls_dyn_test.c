// tls_dyn_test -- thread-local storage in SHARED LIBRARIES: the static
// block ld-toy lays out at startup, __tls_get_addr() and the DTV, and a
// dlopen()ed library's blocks mapped per thread on first use.
//
// What a broken version would still pass, and what stops it:
//   - a loader that gave every thread ONE copy passes every value check
//     in the main thread, so each value is read in a second thread too,
//     its ADDRESS compared, and a write in one checked unseen in the other
//   - one that laid the exe's block anywhere but next to tp breaks errno
//     (the exe reaches it local-exec), so errno is checked after all this
//   - one whose DTV never grows passes a thread created AFTER a dlopen,
//     so one thread is started BEFORE it and asked afterwards -- and the
//     DTV's own length read (%fs:8), since a short table writes into the
//     slack of its page and works by accident
//   - one that leaked a joined thread's area passes everything once, so
//     fifty threads are created and joined and the region count compared
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "base/utest.h"

int *tls_lib_gd_addr(void);
int *tls_lib_bss_addr(void);
int *tls_lib_ie_addr(void);
long *tls_lib_shared_addr(void);
int tls_lib_bump(void);
extern __thread long tls_lib_shared;     // the exe's own TPOFF64 into the library's block

struct seen { int gd, bss, ie; long shared; int *gd_at, *ie_at; long *shared_at; };

static void look(struct seen *s) {
    s->gd_at = tls_lib_gd_addr();
    s->ie_at = tls_lib_ie_addr();
    s->shared_at = &tls_lib_shared;
    s->gd = *s->gd_at;
    s->bss = *tls_lib_bss_addr();
    s->ie = *s->ie_at;
    s->shared = tls_lib_shared;
}

static void *thread_look(void *arg) {
    look((struct seen *)arg);
    tls_lib_bump();                       // this thread's copies only
    return 0;
}

// The plugin, reached through dlsym from any thread.
static int *(*g_plug_addr)(void);
static int (*g_plug_zero_sum)(void);
struct plug_seen { int value, zeros; int *at; };

// The calling thread's DTV length: dtv[0], the psABI's TCB word at %fs:8.
static uint64_t dtv_len(void) {
    uint64_t *dtv;
    __asm__ volatile ("mov %%fs:8, %0" : "=r"(dtv));
    return dtv ? dtv[0] : 0;
}

static volatile int g_go;
static struct plug_seen g_early;
static uint64_t g_early_dtv_before, g_early_dtv_after;
static void *early_thread(void *arg) {        // started BEFORE the dlopen
    (void)arg;
    g_early_dtv_before = dtv_len();
    while (!g_go) sys_yield();
    g_early.at = g_plug_addr();
    g_early_dtv_after = dtv_len();
    g_early.value = *g_early.at;
    g_early.zeros = g_plug_zero_sum();
    return 0;
}
static void *late_thread(void *arg) {         // started after it
    struct plug_seen *p = arg;
    p->at = g_plug_addr();
    p->value = *p->at;
    p->zeros = g_plug_zero_sum();
    return 0;
}
static void *nothing(void *arg) { (void)arg; (void)*tls_lib_gd_addr(); return 0; }

static int region_count(void) {
    struct query_procmap q;
    int n = 0;
    for (unsigned i = 0; sys_query_record(QUERY_PROCMAP, i, &q, sizeof q) >= (int)sizeof q; i++)
        if ((int)q.pid == sys_getpid()) n++;
    return n;
}

int main(void) {
    utest_begin("tls_dyn_test", "thread-local storage in shared libraries", UTEST_VERDICT_FILE);

    struct seen m;
    look(&m);
    utest_check(m.gd == 7 && m.bss == 0, "a startup library's TLS starts from its template (global-dynamic)");
    utest_check(m.ie == 9, "...and its initial-exec variable too");
    utest_check(m.shared == 1234 && m.shared_at == tls_lib_shared_addr(),
                "the executable's extern __thread and the library's own access are one variable");

    struct seen t;
    memset(&t, 0, sizeof t);
    pthread_t th;
    int made = pthread_create(&th, 0, thread_look, &t) == 0;
    if (made) pthread_join(th, 0);
    utest_check(made && t.gd == 7 && t.ie == 9 && t.shared == 1234 && t.bss == 0,
                "a new thread starts from the templates, not from main's values");
    utest_check(made && t.gd_at != m.gd_at && t.ie_at != m.ie_at && t.shared_at != m.shared_at,
                "...at addresses of its own");
    utest_check(*tls_lib_gd_addr() == 7 && *tls_lib_ie_addr() == 9,
                "the thread's writes did not reach main's copies");

    pthread_t early;
    int early_made = pthread_create(&early, 0, early_thread, 0) == 0;

    void *h = dlopen("/lib/libtlsplug.so", RTLD_NOW);
    utest_check(h != 0, "a library with TLS can be dlopen()ed");
    if (h) {
        g_plug_addr = (int *(*)(void))dlsym(h, "tls_plug_addr");
        g_plug_zero_sum = (int (*)(void))dlsym(h, "tls_plug_zero_sum");
    }
    if (g_plug_addr && g_plug_zero_sum) {
        int *at = g_plug_addr();
        utest_check(*at == 42 && g_plug_zero_sum() == 0,
                    "its block is mapped on first use, from its template");
        utest_check(g_plug_addr() == at && g_plug_zero_sum() == 1,
                    "...once: the second call finds the same block");
        g_go = 1;
        if (early_made) pthread_join(early, 0);
        utest_check(early_made && g_early.value == 42 && g_early.zeros == 0 && g_early.at != at,
                    "a thread started BEFORE the dlopen gets a copy of its own");
        utest_checkf(g_early_dtv_after > g_early_dtv_before,
                     "...through a DTV that grew to reach the new module (%llu, then %llu)",
                     (unsigned long long)g_early_dtv_before, (unsigned long long)g_early_dtv_after);
        struct plug_seen late = {0};
        pthread_t lt;
        int lmade = pthread_create(&lt, 0, late_thread, &late) == 0;
        if (lmade) pthread_join(lt, 0);
        utest_check(lmade && late.value == 42 && late.zeros == 0 && late.at != at,
                    "...and so does one started after it");
    } else {
        g_go = 1;
        if (early_made) pthread_join(early, 0);
        utest_check(0, "dlsym finds the plugin's functions");
    }

    void *ie = dlopen("/lib/libtlsie.so", RTLD_NOW);
    const char *why = ie ? 0 : dlerror();
    utest_checkf(!ie && why && strstr(why, "initial-exec"),
                 "a dlopen() of a library using initial-exec TLS is refused, saying why (%s)",
                 why ? why : "no reason");

    int before = region_count();
    int all = 1;
    for (int i = 0; i < 50 && all; i++) {
        pthread_t x;
        all = pthread_create(&x, 0, nothing, 0) == 0 && pthread_join(x, 0) == 0;
    }
    int after = region_count();
    utest_checkf(all && after == before,
                 "fifty threads created and joined leave no mapping behind (%d regions, then %d)",
                 before, after);

    sys_close(-2);
    utest_check(errno == EBADF, "the executable's own TLS (errno) still works");
    return utest_end();
}
