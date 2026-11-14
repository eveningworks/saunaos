// See base/uworker.h.
#include <pthread.h>
#include "base/uworker.h"
#include "rt/sys.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct uworker_job *g_head, *g_tail;
static int g_started;     // 1 running, -1 could not start (jobs run inline)
static int g_pending;     // jobs not IDLE

static void *worker(void *arg) {
    (void)arg;
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
