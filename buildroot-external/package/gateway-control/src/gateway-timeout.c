/* SPDX-License-Identifier: MIT
 * Bound a maintenance command and its process group without a board reboot.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { interrupted = sig; }
static double monotonic(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        perror("clock_gettime");
        exit(125);
    }
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

int main(int argc, char **argv)
{
    char *end;
    long seconds;
    pid_t child;
    struct sigaction sa = {0};
    int status = 0, stopped = 0, sent_kill = 0, result = 124;
    double deadline, kill_at = 0;
    const struct timespec tick = {0, 50000000};

    if (argc < 3) {
        fprintf(stderr, "Usage: gateway-timeout SECONDS COMMAND [ARG...]\n");
        return 125;
    }
    errno = 0;
    seconds = strtol(argv[1], &end, 10);
    if (errno || *end || end == argv[1] || seconds < 1 || seconds > 300)
        return 125;
    deadline = monotonic() + seconds;
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    child = fork();
    if (child < 0) {
        perror("fork");
        return 125;
    }
    if (child == 0) {
        if (setpgid(0, 0) < 0) _exit(125);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        execvp(argv[2], argv + 2);
        status = errno;
        perror("execvp");
        _exit(status == ENOENT ? 127 : 126);
    }
    /* Also establish the group in the parent to close the fork/exec race. */
    if (setpgid(child, child) < 0 && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 125;
    }
    for (;;) {
        siginfo_t info = {0};
        int observed = waitid(P_PID, (id_t)child, &info,
                              WEXITED | WNOHANG | WNOWAIT);
        double now = monotonic();
        if (!stopped && observed == 0 && info.si_pid == child) {
            waitpid(child, &status, 0);
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 125;
        }
        if (!stopped && (interrupted || now >= deadline)) {
            result = interrupted ? 128 + interrupted : 124;
            stopped = 1;
            kill_at = now + 2;
            kill(-child, SIGTERM);
        }
        if (stopped) {
            /* Keep the grace period even if the leader exits; its children
             * may still be running. Never wait forever on kernel D-state. */
            if (!sent_kill && now >= kill_at) {
                kill(-child, SIGKILL);
                sent_kill = 1;
                kill_at = now + 1;
            }
            if (sent_kill && (info.si_pid == child || observed < 0 || now >= kill_at)) {
                waitpid(child, NULL, WNOHANG);
                return result;
            }
        } else if (observed < 0 && errno != EINTR) {
            return 125;
        }
        nanosleep(&tick, NULL);
    }
}
