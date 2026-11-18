// See base/uworker.h.
#include <pthread.h>
#include "base/uworker.h"
#include <sys/resource.h>   // PRIO_PROCESS
#include "rt/sys.h"
#include "base/urace.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct uworker_job *g_head, *g_tail;
static int g_started;     // 1 running, -1 could not start (jobs run inline)
static int g_pending;     // jobs not IDLE

// THE WORKER YIELDS THE CPU TO ITS PROGRAM'S OWN THREAD: nice 10, a
// weight below the frame thread's (CFS gives ~1.25x a step). Parked
// between jobs it wakes with a sleeper's credit, and without the lower
// weight its decode outran the compositor's frame under load -- a 3 s
// frame spent waiting for the CPU while its own worker ran. Linux's
// compositors do the same from the other side (KWin's and Mutter's main
// thread at a real-time priority).
#define UWORKER_NICE 10

static void *worker(void *arg) {
    (void)arg;
    // `who` 0 is the calling THREAD here (its own slot), as Linux applies nice.
    sys_setpriority(PRIO_PROCESS, 0, UWORKER_NICE);
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_head) pthread_cond_wait(&g_cond, &g_lock);
        struct uworker_job *j = g_head;
        g_head = j->next;
        if (!g_head) g_tail = 0;
        j->next = 0;
        j->state = UWORKER_RUNNING;
        pthread_mutex_unlock(&g_lock);
        j->run(j);   // no lock held: this is where the waiting happens
        urace_delay();   // a test's race amplifier (base/urace.h); 0 by default
        pthread_mutex_lock(&g_lock);
        j->state = UWORKER_DONE;
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

int uworker_submit(struct uworker_job *job) {
    if (!job || !job->run) return 0;
    pthread_mutex_lock(&g_lock);
    if (job->state != UWORKER_IDLE) { pthread_mutex_unlock(&g_lock); return 0; }
    if (!g_started) {
        pthread_t t;
        g_started = pthread_create(&t, 0, worker, 0) == 0 ? 1 : -1;
        if (g_started < 0) sys_eprint("uworker: no worker thread -- jobs run inline (said once)\n");
    }
    g_pending++;
    if (g_started < 0) {
        job->state = UWORKER_RUNNING;
        pthread_mutex_unlock(&g_lock);
        job->run(job);
        pthread_mutex_lock(&g_lock);
        job->state = UWORKER_DONE;
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    job->state = UWORKER_QUEUED;
    job->next = 0;
    if (g_tail) g_tail->next = job; else g_head = job;
    g_tail = job;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int uworker_take(struct uworker_job *job) {
    if (!job) return 0;
    pthread_mutex_lock(&g_lock);
    int done = job->state == UWORKER_DONE;
    if (done) { job->state = UWORKER_IDLE; g_pending--; }
    pthread_mutex_unlock(&g_lock);
    return done;
}

int uworker_busy(struct uworker_job *job) {
    if (!job) return 0;
    pthread_mutex_lock(&g_lock);
    int busy = job->state != UWORKER_IDLE;
    pthread_mutex_unlock(&g_lock);
    return busy;
}

int uworker_pending(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_pending;
    pthread_mutex_unlock(&g_lock);
    return n;
}
