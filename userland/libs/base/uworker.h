#ifndef ULIB_UWORKER_H
#define ULIB_UWORKER_H

// A background thread for a program's blocking work: one per process,
// started on the first submit, running JOBS one at a time in the order
// they were asked for -- GIO's g_task_run_in_thread shape, with one
// thread instead of a pool. What it is for: a read or a decode that can
// wait out the disk or a volume lock, asked for by a loop that owes the
// next frame.
//
// A JOB IS THE CALLER'S, embedded in its own state, and goes IDLE ->
// QUEUED -> RUNNING -> DONE -> IDLE. Between submit and take the job's
// data belongs to `run`, on the worker; once uworker_take() has answered
// 1 it is the caller's again. A job is in flight at most once: a second
// submit while it is not IDLE is refused, and the caller asks again
// after its take.
//
// IF THE THREAD CANNOT START, a submit runs the job inline and leaves it
// DONE -- slower, never lost (said once a process).

struct uworker_job {
    void (*run)(struct uworker_job *job);   // on the worker; no uworker call inside
    int state;                              // UWORKER_* below, read under the worker's lock
    struct uworker_job *next;               // the queue's link
};

enum { UWORKER_IDLE, UWORKER_QUEUED, UWORKER_RUNNING, UWORKER_DONE };

// Queue `job` (its `run` set). 1 queued (or run inline), 0 refused: it is
// not IDLE.
int uworker_submit(struct uworker_job *job);

// 1 if `job` is DONE, and it is IDLE again; 0 otherwise.
int uworker_take(struct uworker_job *job);

// 1 while `job` is QUEUED, RUNNING or DONE and not yet taken.
int uworker_busy(struct uworker_job *job);

// Jobs not IDLE, across the process -- what a "is everything caught up"
// question (a test's sync) waits on.
int uworker_pending(void);

#endif // ULIB_UWORKER_H
