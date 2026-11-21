#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <gpiod.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SOCKET_PATH
#define SOCKET_PATH "/run/gateway-control.sock"
#endif
static volatile sig_atomic_t stopping;
static int reset_asserted, boot_value;
static struct gpiod_line *reset_line, *boot_line;

static void stop_signal(int sig) { (void)sig; stopping = 1; }
static int set_reset(int asserted)
{
    /* ACTIVE_LOW + OPEN_DRAIN: 1 drives NRST low; 0 releases to input. */
    if (gpiod_line_set_value(reset_line, asserted) < 0) return -1;
    reset_asserted = asserted;
    return 0;
}
static int set_boot(int value)
{
    if (gpiod_line_set_value(boot_line, value) < 0) return -1;
    boot_value = value;
    return 0;
}
static void delay_ms(long ms)
{
    struct timespec delay = { ms / 1000, (ms % 1000) * 1000000 };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) { }
}
static int reset_pulse(void)
{
    if (set_reset(1) < 0) return -1;
    delay_ms(20); /* Board software choice; verify NRST timing on hardware. */
    if (set_reset(0) < 0) return -1;
    delay_ms(100);
    return 0;
}
static int execute(const char *cmd)
{
    if (!strcmp(cmd, "status")) return 0;
    if (!strcmp(cmd, "normal")) {
        if (set_boot(0) < 0) return -1;
        return set_reset(0);
    }
    if (!strcmp(cmd, "disable")) {
        /* The 2026-10-05 board has direct UART/BOOT0 wiring, no PE4 gate. */
        errno = EOPNOTSUPP;
        return -1;
    }
    if (!strcmp(cmd, "reset")) return reset_pulse();
    if (!strcmp(cmd, "reset-low")) return set_reset(1);
    if (!strcmp(cmd, "reset-release")) return set_reset(0);
    if (!strcmp(cmd, "boot-low")) return set_boot(0);
    if (!strcmp(cmd, "boot-high")) return set_boot(1);
    if (!strcmp(cmd, "rom")) {
        if (set_boot(1) < 0) return -1;
        delay_ms(1);
        return reset_pulse();
    }
    errno = EINVAL;
    return -1;
}
static int serve(void)
{
    struct gpiod_chip *chip;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct pollfd waitfd;
    int server = -1, reset_requested = 0, boot_requested = 0, result = 1;
    reset_line = gpiod_line_find("F407_RESET_ASSERT");
    if (!reset_line) { perror("find F407_RESET_ASSERT (check gpioinfo/DTS)"); return 1; }
    chip = gpiod_line_get_chip(reset_line);
    boot_line = gpiod_chip_find_line(chip, "F407_BOOT0_SET");
    if (!boot_line) { perror("find F407_BOOT0_SET"); goto out; }
    if (gpiod_line_request_output_flags(reset_line, "gateway-control-reset",
            GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW | GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN |
            GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE, 0) < 0) {
        perror("request active-low open-drain NRST"); goto out;
    }
    reset_requested = 1;
    /* BOOT0 is push-pull and must not share NRST's electrical flags. */
    if (gpiod_line_request_output_flags(boot_line, "gateway-control-boot",
            GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE, 0) < 0) {
        perror("request BOOT0"); goto out;
    }
    boot_requested = 1;
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); goto out; }
    strcpy(address.sun_path, SOCKET_PATH);
    /* The GPIO request prevents a second instance from deleting a live socket. */
    if (unlink(SOCKET_PATH) < 0 && errno != ENOENT) { perror("unlink socket"); goto out; }
    umask(077);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 4) < 0) {
        perror("bind/listen"); goto out;
    }
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    waitfd.fd = server;
    waitfd.events = POLLIN;
    while (!stopping) {
        int client, n, status;
        char command[32], reply[160];
        struct timeval timeout = { 1, 0 };
        status = poll(&waitfd, 1, 500);
        if (status < 0) { if (errno == EINTR) continue; perror("poll"); goto out; }
        if (!status) continue;
        client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; perror("accept"); goto out; }
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        n = (int)recv(client, command, sizeof(command) - 1, 0);
        if (n > 0) {
            command[n] = '\0';
            command[strcspn(command, "\r\n")] = '\0';
            if (execute(command) < 0) {
                if (errno == EOPNOTSUPP)
                    snprintf(reply, sizeof(reply), "ERROR No target interface gate on this board; disable cannot isolate UART/BOOT0\n");
                else
                    snprintf(reply, sizeof(reply), "ERROR %s\n", strerror(errno));
            }
            else {
                int nrst = gpiod_line_get_value(reset_line), boot = gpiod_line_get_value(boot_line);
                snprintf(reply, sizeof(reply), "OK PD5(reset_request)=%d PD6(BOOT0)=%d NRST=%s NRST_sample=%d BOOT0_sample=%d\n",
                         reset_asserted, boot_value, reset_asserted ? "drive-low" : "released", nrst < 0 ? -1 : !nrst, boot);
            }
            (void)send(client, reply, strlen(reply), 0);
        }
        close(client);
    }
    result = 0;
out:
    if (server >= 0) { close(server); unlink(SOCKET_PATH); }
    if (reset_requested) {
        (void)set_reset(0);
        if (gpiod_line_set_config(reset_line, GPIOD_LINE_REQUEST_DIRECTION_INPUT,
                GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE, 0) < 0)
            perror("release NRST to input");
        gpiod_line_release(reset_line);
    }
    if (boot_requested) {
        /* A target-side BOOT0 pull-down is required while no process owns PD6. */
        if (gpiod_line_set_config(boot_line, GPIOD_LINE_REQUEST_DIRECTION_INPUT,
                GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE, 0) < 0)
            perror("release BOOT0 to input");
        gpiod_line_release(boot_line);
    }
    gpiod_chip_close(chip);
    return result;
}
static int client_command(const char *command)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    char reply[160];
    struct timeval timeout = { 3, 0 };
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    ssize_t count;
    if (sock < 0) { perror("socket"); return 1; }
    strcpy(address.sun_path, SOCKET_PATH);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("gateway daemon (check /etc/init.d/S45gateway-control)"); close(sock); return 1;
    }
    if (send(sock, command, strlen(command), 0) != (ssize_t)strlen(command)) {
        perror("send"); close(sock); return 1;
    }
    count = recv(sock, reply, sizeof(reply) - 1, 0);
    close(sock);
    if (count <= 0) { fputs("No daemon reply\n", stderr); return 1; }
    reply[count] = '\0';
    fputs(reply, stdout);
    return strncmp(reply, "OK ", 3) ? 1 : 0;
}
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s {daemon|status|normal|reset|rom|reset-low|reset-release|boot-low|boot-high}\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "daemon")) return serve();
    if (strcmp(argv[1], "status") && strcmp(argv[1], "normal") && strcmp(argv[1], "reset") && strcmp(argv[1], "rom") && strcmp(argv[1], "disable") &&
        strcmp(argv[1], "reset-low") && strcmp(argv[1], "reset-release") && strcmp(argv[1], "boot-low") && strcmp(argv[1], "boot-high")) {
        fputs("Unknown command\n", stderr); return 2;
    }
    return client_command(argv[1]);
}
