#include "gw-common.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

uint64_t gw_mono_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (unsigned)t.tv_nsec / 1000;
}
uint64_t gw_utc_us(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    if (t.tv_sec < 1704067200) return 0;
    return (uint64_t)t.tv_sec * 1000000 + (unsigned)t.tv_nsec / 1000;
}
void gw_iso(uint64_t micros, char *out, size_t size) {
    struct tm t; time_t seconds = (time_t)(micros / 1000000);
    if (!micros) { if (size) *out = 0; return; }
    gmtime_r(&seconds, &t); strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &t);
}
int gw_atomic(const char *path, const void *data, size_t size) {
    char tmp[512]; size_t done = 0; int fd, result = -1;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd < 0) return -1;
    while (done < size) {
        ssize_t n = write(fd, (const char *)data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto out;
        done += (size_t)n;
    }
    if (fsync(fd) == 0 && rename(tmp, path) == 0) {
        char parent[512], *slash; int directory;
        snprintf(parent, sizeof(parent), "%s", path); slash = strrchr(parent, '/');
        if (slash) { *slash = 0; directory = open(parent, O_RDONLY | O_DIRECTORY);
            if (directory >= 0) { fsync(directory); close(directory); }
        }
        result = 0;
    }
out:
    close(fd); if (result) unlink(tmp); return result;
}
char *gw_read(const char *path, size_t max) {
    FILE *f = fopen(path, "rb"); char *s; size_t n;
    if (!f) return NULL;
    s = malloc(max + 1); if (!s) { fclose(f); return NULL; }
    n = fread(s, 1, max, f); s[n] = 0; fclose(f); return s;
}
int gw_prepare(void) {
    const char *dirs[] = {"/data", "/data/gateway", GW_LOGDIR, GW_AUDIT, GW_PROFILES, GW_HISTORY};
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (mkdir(dirs[i], 0700) < 0 && errno != EEXIST) return -1;
    return 0;
}
int gw_baud(unsigned baud) {
    switch (baud) {
        case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200: return B115200; case 230400: return B230400;
        case 460800: return B460800; case 921600: return B921600; default: return -1;
    }
}
int gw_uart_raw(int fd, unsigned baud) {
    struct termios t; int speed = gw_baud(baud);
    if (speed < 0 || tcgetattr(fd, &t) < 0) return -1;
    t.c_iflag = 0; t.c_oflag = 0; t.c_lflag = 0;
    t.c_cflag = CLOCAL | CREAD | CS8;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    cfsetispeed(&t, (speed_t)speed); cfsetospeed(&t, (speed_t)speed);
    return tcsetattr(fd, TCSANOW, &t);
}
void gw_config(GwConfig *cfg) {
    FILE *f; char line[160], key[40], value[100];
    memset(cfg, 0, sizeof(*cfg)); strcpy(cfg->mode, "files");
    cfg->files = 10; cfg->bytes = 32 * 1024 * 1024; cfg->segment = 512 * 1024;
    cfg->baud[0] = cfg->baud[1] = 115200;
    f = fopen(GW_CONFIG, "r"); if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%39[^=]=%99s", key, value) != 2) continue;
        if (!strcmp(key, "mode") && (!strcmp(value, "files") || !strcmp(value, "bytes") || !strcmp(value, "none"))) strcpy(cfg->mode, value);
        else if (!strcmp(key, "files")) cfg->files = (unsigned)strtoul(value, NULL, 10);
        else if (!strcmp(key, "bytes")) cfg->bytes = strtoull(value, NULL, 10);
        else if (!strcmp(key, "segment")) cfg->segment = (unsigned)strtoul(value, NULL, 10);
        else if (!strcmp(key, "baud1")) cfg->baud[0] = (unsigned)strtoul(value, NULL, 10);
        else if (!strcmp(key, "baud2")) cfg->baud[1] = (unsigned)strtoul(value, NULL, 10);
    }
    fclose(f);
    if (cfg->files < 1 || cfg->files > 1000) cfg->files = 10;
    if (cfg->bytes < 65536 || cfg->bytes > 80 * 1024 * 1024) cfg->bytes = 32 * 1024 * 1024;
    if (cfg->segment < 4096 || cfg->segment > 1024 * 1024) cfg->segment = 512 * 1024;
    if (gw_baud(cfg->baud[0]) < 0) cfg->baud[0] = 115200;
    if (gw_baud(cfg->baud[1]) < 0) cfg->baud[1] = 115200;
}
int gw_config_save(const GwConfig *cfg) {
    char text[256]; int n = snprintf(text, sizeof(text),
        "mode=%s\nfiles=%u\nbytes=%llu\nsegment=%u\nbaud1=%u\nbaud2=%u\n",
        cfg->mode, cfg->files, (unsigned long long)cfg->bytes, cfg->segment, cfg->baud[0], cfg->baud[1]);
    return gw_atomic(GW_CONFIG, text, (size_t)n);
}
int gw_log_name(const char *name) {
    const char *uart; char *end; unsigned long port;
    if (strlen(name) > 60 || strspn(name, "0123456789-uaritn.b") != strlen(name)) return 0;
    uart = strstr(name, "-uart");
    if (!uart || uart == name || strspn(name, "0123456789") != (size_t)(uart - name)) return 0;
    port = strtoul(uart + 5, &end, 10);
    return (port == 1 || port == 2) && !strcmp(end, ".bin");
}
int gw_path(char *out, size_t size, const char *name, const char *extension) {
    size_t len = strlen(name);
    if (!gw_log_name(name) || len < 4) return -1;
    return snprintf(out, size, "%s/%.*s%s", GW_LOGDIR, (int)len - 4, name, extension) >= (int)size ? -1 : 0;
}
static int compare_files(const void *a, const void *b) {
    const GwFile *x = a, *y = b; return x->id < y->id ? -1 : x->id > y->id;
}
size_t gw_files(GwFile **files) {
    DIR *d = opendir(GW_LOGDIR); struct dirent *entry; size_t n = 0, capacity = 32;
    GwFile *list; char path[256]; struct stat st;
    *files = NULL; if (!d) return 0;
    list = malloc(capacity * sizeof(*list)); if (!list) { closedir(d); return 0; }
    while ((entry = readdir(d))) {
        unsigned long long id; unsigned port; const char *exts[] = {".bin", ".idx", ".meta"}; size_t k;
        if (!gw_log_name(entry->d_name)) continue;
        if (n == capacity) {
            GwFile *grown; if (capacity >= 16384) break;
            capacity *= 2; grown = realloc(list, capacity * sizeof(*list)); if (!grown) break; list = grown;
        }
        sscanf(entry->d_name, "%llu-uart%u.bin", &id, &port);
        memset(&list[n], 0, sizeof(list[n])); list[n].id = id; list[n].port = port;
        memcpy(list[n].name, entry->d_name, strlen(entry->d_name) + 1);
        for (k = 0; k < 3; k++) {
            gw_path(path, sizeof(path), entry->d_name, exts[k]);
            if (stat(path, &st) == 0) list[n].bytes += (uint64_t)st.st_size;
        }
        n++;
    }
    closedir(d); qsort(list, n, sizeof(*list), compare_files); *files = list; return n;
}
int gw_prune(const GwConfig *cfg, const char *active1, const char *active2) {
    int deleted = 0, pass;
    for (pass = 0; pass < 4; pass++) {
        GwFile *list; size_t n = gw_files(&list), i; uint64_t bytes = 0; unsigned counts[2] = {0, 0};
        for (i = 0; i < n; i++) { bytes += list[i].bytes; counts[list[i].port - 1]++; }
        for (i = 0; i < n; i++) {
            int remove = (!strcmp(cfg->mode, "bytes") && bytes > cfg->bytes) ||
                (!strcmp(cfg->mode, "files") && counts[list[i].port - 1] > cfg->files);
            if (remove && strcmp(list[i].name, active1) && strcmp(list[i].name, active2)) {
                const char *exts[] = {".bin", ".idx", ".meta"}; size_t k; char path[256];
                for (k = 0; k < 3; k++) { gw_path(path, sizeof(path), list[i].name, exts[k]); unlink(path); }
                deleted++; break;
            }
        }
        free(list); if (i == n) break;
    }
    return deleted;
}
