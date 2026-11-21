#include "gw-common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    int uart, raw, index; struct termios saved; char name[64];
    unsigned char buffer[32768]; unsigned used, stamps_used;
    GwStamp stamps[512]; uint64_t raw_size, segment_size, rx, dropped, last_sync;
    int error;
} Port;
static volatile sig_atomic_t stopping, reload;
static uint64_t sequence;
static char boot_id[80];
static GwConfig config;
static void signal_event(int sig) { if (sig == SIGHUP) reload = 1; else stopping = 1; }
static int write_all(int fd, const void *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, (const char *)data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}
static int open_segment(Port *port, unsigned number) {
    char path[256], meta[512], iso[40], value[40]; int n;
    if (port->raw >= 0) { fsync(port->raw); fsync(port->index); close(port->raw); close(port->index); }
    port->raw = port->index = -1; port->name[0] = 0;
    ++sequence; snprintf(value, sizeof(value), "%llu\n", (unsigned long long)sequence);
    if (gw_atomic(GW_LOGDIR "/.sequence", value, strlen(value)) < 0) return -1;
    snprintf(port->name, sizeof(port->name), "%020llu-uart%u.bin", (unsigned long long)sequence, number);
    gw_path(path, sizeof(path), port->name, ".bin"); port->raw = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    gw_path(path, sizeof(path), port->name, ".idx"); port->index = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (port->raw < 0 || port->index < 0) { if (port->raw >= 0) close(port->raw); if (port->index >= 0) close(port->index); port->raw = port->index = -1; return -1; }
    gw_iso(gw_utc_us(), iso, sizeof(iso));
    n = snprintf(meta, sizeof(meta), "{\"uart\":%u,\"boot\":\"%s\",\"created_utc\":\"%s\",\"created_uptime\":%.3f,\"baud\":%u,\"index_format\":\"le-u64-offset,u64-monotonic-us,u64-utc-us,u32-count,u32-reserved\"}\n",
        number, boot_id, iso, gw_mono_us() / 1000000.0, config.baud[number - 1]);
    gw_path(path, sizeof(path), port->name, ".meta");
    if (gw_atomic(path, meta, (size_t)n) < 0) {
        close(port->raw); close(port->index); port->raw = port->index = -1; port->name[0] = 0; return -1;
    }
    port->raw_size = port->segment_size = 0; port->last_sync = gw_mono_us(); return 0;
}
static void flush_port(Port *port, unsigned number) {
    struct statvfs disk; size_t i; uint64_t base;
    if (!port->used) return;
    if (statvfs(GW_LOGDIR, &disk) < 0 || (uint64_t)disk.f_bavail * disk.f_frsize < 8 * 1024 * 1024) {
        port->dropped += port->used; port->used = port->stamps_used = 0; port->error = ENOSPC; return;
    }
    if (port->raw < 0 || port->segment_size >= config.segment) {
        if (open_segment(port, number) < 0) { port->error = errno ? errno : EIO; port->dropped += port->used; port->used = port->stamps_used = 0; return; }
    }
    base = port->raw_size;
    if (write_all(port->raw, port->buffer, port->used) < 0) { port->error = errno; goto failed; }
    for (i = 0; i < port->stamps_used; i++) port->stamps[i].offset += base;
    if (write_all(port->index, port->stamps, port->stamps_used * sizeof(GwStamp)) < 0) { port->error = errno; goto failed; }
    port->raw_size += port->used; port->segment_size += port->used + port->stamps_used * sizeof(GwStamp);
    port->used = port->stamps_used = 0; port->error = 0;
    if (gw_mono_us() - port->last_sync >= 5000000) {
        if (fsync(port->raw) < 0 || fsync(port->index) < 0) port->error = errno;
        port->last_sync = gw_mono_us();
    }
    return;
failed:
    port->dropped += port->used; port->used = port->stamps_used = 0;
    close(port->raw); close(port->index); port->raw = port->index = -1;
}
static void status(Port ports[2]) {
    GwFile *list; size_t i, n = gw_files(&list); uint64_t bytes = 0; unsigned counts[2] = {0, 0};
    char json[1400]; int len;
    for (i = 0; i < n; i++) { bytes += list[i].bytes; counts[list[i].port - 1]++; }
    free(list);
    len = snprintf(json, sizeof(json), "{\"pid\":%ld,\"boot\":\"%s\",\"mode\":\"%s\",\"limit_files_per_uart\":%u,\"limit_bytes_total\":%llu,\"segment_bytes\":%u,\"baud\":[%u,%u],\"files\":[%u,%u],\"stored_bytes\":%llu,\"received\":[%llu,%llu],\"buffered\":[%u,%u],\"dropped\":[%llu,%llu],\"error\":[%d,%d],\"active\":[\"%s\",\"%s\"],\"flush_ms\":1000,\"sync_seconds\":5}\n",
        (long)getpid(), boot_id, config.mode, config.files, (unsigned long long)config.bytes, config.segment, config.baud[0], config.baud[1], counts[0], counts[1], (unsigned long long)bytes,
        (unsigned long long)ports[0].rx, (unsigned long long)ports[1].rx, ports[0].used, ports[1].used, (unsigned long long)ports[0].dropped, (unsigned long long)ports[1].dropped,
        ports[0].error, ports[1].error, ports[0].name, ports[1].name);
    /* RAM status needs no Flash fsync. */
    { FILE *f = fopen(GW_STATS ".tmp", "w"); if (f) { fwrite(json, 1, (size_t)len, f); fclose(f); rename(GW_STATS ".tmp", GW_STATS); } }
}
static int discovery_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0), yes = 1;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(32137), .sin_addr.s_addr = INADDR_ANY };
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(fd); return -1; }
    return fd;
}
static int control_socket(void) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (fd < 0) return -1;
    strcpy(address.sun_path, "/run/gateway-logs.sock"); unlink(address.sun_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(fd); return -1; }
    chmod(address.sun_path, 0600); return fd;
}
static void log_control(int fd, Port ports[2]) {
    char command[100], path[256]; struct sockaddr_un peer; socklen_t length = sizeof(peer);
    const char *response = "日志操作无效"; ssize_t n = recvfrom(fd, command, sizeof(command) - 1, 0, (struct sockaddr *)&peer, &length);
    unsigned i;
    if (n <= 0) return;
    command[n] = 0;
    if (!strncmp(command, "delete ", 7) && gw_log_name(command + 7)) {
        const char *name = command + 7; int exists;
        gw_path(path, sizeof(path), name, ".bin"); exists = access(path, F_OK) == 0;
        for (i = 0; i < 2; i++) if (!strcmp(ports[i].name, name)) {
            /* Pending bytes belong to the explicitly deleted active file. */
            ports[i].used = ports[i].stamps_used = 0;
            if (ports[i].raw >= 0) { close(ports[i].raw); close(ports[i].index); }
            ports[i].raw = ports[i].index = -1; ports[i].name[0] = 0;
            ports[i].raw_size = ports[i].segment_size = 0;
        }
        if (!exists) response = "日志文件不存在";
        else if (unlink(path) < 0) response = "日志删除失败";
        else {
            const char *extensions[] = {".idx", ".meta"};
            response = "OK";
            for (i = 0; i < 2; i++) { gw_path(path, sizeof(path), name, extensions[i]); if (unlink(path) < 0 && errno != ENOENT) response = "部分索引删除失败"; }
            { int directory = open(GW_LOGDIR, O_RDONLY | O_DIRECTORY); if (directory >= 0) { if (fsync(directory) < 0) response = "日志已删除，但目录同步失败"; close(directory); } }
            status(ports);
        }
    }
    sendto(fd, response, strlen(response), MSG_DONTWAIT, (struct sockaddr *)&peer, length);
}
int main(void) {
    Port ports[2]; struct pollfd waitfds[4]; uint64_t last_flush, last_status; unsigned i;
    int lock = open("/run/gateway-logger.lock", O_RDWR | O_CREAT, 0600), discovery, control;
    char *boot;
    _Static_assert(sizeof(GwStamp) == 32, "Timestamp index layout changed");
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) return 1;
    if (gw_prepare() < 0) return 1;
    gw_config(&config); boot = gw_read("/proc/sys/kernel/random/boot_id", 70);
    if (boot) { boot[strcspn(boot, "\r\n")] = 0; snprintf(boot_id, sizeof(boot_id), "%s", boot); free(boot); }
    { GwFile *files; char *saved; size_t n = gw_files(&files);
        if (n) sequence = files[n - 1].id;
        free(files);
        saved = gw_read(GW_LOGDIR "/.sequence", 40);
        if (saved) { uint64_t previous = strtoull(saved, NULL, 10); if (previous > sequence) sequence = previous; free(saved); }
    }
    memset(ports, 0, sizeof(ports));
    for (i = 0; i < 2; i++) {
        char device[30]; ports[i].raw = ports[i].index = -1;
        snprintf(device, sizeof(device), "/dev/ttyS%u", i + 1);
        ports[i].uart = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (ports[i].uart < 0 || tcgetattr(ports[i].uart, &ports[i].saved) < 0 || gw_uart_raw(ports[i].uart, config.baud[i]) < 0) { perror(device); return 1; }
        waitfds[i].fd = ports[i].uart; waitfds[i].events = POLLIN;
    }
    discovery = discovery_socket(); waitfds[2].fd = discovery; waitfds[2].events = POLLIN;
    control = control_socket(); waitfds[3].fd = control; waitfds[3].events = POLLIN;
    if (control < 0) { perror("logger control socket"); return 1; }
    signal(SIGTERM, signal_event); signal(SIGINT, signal_event); signal(SIGHUP, signal_event);
    last_flush = last_status = gw_mono_us();
    while (!stopping) {
        int ready = poll(waitfds, 4, 100);
        if (ready < 0 && errno != EINTR) break;
        if (reload) {
            GwConfig next;
            reload = 0;
            for (i = 0; i < 2; i++) flush_port(&ports[i], i + 1);
            gw_config(&next);
            for (i = 0; i < 2; i++) if (next.baud[i] != config.baud[i]) {
                if (ports[i].raw >= 0) { fsync(ports[i].raw); fsync(ports[i].index); close(ports[i].raw); close(ports[i].index); }
                ports[i].raw = ports[i].index = -1; ports[i].name[0] = 0;
            }
            config = next;
            for (i = 0; i < 2; i++) if (gw_uart_raw(ports[i].uart, config.baud[i]) < 0) ports[i].error = errno;
        }
        for (i = 0; i < 2; i++) if (waitfds[i].revents & POLLIN) {
            Port *port = &ports[i]; ssize_t n;
            if (port->used > sizeof(port->buffer) - 4096 || port->stamps_used >= 512) flush_port(port, i + 1);
            n = read(port->uart, port->buffer + port->used, sizeof(port->buffer) - port->used);
            if (n > 0) {
                GwStamp *stamp = &port->stamps[port->stamps_used++];
                stamp->offset = port->used; stamp->count = (uint32_t)n; stamp->reserved = 0;
                stamp->monotonic_us = gw_mono_us(); stamp->utc_us = gw_utc_us();
                port->used += (unsigned)n; port->rx += (uint64_t)n;
            }
        }
        if (discovery >= 0 && (waitfds[2].revents & POLLIN)) {
            char query[100]; struct sockaddr_in peer; socklen_t size = sizeof(peer);
            ssize_t n = recvfrom(discovery, query, sizeof(query) - 1, 0, (struct sockaddr *)&peer, &size);
            if (n > 0) { query[n] = 0;
                if (!strcmp(query, "F1C_GATEWAY_DISCOVER_V1")) {
                    const char response[] = "{\"service\":\"f1c200s-gateway\",\"hostname\":\"f1c200s-gateway.local\",\"port\":80}";
                    sendto(discovery, response, sizeof(response) - 1, 0, (struct sockaddr *)&peer, size);
                }
            }
        }
        if (waitfds[3].revents & POLLIN) log_control(control, ports);
        if (gw_mono_us() - last_flush >= 1000000) {
            for (i = 0; i < 2; i++) flush_port(&ports[i], i + 1);
            for (i = 0; i < 2; i++) if (ports[i].raw >= 0 && gw_mono_us() - ports[i].last_sync >= 5000000) {
                if (fsync(ports[i].raw) < 0 || fsync(ports[i].index) < 0) ports[i].error = errno;
                ports[i].last_sync = gw_mono_us();
            }
            gw_prune(&config, ports[0].name, ports[1].name); last_flush = gw_mono_us();
        }
        if (gw_mono_us() - last_status >= 1000000) { status(ports); last_status = gw_mono_us(); }
    }
    for (i = 0; i < 2; i++) {
        flush_port(&ports[i], i + 1);
        if (ports[i].raw >= 0) { fsync(ports[i].raw); fsync(ports[i].index); close(ports[i].raw); close(ports[i].index); }
        tcsetattr(ports[i].uart, TCSANOW, &ports[i].saved); close(ports[i].uart);
    }
    close(control); unlink("/run/gateway-logs.sock");
    close(lock); if (discovery >= 0) close(discovery); return 0;
}
