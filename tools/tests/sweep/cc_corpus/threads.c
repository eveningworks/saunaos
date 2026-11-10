/* POSIX threads: create and join, a mutex and condition variable, per-thread TLS, once. */
#include <pthread.h>
#include <stdio.h>

#define WORKERS 4
#define ROUNDS 20000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready;
static __thread int mine;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_runs;

static void init_once(void) { once_runs++; }

static void *worker(void *arg) {
    long id = (long)arg;
    pthread_once(&once, init_once);
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cv, &lock);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < ROUNDS; i++) {
        mine++;
        pthread_mutex_lock(&lock);
        counter += id + 1;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(long)(mine * 10 + id);
}

int main(void) {
    pthread_t t[WORKERS];
    for (long i = 0; i < WORKERS; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lock);
    long sum = 0;
    for (int i = 0; i < WORKERS; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    printf("threads counter %ld joined %ld once %d main-tls %d\n", counter, sum, once_runs, mine);
    return 0;
}
