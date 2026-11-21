#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Bounded binary upload over SSH stdin or the UART0 maintenance console. */
static struct termios saved;
static int restore_tty;
static volatile sig_atomic_t stopped;
static void restore(void) { if (restore_tty) tcsetattr(0, TCSANOW, &saved); }
static void signal_stop(int sig) { (void)sig; stopped = 1; }
static long seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }
int main(int argc, char **argv)
{
    char part[256], *end;
    unsigned char buf[4096];
    long length, done = 0, deadline;
    int fd, result = 1;
    struct pollfd input = { 0, POLLIN, 0 };
    if (argc != 4 || strncmp(argv[1], "/run/gw-upload-", 15) || strchr(argv[1] + 5, '/')) {
        fputs("Usage: gateway-upload /run/gw-upload-NAME BYTES TOKEN\n", stderr); return 2;
    }
    length = strtol(argv[2], &end, 10);
    if (*end || length < 1 || length > 8 * 1024 * 1024 || strlen(argv[1]) > 180 ||
        strlen(argv[3]) > 64 || strspn(argv[3], "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_") != strlen(argv[3])) return 2;
    snprintf(part, sizeof(part), "%s.part", argv[1]);
    fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open upload"); return 1; }
    atexit(restore);
    if (isatty(0)) {
        struct termios raw;
        if (tcgetattr(0, &saved) < 0) goto out;
        raw = saved;
        raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        raw.c_oflag &= ~OPOST;
        raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw.c_cflag &= ~(CSIZE | PARENB); raw.c_cflag |= CS8;
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &raw) < 0) goto out;
        restore_tty = 1;
    }
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop); signal(SIGHUP, signal_stop);
    deadline = seconds() + 20 + length / 6000;
    printf("\n__GW_READY_%s__\n", argv[3]); fflush(stdout);
    while (done < length && !stopped && seconds() < deadline) {
        ssize_t n, written = 0;
        int ready = poll(&input, 1, 500);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (input.revents & (POLLERR | POLLNVAL))) break;
        if (!ready) continue;
        n = read(0, buf, (size_t)(length - done < (long)sizeof(buf) ? length - done : (long)sizeof(buf)));
        if (n <= 0) break;
        while (written < n) {
            ssize_t w = write(fd, buf + written, (size_t)(n - written));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) goto out;
            written += w;
        }
        done += n;
    }
    if (done == length && fsync(fd) == 0 && rename(part, argv[1]) == 0) result = 0;
out:
    close(fd); if (result) unlink(part);
    restore(); restore_tty = 0;
    printf("\n__GW_STORED_%s__:%d:%ld\n", argv[3], result, done);
    fflush(stdout); return result;
}
