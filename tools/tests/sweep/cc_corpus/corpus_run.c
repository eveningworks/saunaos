/* corpus_run -- the guest half of native_corpus_test.py: runs a job list,
 * each job under a time limit, and records how each one ended.
 *
 *   corpus_run JOBFILE RESDIR
 *
 * JOBFILE has one job per line, `<id> TAB <seconds> TAB <shell command>`;
 * each runs as `/bin/dash -c <command>`, its stdout and stderr in
 * RESDIR/<id>.out, and ends a line `<id> <status> <microseconds>` in
 * RESDIR/results.txt, flushed per job so a host can watch progress.
 * <status> is the exit code, `sig<N>` for a signal, `timeout`, or
 * `wait-failed:<errno>` when waitpid() itself refused. A
 * line RESDIR/DONE is written last.
 *
 * Test infrastructure, not under test: the host cross-compiles it.
 *
 * THE TRAP: a timeout kills the DASH this forked, not its children --
 * tolibc's kill() takes no process group. So a job whose last step may
 * hang (a test program) EXECs it: the program then IS that process. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void run_one(const char *resdir, const char *id, long secs, const char *cmd, FILE *res) {
    char out[1024];
    snprintf(out, sizeof out, "%s/%s.out", resdir, id);
    long long t0 = now_us();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(res, "%s fork-failed 0\n", id);
        fflush(res);
        return;
    }
    if (pid == 0) {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        char *argv[] = { "dash", "-c", (char *)cmd, NULL };
        execv("/bin/dash", argv);
        _exit(127);
    }
    long long deadline = t0 + (long long)secs * 1000000;
    int status = 0, timed_out = 0, wait_failed = 0, wait_errno = 0;
    for (;;) {
        errno = 0; // tolibc's waitpid() returns -1 without setting it
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR) {
            wait_failed = 1;   // a flag, not a status: any int is a status
            wait_errno = errno;
            break;
        }
        if (now_us() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = 1;
            break;
        }
        usleep(2000);
    }
    long long us = now_us() - t0;
    if (timed_out)
        fprintf(res, "%s timeout %lld\n", id, us);
    else if (wait_failed)
        fprintf(res, "%s wait-failed:%d %lld\n", id, wait_errno, us);
    else if (WIFSIGNALED(status))
        fprintf(res, "%s sig%d %lld\n", id, WTERMSIG(status), us);
    else
        fprintf(res, "%s %d %lld\n", id, WEXITSTATUS(status), us);
    fflush(res);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: corpus_run JOBFILE RESDIR\n");
        return 2;
    }
    FILE *jobs = fopen(argv[1], "r");
    if (!jobs) {
        perror(argv[1]);
        return 1;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/results.txt", argv[2]);
    FILE *res = fopen(path, "a");
    if (!res) {
        perror(path);
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, jobs) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2)
            continue;
        *t1 = *t2 = 0;
        run_one(argv[2], line, atol(t1 + 1), t2 + 1, res);
    }
    free(line);
    fclose(res);
    snprintf(path, sizeof path, "%s/DONE", argv[2]);
    FILE *d = fopen(path, "w");
    if (d) {
        fputs("done\n", d);
        fclose(d);
    }
    return 0;
}
