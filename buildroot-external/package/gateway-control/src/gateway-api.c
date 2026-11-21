#include "gw-common.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct { const char *key, *value; } Param;
static Param params[48]; static unsigned param_count;
static int cli_mode, response_exit, cli_wait_seconds;
static const char *request_operation, *request_method;
static int record_response;
static uint64_t record_event(const char *operation, const char *source, int status, const cJSON *result);
static void cache_command(const char *path, const char *command);
#define USB_POWER_STATE "/sys/bus/platform/devices/usb-power-control/state"
static const char *default_target(void) {
    static char target[8]; FILE *f = fopen("/etc/gateway/target.conf", "r");
    strcpy(target, "f103");
    if (f) { char text[20]; if (fgets(text, sizeof(text), f)) { text[strcspn(text, "\r\n")] = 0; if (!strcmp(text, "f103") || !strcmp(text, "f407")) strcpy(target, text); } fclose(f); }
    return target;
}
static void set_param(const char *key, const char *text) {
    unsigned i; for (i = 0; i < param_count; i++) if (!strcmp(params[i].key, key)) { params[i].value = text; return; }
    if (param_count < 48) { params[param_count].key = key; params[param_count++].value = text; }
}
static const char *value(const char *key) {
    unsigned i; for (i = 0; i < param_count; i++) if (!strcmp(params[i].key, key)) return params[i].value;
    return "";
}
static void decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && r[1] && r[2] && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], 0}; *w++ = (char)strtoul(hex, NULL, 16); r += 3;
        } else { *w++ = *r == '+' ? ' ' : *r; r++; }
    }
    *w = 0;
}
static void parse(char *text) {
    char *next = text;
    while (next && *next && param_count < 48) {
        char *entry = next, *equals; next = strchr(next, '&'); if (next) *next++ = 0;
        equals = strchr(entry, '='); if (!equals) continue; *equals++ = 0;
        decode(entry); decode(equals); params[param_count].key = entry; params[param_count++].value = equals;
    }
}
static void headers(int status, const char *type) {
    if (cli_mode) return;
    printf("Status: %d\r\nContent-Type: %s\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n", status, type);
}
static void json_response(int status, cJSON *json) {
    char *s;
    if (!cJSON_GetObjectItem(json, "api_version")) cJSON_AddNumberToObject(json, "api_version", 1);
    if (!cJSON_GetObjectItem(json, "ok")) cJSON_AddBoolToObject(json, "ok", status < 400);
    if (!cJSON_GetObjectItem(json, "code")) cJSON_AddStringToObject(json, "code", status < 400 ? "ok" : status == 409 ? "busy_or_conflict" : status == 503 ? "service_unavailable" : status == 501 ? "protocol_not_ready" : status >= 500 ? "internal_error" : "invalid_request");
    if (record_response && request_operation) {
        uint64_t sequence = record_event(request_operation, cli_mode ? "cli" : "http", status, json);
        cJSON_AddBoolToObject(json, "history_saved", sequence != 0);
        if (sequence) cJSON_AddNumberToObject(json, "activity_sequence", (double)sequence);
        record_response = 0;
    }
    response_exit = status >= 400 ? 1 : 0;
    s = cJSON_PrintUnformatted(json); headers(status, "application/json; charset=utf-8"); printf("%s%s\n", cli_mode ? "" : "\r\n", s ? s : "{}");
    free(s); cJSON_Delete(json);
}
static void error_response(int status, const char *text) {
    cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "error", text); json_response(status, j);
}
static cJSON *json_file(const char *path) {
    char *s = gw_read(path, 131072); cJSON *j = s ? cJSON_Parse(s) : NULL; free(s); return j;
}
static int save_json(const char *path, const cJSON *json) {
    char *s = cJSON_PrintUnformatted(json); int result = s ? gw_atomic(path, s, strlen(s)) : -1; free(s); return result;
}
static char *command_result(const char *command, int *exit_code) {
    FILE *f = popen(command, "r"); char *s = calloc(1, 8193); size_t n;
    int status;
    if (exit_code) *exit_code = 127;
    if (!s) return NULL;
    if (!f) return s;
    n = fread(s, 1, 8192, f); s[n] = 0;
    { char discard[1024]; while (fread(discard, 1, sizeof(discard), f)) {} }
    status = pclose(f); if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return s;
}
static char *command_text(const char *command) { return command_result(command, NULL); }
static void add_cached(cJSON *j, const char *key, const char *path) {
    char *s = gw_read(path, 8192); cJSON_AddStringToObject(j, key, s ? s : ""); free(s);
}
static char *control_text(const char *command) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval timeout = { 2, 0 }; char buffer[256]; ssize_t n = -1;
    if (fd < 0) return NULL;
    strcpy(address.sun_path, "/run/gateway-control.sock");
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && send(fd, command, strlen(command), MSG_NOSIGNAL) > 0)
        n = recv(fd, buffer, sizeof(buffer) - 1, 0);
    close(fd); if (n <= 0) return NULL; buffer[n] = 0; return strdup(buffer);
}
static cJSON *gpio_object(const char *reply) {
    cJSON *j = cJSON_CreateObject(); const char *field; int number;
    cJSON_AddStringToObject(j, "scope", "F4_connector"); cJSON_AddBoolToObject(j, "target_execution_confirmed", 0);
    field = strstr(reply ? reply : "", "PD5(reset_request)="); if (field && sscanf(field, "PD5(reset_request)=%d", &number) == 1) cJSON_AddBoolToObject(j, "nrst_low_requested", number);
    field = strstr(reply ? reply : "", "PD6(BOOT0)="); if (field && sscanf(field, "PD6(BOOT0)=%d", &number) == 1) cJSON_AddNumberToObject(j, "boot0_requested", number);
    field = strstr(reply ? reply : "", "NRST_sample="); if (field && sscanf(field, "NRST_sample=%d", &number) == 1 && number >= 0) cJSON_AddNumberToObject(j, "nrst_sample", number);
    field = strstr(reply ? reply : "", "BOOT0_sample="); if (field && sscanf(field, "BOOT0_sample=%d", &number) == 1 && number >= 0) cJSON_AddNumberToObject(j, "boot0_sample", number);
    return j;
}
static int usb_power_state(void) {
    char *text = gw_read(USB_POWER_STATE, 32); int state = -1;
    if (text) { if (!strncmp(text, "enabled", 7)) state = 1; else if (!strncmp(text, "disabled", 8)) state = 0; free(text); }
    return state;
}
static cJSON *usb_power_object(void) {
    cJSON *j = cJSON_CreateObject(); int state = usb_power_state();
    cJSON_AddStringToObject(j, "pin", "PD7"); cJSON_AddBoolToObject(j, "available", state >= 0);
    if (state >= 0) cJSON_AddBoolToObject(j, "enabled", state);
    return j;
}
static int safe_id(const char *s) { return strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32; }
static void new_id(char id[33]) {
    unsigned char bytes[16]; unsigned i; int f = open("/dev/urandom", O_RDONLY);
    if (f < 0 || read(f, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) { fprintf(stderr, "random source unavailable\n"); exit(1); }
    close(f); for (i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", bytes[i]);
}
#include "gateway-history.inc"
#include "gateway-f407.inc"
static cJSON *status_object(void) {
    if (cli_mode) {
        cache_command("/run/gateway-wifi-status", "/bin/busybox timeout 3 wpa_cli -i wlan0 status 2>/dev/null");
        cache_command("/run/gateway-usb-status", "/bin/busybox timeout 3 lsusb 2>/dev/null");
    }
    cJSON *j = cJSON_CreateObject(), *logger = json_file(GW_STATS); struct sysinfo system; struct statvfs disk;
    struct ifaddrs *addresses = NULL, *p; char iso[40], *token = gw_read(GW_TOKEN, 100);
    gw_iso(gw_utc_us(), iso, sizeof(iso)); cJSON_AddStringToObject(j, "utc", iso);
    cJSON_AddBoolToObject(j, "time_valid", gw_utc_us() != 0); cJSON_AddNumberToObject(j, "uptime", gw_mono_us() / 1000000.0);
    cJSON_AddStringToObject(j, "hostname", "f1c200s-gateway.local"); cJSON_AddStringToObject(j, "version", "1.3.0");
    cJSON_AddNumberToObject(j, "api_version", 1); cJSON_AddBoolToObject(j, "ok", 1);
    cJSON_AddStringToObject(j, "configured_target", default_target());
    cJSON_AddItemToObject(j,"f407",mcu_cached());
    { cJSON *recording=json_file("/run/gateway-inertial-record.json"); cJSON_AddItemToObject(j,"inertial_recording",recording?recording:cJSON_CreateNull()); }
    cJSON_AddItemToObject(j, "usb_power", usb_power_object());
    { cJSON *units = cJSON_CreateObject(); cJSON_AddStringToObject(units, "memory_storage", "bytes"); cJSON_AddStringToObject(units, "uptime", "seconds"); cJSON_AddStringToObject(units, "utc", "ISO8601_UTC"); cJSON_AddItemToObject(j, "units", units); }
    { char *boot = gw_read("/proc/sys/kernel/random/boot_id", 80); if (boot) { boot[strcspn(boot, "\r\n")] = 0; cJSON_AddStringToObject(j, "boot_id", boot); free(boot); } }
    if (token) { token[strcspn(token, "\r\n")] = 0; cJSON_AddStringToObject(j, "token", token); free(token); }
    if (sysinfo(&system) == 0) {
        cJSON_AddNumberToObject(j, "ram_total", (double)system.totalram * system.mem_unit);
        cJSON_AddNumberToObject(j, "ram_free", (double)system.freeram * system.mem_unit);
        cJSON_AddNumberToObject(j, "load", system.loads[0] / 65536.0);
    }
    if (statvfs("/data", &disk) == 0) {
        cJSON_AddNumberToObject(j, "flash_total", (double)disk.f_blocks * disk.f_frsize);
        cJSON_AddNumberToObject(j, "flash_free", (double)disk.f_bavail * disk.f_frsize);
    }
    if (getifaddrs(&addresses) == 0) {
        for (p = addresses; p; p = p->ifa_next) if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, "wlan0")) {
            char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, sizeof(ip));
            cJSON_AddStringToObject(j, "ip", ip); break;
        }
        freeifaddrs(addresses);
    }
    {
        cJSON *pid = logger ? cJSON_GetObjectItem(logger, "pid") : NULL;
        cJSON_AddBoolToObject(j, "logger_alive", cJSON_IsNumber(pid) && pid->valueint > 1 && kill((pid_t)pid->valueint, 0) == 0);
    }
    cJSON_AddItemToObject(j, "logger", logger ? logger : cJSON_CreateNull());
    add_cached(j, "wifi", "/run/gateway-wifi-status");
    add_cached(j, "usb", "/run/gateway-usb-status");
    { char *control = control_text("status"); cJSON_AddStringToObject(j, "control", control ? control : ""); cJSON_AddItemToObject(j, "gpio", gpio_object(control)); free(control); }
    { char *wifi = gw_read("/run/gateway-wifi-status", 8192), *usb = gw_read("/run/gateway-usb-status", 8192), *next;
        cJSON *network = cJSON_CreateObject(), *stlink = cJSON_CreateObject();
        for (next = wifi; next && *next;) {
            char *line = next, *equals; next = strchr(next, '\n'); if (next) *next++ = 0;
            equals = strchr(line, '='); if (equals) {
                *equals++ = 0;
                if (!strcmp(line, "ssid")) {
                    char *readp = equals, *writep = equals;
                    while (*readp) {
                        if (readp[0] == '\\' && readp[1] == 'x' && readp[2] && readp[3] && isxdigit((unsigned char)readp[2]) && isxdigit((unsigned char)readp[3])) {
                            char hex[3] = {readp[2], readp[3], 0}; *writep++ = (char)strtoul(hex, NULL, 16); readp += 4;
                        } else *writep++ = *readp++;
                    }
                    *writep = 0;
                }
                cJSON_AddStringToObject(network, line, equals);
            }
        }
        cJSON_AddItemToObject(j, "network", network); cJSON_AddBoolToObject(stlink, "present", usb && strstr(usb, "0483:3748"));
        cJSON_AddStringToObject(stlink, "usb_id", "0483:3748"); cJSON_AddItemToObject(j, "stlink", stlink); free(wifi); free(usb);
    }
    return j;
}
static void api_status(void) { json_response(200, status_object()); }
#include "gateway-diagnose.inc"
static void api_logs(void) {
    GwFile *files; size_t n = gw_files(&files), i; unsigned port = (unsigned)atoi(value("uart"));
    cJSON *j = cJSON_CreateObject(), *list = cJSON_CreateArray();
    unsigned limit = 200, skip = (unsigned)strtoul(value("skip"), NULL, 10), shown = 0, matched = 0;
    for (i = n; i > 0; i--) if (!port || files[i - 1].port == port) {
        cJSON *entry, *meta; char path[256]; struct stat raw;
        if (matched++ < skip || shown >= limit) continue;
        entry = cJSON_CreateObject(); cJSON_AddStringToObject(entry, "name", files[i - 1].name);
        cJSON_AddNumberToObject(entry, "uart", files[i - 1].port); cJSON_AddNumberToObject(entry, "total_bytes", (double)files[i - 1].bytes);
        gw_path(path, sizeof(path), files[i - 1].name, ".bin");
        if (stat(path, &raw) == 0) cJSON_AddNumberToObject(entry, "raw_bytes", (double)raw.st_size);
        gw_path(path, sizeof(path), files[i - 1].name, ".idx");
        if (stat(path, &raw) == 0) cJSON_AddNumberToObject(entry, "index_bytes", (double)raw.st_size);
        gw_path(path, sizeof(path), files[i - 1].name, ".meta"); meta = json_file(path);
        if (meta) cJSON_AddItemToObject(entry, "meta", meta);
        cJSON_AddItemToArray(list, entry); shown++;
    }
    free(files); cJSON_AddNumberToObject(j, "total", matched); cJSON_AddItemToObject(j, "files", list); json_response(200, j);
}
static void api_view(void) {
    char path[256]; unsigned char data[8192]; size_t length; FILE *f; GwStamp stamp; int previous_byte = -1;
    uint64_t start = strtoull(value("start"), NULL, 10); unsigned limit = (unsigned)strtoul(value("limit"), NULL, 10);
    char hex[16385]; size_t i; cJSON *j, *marks;
    if (gw_path(path, sizeof(path), value("name"), ".bin") < 0) { error_response(400, "日志文件名无效"); return; }
    if (!limit || limit > 8192) limit = 8192;
    f = fopen(path, "rb"); if (!f) { error_response(404, "日志文件已删除或不存在"); return; }
    if (start > 2 * 1024 * 1024 || fseek(f, (long)start, SEEK_SET)) { fclose(f); error_response(400, "位置无效"); return; }
    if (start && !fseek(f, (long)start - 1, SEEK_SET)) previous_byte = fgetc(f);
    if (fseek(f, (long)start, SEEK_SET)) { fclose(f); error_response(400, "位置无效"); return; }
    length = fread(data, 1, limit, f); fclose(f);
    for (i = 0; i < length; i++) snprintf(hex + 2 * i, 3, "%02x", data[i]);
    hex[2 * length] = 0; j = cJSON_CreateObject(); marks = cJSON_CreateArray();
    cJSON_AddStringToObject(j, "name", value("name"));
    cJSON_AddStringToObject(j, "hex", hex); cJSON_AddNumberToObject(j, "start", (double)start); cJSON_AddNumberToObject(j, "length", length);
    cJSON_AddNumberToObject(j, "previous_byte", previous_byte);
    gw_path(path, sizeof(path), value("name"), ".idx"); f = fopen(path, "rb");
    if (f) {
        while (fread(&stamp, sizeof(stamp), 1, f) == 1) {
            cJSON *m; char iso[40];
            if (stamp.offset >= start + length) break;
            if (stamp.offset + stamp.count <= start) continue;
            m = cJSON_CreateObject(); gw_iso(stamp.utc_us, iso, sizeof(iso));
            if (stamp.utc_us && *iso) {
                size_t iso_length = strlen(iso);
                snprintf(iso + iso_length - 1, sizeof(iso) - iso_length + 1, ".%03uZ", (unsigned)(stamp.utc_us / 1000 % 1000));
            }
            cJSON_AddNumberToObject(m, "offset", (double)stamp.offset); cJSON_AddNumberToObject(m, "count", stamp.count);
            cJSON_AddNumberToObject(m, "uptime", stamp.monotonic_us / 1000000.0); cJSON_AddStringToObject(m, "utc", iso);
            cJSON_AddItemToArray(marks, m);
        }
        fclose(f);
    }
    cJSON_AddItemToObject(j, "marks", marks); json_response(200, j);
}
#include "gateway-export.inc"
static void api_download(void) {
    char path[256], buffer[8192]; FILE *f; size_t n; const char *ext = ".bin";
    if (!strcmp(value("format"), "text") || !strcmp(value("format"), "hex")) { api_export(value("format")); return; }
    if (!strcmp(value("format"), "timestamps")) {
        GwStamp stamp;
        if (gw_path(path, sizeof(path), value("name"), ".idx") < 0 || !(f = fopen(path, "rb"))) { error_response(404, "时间戳文件不存在"); return; }
        headers(200, "text/csv; charset=utf-8");
        if (!cli_mode) printf("Content-Disposition: attachment; filename=\"%.*s-timestamps.csv\"\r\n\r\n", (int)strlen(value("name")) - 4, value("name"));
        printf("offset,count,utc,uptime_seconds\n");
        while (fread(&stamp, sizeof(stamp), 1, f) == 1) {
            char iso[40]; gw_iso(stamp.utc_us, iso, sizeof(iso));
            printf("%llu,%u,%s,%.6f\n", (unsigned long long)stamp.offset, stamp.count, iso, stamp.monotonic_us / 1000000.0);
        }
        fclose(f); fflush(stdout); download_record("timestamps", 0); return;
    }
    if (!strcmp(value("format"), "index")) ext = ".idx";
    else if (!strcmp(value("format"), "meta")) ext = ".meta";
    if (gw_path(path, sizeof(path), value("name"), ext) < 0 || !(f = fopen(path, "rb"))) { error_response(404, "文件不存在"); return; }
    headers(200, "application/octet-stream"); if (!cli_mode) printf("Content-Disposition: attachment; filename=\"%.*s%s\"\r\n\r\n", (int)strlen(value("name")) - 4, value("name"), ext);
    while ((n = fread(buffer, 1, sizeof(buffer), f))) fwrite(buffer, 1, n, stdout);
    fclose(f); fflush(stdout); download_record(value("format"), 0);
}
static void api_settings(void) {
    GwConfig cfg; cJSON *j = cJSON_CreateObject(); char *pid; gw_config(&cfg);
    if (*value("mode")) {
        if (strcmp(value("mode"), "files") && strcmp(value("mode"), "bytes") && strcmp(value("mode"), "none")) { cJSON_Delete(j); error_response(400, "保留策略无效"); return; }
        strcpy(cfg.mode, value("mode")); cfg.files = (unsigned)strtoul(value("files"), NULL, 10); cfg.bytes = strtoull(value("bytes"), NULL, 10);
        if (*value("baud1")) cfg.baud[0] = (unsigned)strtoul(value("baud1"), NULL, 10);
        if (*value("baud2")) cfg.baud[1] = (unsigned)strtoul(value("baud2"), NULL, 10);
        if (cfg.files < 1 || cfg.files > 1000 || cfg.bytes < 4 * 1024 * 1024 || cfg.bytes > 80 * 1024 * 1024 || gw_baud(cfg.baud[0]) < 0 || gw_baud(cfg.baud[1]) < 0) { cJSON_Delete(j); error_response(400, "数量、容量或波特率无效"); return; }
        if (gw_config_save(&cfg) < 0) { cJSON_Delete(j); error_response(500, "配置保存失败"); return; }
        pid = gw_read("/run/gateway-logger.pid", 40); if (pid) { long p = strtol(pid, NULL, 10); if (p > 1) kill((pid_t)p, SIGHUP); free(pid); }
    }
    cJSON_AddStringToObject(j, "mode", cfg.mode); cJSON_AddNumberToObject(j, "files", cfg.files); cJSON_AddNumberToObject(j, "bytes", (double)cfg.bytes);
    cJSON_AddNumberToObject(j, "baud1", cfg.baud[0]); cJSON_AddNumberToObject(j, "baud2", cfg.baud[1]); json_response(200, j);
}
static int hex_byte(const char *p) {
    char s[3] = {p[0], p[1], 0}; if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return -1;
    return (int)strtol(s, NULL, 16);
}
static int validate_hex(const char *path, const char *target) {
    FILE *f = fopen(path, "r"); char line[600]; uint64_t base = 0, upper = !strcmp(target, "f103") ? 0x08010000ULL : 0x08100000ULL;
    int eof = 0, data = 0, valid = 1;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        int bytes[260], count, type, sum = 0, address, i; size_t n = strcspn(line, "\r\n");
        if (!n) continue;
        if (eof || line[0] != ':' || n < 11 || (n - 1) % 2) { valid = 0; break; }
        count = hex_byte(line + 1); if (count < 0 || n != (size_t)(11 + count * 2)) { valid = 0; break; }
        for (i = 0; i < count + 5; i++) { bytes[i] = hex_byte(line + 1 + 2 * i); if (bytes[i] < 0) valid = 0; sum += bytes[i]; }
        if (!valid || (sum & 255)) { valid = 0; break; }
        type = bytes[3]; address = bytes[1] * 256 + bytes[2];
        if (type == 0) {
            uint64_t absolute = base + (unsigned)address;
            if (absolute < 0x08000000ULL || absolute + (unsigned)count > upper) { valid = 0; break; }
            data += count;
        } else if (type == 1 && count == 0) eof = 1;
        else if (type == 4 && count == 2) base = (uint64_t)(bytes[4] * 256 + bytes[5]) << 16;
        else if (type == 2 && count == 2) base = (uint64_t)(bytes[4] * 256 + bytes[5]) << 4;
        else if ((type != 3 && type != 5) || count != 4) { valid = 0; break; }
    }
    fclose(f); return valid && eof && data;
}
static int validate_elf(const char *path, const char *target, size_t length) {
    FILE *f = fopen(path, "rb"); Elf32_Ehdr header; unsigned i; int data = 0, valid = 1;
    uint64_t upper = !strcmp(target, "f103") ? 0x08010000ULL : 0x08100000ULL;
    if (!f) return 0;
    if (fread(&header, sizeof(header), 1, f) != 1 || memcmp(header.e_ident, ELFMAG, SELFMAG) ||
        header.e_ident[EI_CLASS] != ELFCLASS32 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT ||
        header.e_machine != EM_ARM || header.e_type != ET_EXEC || header.e_ehsize != sizeof(header) ||
        header.e_phentsize != sizeof(Elf32_Phdr) || !header.e_phnum || header.e_phnum > 128 ||
        (uint64_t)header.e_phoff + (uint64_t)header.e_phnum * sizeof(Elf32_Phdr) > length) { fclose(f); return 0; }
    for (i = 0; i < header.e_phnum; i++) {
        Elf32_Phdr segment; uint64_t address;
        if (fseek(f, (long)(header.e_phoff + i * sizeof(segment)), SEEK_SET) || fread(&segment, sizeof(segment), 1, f) != 1) { valid = 0; break; }
        if (segment.p_type != PT_LOAD || !segment.p_filesz) continue;
        address = segment.p_paddr ? segment.p_paddr : segment.p_vaddr;
        if (segment.p_filesz > segment.p_memsz || (uint64_t)segment.p_offset + segment.p_filesz > length ||
            address < 0x08000000ULL || address + segment.p_filesz > upper) { valid = 0; break; }
        data = 1;
    }
    fclose(f); return valid && data;
}
static void wait_job(const char *id, const char *audit, unsigned seconds) {
    uint64_t deadline = gw_mono_us() + (uint64_t)seconds * 1000000;
    for (;;) {
        cJSON *record = json_file(audit), *state = record ? cJSON_GetObjectItem(record, "state") : NULL;
        if (cJSON_IsString(state) && strcmp(state->valuestring, "running")) {
            char path[256], *output; int success = !strcmp(state->valuestring, "success");
            snprintf(path, sizeof(path), "%s/%s.txt", GW_AUDIT, id); output = gw_read(path, 65536);
            cJSON_AddStringToObject(record, "output", output ? output : ""); free(output);
            if (!success) cJSON_AddStringToObject(record, "code", "flash_failed");
            json_response(success ? 200 : 500, record); return;
        }
        if (gw_mono_us() >= deadline) {
            if (!record) record = cJSON_CreateObject();
            cJSON_AddStringToObject(record, "id", id); cJSON_AddBoolToObject(record, "wait_timed_out", 1);
            json_response(202, record); response_exit = 75; return;
        }
        cJSON_Delete(record); usleep(200000);
    }
}
static void audit_trim(void) {
    for (;;) {
        DIR *d = opendir(GW_AUDIT); struct dirent *e; unsigned n = 0;
        double oldest = 1e30; char oldest_name[38] = "", path[256];
        if (!d) return;
        while ((e = readdir(d))) if (strlen(e->d_name) == 37 && !strcmp(e->d_name + 32, ".json")) {
            cJSON *record, *seq; snprintf(path, sizeof(path), "%s/%.37s", GW_AUDIT, e->d_name);
            record = json_file(path); seq = record ? cJSON_GetObjectItem(record, "sequence") : NULL;
            n++;
            if (cJSON_IsNumber(seq) && seq->valuedouble < oldest) {
                oldest = seq->valuedouble; memcpy(oldest_name, e->d_name, 38);
            }
            cJSON_Delete(record);
        }
        closedir(d); if (n <= 64 || !*oldest_name) return;
        snprintf(path, sizeof(path), "%s/%s", GW_AUDIT, oldest_name); unlink(path);
        oldest_name[32] = 0; snprintf(path, sizeof(path), "%s/%s.txt", GW_AUDIT, oldest_name); unlink(path);
    }
}
static void worker(const char *id, const char *target, const char *path, int lock, cJSON *record) {
    char audit[256], log[256], script[512]; pid_t child; int result = 1, status = 0; char *output;
    int nullfd = open("/dev/null", O_RDWR), logfd;
    setsid(); if (nullfd >= 0) { dup2(nullfd, 0); dup2(nullfd, 1); dup2(nullfd, 2); if (nullfd > 2) close(nullfd); }
    snprintf(audit, sizeof(audit), "%s/%s.json", GW_AUDIT, id); snprintf(log, sizeof(log), "%s/%s.txt", GW_AUDIT, id);
    snprintf(script, sizeof(script), "program %s verify reset exit", path);
    logfd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600); child = logfd >= 0 ? fork() : -1;
    if (child == 0) {
        dup2(logfd, 1); dup2(logfd, 2); close(logfd); close(lock);
        execl("/usr/bin/openocd", "openocd", "-f", !strcmp(target, "f103") ? "/etc/gateway/openocd-f103.cfg" : "/etc/gateway/openocd-f407-0.12.cfg", "-c", script, (char *)NULL); _exit(127);
    }
    if (logfd >= 0) close(logfd);
    if (child > 0) {
        uint64_t deadline = gw_mono_us() + 180000000;
        pid_t waited;
        while ((waited = waitpid(child, &status, WNOHANG)) == 0 || (waited < 0 && errno == EINTR)) {
            if (gw_mono_us() >= deadline) {
                kill(child, SIGKILL); waitpid(child, &status, 0); result = 124; break;
            }
            usleep(200000);
        }
        if (result != 124) result = waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    output = gw_read(log, 131072);
    cJSON_ReplaceItemInObject(record, "state", cJSON_CreateString(result == 0 && output && strstr(output, "** Verified OK **") ? "success" : "failed"));
    cJSON_AddNumberToObject(record, "exit_code", result); cJSON_AddNumberToObject(record, "finished_uptime", gw_mono_us() / 1000000.0);
    { char iso[40]; gw_iso(gw_utc_us(), iso, sizeof(iso)); cJSON_AddStringToObject(record, "finished_utc", iso); }
    save_json(audit, record); record_event("flash-result", "worker", !strcmp(cJSON_GetObjectItem(record, "state")->valuestring, "success") ? 200 : 500, record);
    free(output); cJSON_Delete(record); unlink(path); audit_trim(); close(lock); _exit(0);
}
static int receive_upload(const char *path, size_t length) {
    char buffer[8192]; size_t done = 0; int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    while (done < length) {
        size_t want = length - done > sizeof(buffer) ? sizeof(buffer) : length - done;
        ssize_t n = read(0, buffer, want), written = 0;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        while (written < n) { ssize_t w = write(fd, buffer + written, (size_t)(n - written)); if (w < 0 && errno == EINTR) continue; if (w <= 0) break; written += w; }
        if (written != n) break;
        done += (size_t)n;
    }
    close(fd); if (done != length) { unlink(path); return -1; } return 0;
}
static char *upload_hash(const char *path) {
    char command[400], *hash; snprintf(command, sizeof(command), "sha256sum %s", path); hash = command_text(command);
    if (!hash || strlen(hash) < 64) { free(hash); return NULL; } hash[64] = 0; return hash;
}
static void api_flash(size_t length) {
    const char *target = value("target"), *filename = value("filename"); char id[33], path[256], audit[256], iso[40];
    int lock, is_elf; cJSON *record; char *hash; pid_t child;
    const char *request_id = value("request_id");
    if ((strcmp(target, "f103") && strcmp(target, "f407")) || strlen(filename) < 5 || strlen(filename) > 240 ||
        (strcasecmp(filename + strlen(filename) - 4, ".hex") && strcasecmp(filename + strlen(filename) - 4, ".elf")) || !length || length > 4 * 1024 * 1024 ||
        (*request_id && !safe_id(request_id))) { error_response(400, "请选择目标型号及有效 HEX/ELF 文件（最大4MiB）"); return; }
    if (*request_id) {
        snprintf(audit, sizeof(audit), "%s/%s.json", GW_AUDIT, request_id); record = json_file(audit);
        if (record) {
            cJSON *old_target = cJSON_GetObjectItem(record, "target"), *old_name = cJSON_GetObjectItem(record, "filename"), *old_bytes = cJSON_GetObjectItem(record, "bytes");
            if (!cJSON_IsString(old_target) || strcmp(old_target->valuestring, target) || !cJSON_IsString(old_name) || strcmp(old_name->valuestring, filename) ||
                !cJSON_IsNumber(old_bytes) || old_bytes->valuedouble != (double)length) { cJSON_Delete(record); error_response(409, "任务编号已用于另一个烧录请求"); return; }
            snprintf(path, sizeof(path), "/run/gw-replay-%s-%ld", request_id, (long)getpid());
            if (receive_upload(path, length) < 0) { cJSON_Delete(record); error_response(400, "重试请求镜像不完整"); return; }
            hash = upload_hash(path); unlink(path);
            { cJSON *old_hash = cJSON_GetObjectItem(record, "sha256");
                int same = hash && cJSON_IsString(old_hash) && !strcmp(hash, old_hash->valuestring); free(hash);
                if (!same) { cJSON_Delete(record); error_response(409, "任务编号对应的镜像校验值不同，请使用新编号"); return; }
            }
            cJSON_AddBoolToObject(record, "replayed", 1);
            if (cli_mode && cli_wait_seconds) { cJSON_Delete(record); wait_job(request_id, audit, (unsigned)cli_wait_seconds); }
            else json_response(200, record);
            return;
        }
    }
    lock = open("/run/gateway-flash.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { if (lock >= 0) close(lock); error_response(409, "已有烧录任务运行"); return; }
    if (*request_id) { snprintf(audit, sizeof(audit), "%s/%s.json", GW_AUDIT, request_id); if (access(audit, F_OK) == 0) { close(lock); api_flash(length); return; } }
    is_elf = !strcasecmp(filename + strlen(filename) - 4, ".elf");
    if (*request_id) strcpy(id, request_id); else new_id(id);
    snprintf(path, sizeof(path), "/run/gw-upload-%s.%s", id, is_elf ? "elf" : "hex");
    if (receive_upload(path, length) < 0 || !(is_elf ? validate_elf(path, target, length) : validate_hex(path, target))) { unlink(path); close(lock); error_response(400, "镜像不完整、格式或校验失败，或地址超出目标 Flash"); return; }
    hash = upload_hash(path);
    if (!hash) { unlink(path); close(lock); error_response(500, "文件校验值计算失败"); return; }
    record = cJSON_CreateObject(); gw_iso(gw_utc_us(), iso, sizeof(iso));
    cJSON_AddStringToObject(record, "id", id); cJSON_AddStringToObject(record, "filename", filename); cJSON_AddStringToObject(record, "target", target);
    cJSON_AddStringToObject(record, "sha256", hash); cJSON_AddStringToObject(record, "utc", iso); cJSON_AddNumberToObject(record, "uptime", gw_mono_us() / 1000000.0);
    cJSON_AddNumberToObject(record, "bytes", length); cJSON_AddStringToObject(record, "state", "running"); free(hash);
    cJSON_AddStringToObject(record, "format", is_elf ? "elf" : "hex");
    if (!strcmp(value("dry_run"), "1")) {
        cJSON_ReplaceItemInObject(record, "state", cJSON_CreateString("validated")); cJSON_AddBoolToObject(record, "dry_run", 1);
        unlink(path); close(lock); json_response(200, record); return;
    }
    {
        char *old = gw_read(GW_AUDIT "/.sequence", 40), text[40];
        uint64_t seq = old ? strtoull(old, NULL, 10) + 1 : 1; free(old);
        snprintf(text, sizeof(text), "%llu\n", (unsigned long long)seq);
        if (gw_atomic(GW_AUDIT "/.sequence", text, strlen(text)) < 0) { cJSON_Delete(record); unlink(path); close(lock); error_response(500, "记录序号保存失败"); return; }
        cJSON_AddNumberToObject(record, "sequence", (double)seq);
    }
    snprintf(audit, sizeof(audit), "%s/%s.json", GW_AUDIT, id);
    if (save_json(audit, record) < 0) { cJSON_Delete(record); unlink(path); close(lock); error_response(500, "烧录记录保存失败"); return; }
    audit_trim();
    child = fork();
    if (child == 0) worker(id, target, path, lock, record);
    close(lock);
    if (child < 0) {
        cJSON_ReplaceItemInObject(record, "state", cJSON_CreateString("failed")); save_json(audit, record);
        cJSON_Delete(record); unlink(path); error_response(500, "任务启动失败"); return;
    }
    if (cli_mode && cli_wait_seconds) { cJSON_Delete(record); wait_job(id, audit, (unsigned)cli_wait_seconds); }
    else json_response(202, record);
}
static void api_jobs(void) {
    DIR *d = opendir(GW_AUDIT); struct dirent *entry; cJSON *j = cJSON_CreateObject(), *records = cJSON_CreateArray();
    unsigned n = 0; char path[256];
    while (d && (entry = readdir(d)) && n < 128) if (strlen(entry->d_name) == 37 && !strcmp(entry->d_name + 32, ".json")) {
        cJSON *record; snprintf(path, sizeof(path), "%s/%.37s", GW_AUDIT, entry->d_name); record = json_file(path);
        if (record) { cJSON_AddItemToArray(records, record); n++; }
    }
    if (d) closedir(d);
    cJSON_AddItemToObject(j, "records", records); json_response(200, j);
}
static void api_job(void) {
    char path[256]; cJSON *record; char *log;
    if (!safe_id(value("id"))) { error_response(400, "记录编号无效"); return; }
    snprintf(path, sizeof(path), "%s/%s.json", GW_AUDIT, value("id")); record = json_file(path);
    if (!record) { error_response(404, "记录不存在"); return; }
    snprintf(path, sizeof(path), "%s/%s.txt", GW_AUDIT, value("id")); log = gw_read(path, 65536);
    cJSON_AddStringToObject(record, "output", log ? log : ""); free(log); json_response(200, record);
}
static void api_profiles(const char *op) {
    cJSON *j, *data; char id[33], path[256];
    if (!strcmp(op, "profiles")) {
        DIR *d = opendir(GW_PROFILES); struct dirent *entry; cJSON *list = cJSON_CreateArray(); j = cJSON_CreateObject();
        while (d && (entry = readdir(d))) if (strlen(entry->d_name) == 37 && !strcmp(entry->d_name + 32, ".json")) {
            snprintf(path, sizeof(path), "%s/%.37s", GW_PROFILES, entry->d_name); data = json_file(path); if (data) cJSON_AddItemToArray(list, data);
        }
        if (d) closedir(d);
        cJSON_AddItemToObject(j, "profiles", list);
        data = json_file(GW_PROFILES "/current"); cJSON_AddItemToObject(j, "current", data ? data : cJSON_CreateNull()); json_response(200, j); return;
    }
    if (!strcmp(op, "profile-save")) {
        DIR *d = opendir(GW_PROFILES); struct dirent *entry; unsigned count = 0;
        while (d && (entry = readdir(d))) if (strlen(entry->d_name) == 37 && !strcmp(entry->d_name + 32, ".json")) count++;
        if (d) closedir(d);
        if (count >= 32) { error_response(409, "参数组已达到32组上限，请先删除不需要的组"); return; }
        data = cJSON_Parse(value("data"));
        if (!cJSON_IsObject(data) || !cJSON_IsString(cJSON_GetObjectItem(data, "name")) || !cJSON_IsObject(cJSON_GetObjectItem(data, "values"))) { cJSON_Delete(data); error_response(400, "参数组格式无效"); return; }
        new_id(id); cJSON_AddStringToObject(data, "id", id); cJSON_AddStringToObject(data, "target", "f407");
        cJSON_AddBoolToObject(data, "applied_to_target", 0); snprintf(path, sizeof(path), "%s/%s.json", GW_PROFILES, id);
        if (save_json(path, data) < 0) { cJSON_Delete(data); error_response(500, "参数组保存失败"); return; }
        json_response(200, data); return;
    }
    if (!strcmp(op, "profile-delete")) {
        if (!safe_id(value("id"))) { error_response(400, "参数组编号无效"); return; }
        snprintf(path, sizeof(path), "%s/%s.json", GW_PROFILES, value("id"));
        if (unlink(path) < 0) { error_response(404, "参数组不存在"); return; }
        j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "deleted", 1); json_response(200, j); return;
    }
    if (!strcmp(op, "profile-select")) {
        if (!safe_id(value("id"))) { error_response(400, "参数组编号无效"); return; }
        snprintf(path, sizeof(path), "%s/%s.json", GW_PROFILES, value("id")); data = json_file(path);
        if (!data) { error_response(404, "参数组不存在"); return; }
    } else if (!strcmp(op, "profile-restore")) {
        data = json_file(GW_PROFILES "/previous"); if (!data) { error_response(404, "尚无上一组"); return; }
    } else { error_response(409, "F407 参数协议待接入；当前仅保存网关配置组，不执行目标写入"); return; }
    j = json_file(GW_PROFILES "/current"); if (j) { save_json(GW_PROFILES "/previous", j); cJSON_Delete(j); }
    if (save_json(GW_PROFILES "/current", data) < 0) { cJSON_Delete(data); error_response(500, "参数组选择失败"); return; }
    json_response(200, data);
}
static void recover_jobs(void) {
    int lock = open("/run/gateway-flash.lock", O_RDWR | O_CREAT, 0600);
    DIR *d; struct dirent *entry; char path[256];
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { if (lock >= 0) close(lock); return; }
    d = opendir(GW_AUDIT);
    while (d && (entry = readdir(d))) if (strlen(entry->d_name) == 37 && !strcmp(entry->d_name + 32, ".json")) {
        cJSON *record, *state; snprintf(path, sizeof(path), "%s/%.37s", GW_AUDIT, entry->d_name);
        record = json_file(path); state = record ? cJSON_GetObjectItem(record, "state") : NULL;
        if (cJSON_IsString(state) && !strcmp(state->valuestring, "running")) {
            cJSON_ReplaceItemInObject(record, "state", cJSON_CreateString("interrupted")); save_json(path, record);
        }
        cJSON_Delete(record);
    }
    if (d) closedir(d);
    audit_trim();
    close(lock);
}
static void api_delete_log(void) {
    int fd; struct sockaddr_un local = { .sun_family = AF_UNIX }, remote = { .sun_family = AF_UNIX };
    struct timeval timeout = { 3, 0 }; char command[100], response[80]; ssize_t n;
    if (!gw_log_name(value("name"))) { error_response(400, "日志文件名无效"); return; }
    fd = socket(AF_UNIX, SOCK_DGRAM, 0); if (fd < 0) { error_response(500, "日志服务连接失败"); return; }
    snprintf(local.sun_path, sizeof(local.sun_path), "/run/gateway-log-client-%ld.sock", (long)getpid());
    strcpy(remote.sun_path, "/run/gateway-logs.sock"); unlink(local.sun_path);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) { close(fd); error_response(500, "日志服务连接失败"); return; }
    snprintf(command, sizeof(command), "delete %s", value("name"));
    n = sendto(fd, command, strlen(command), 0, (struct sockaddr *)&remote, sizeof(remote));
    if (n > 0) n = recv(fd, response, sizeof(response) - 1, 0);
    close(fd); unlink(local.sun_path);
    if (n <= 0) { error_response(503, "日志服务未响应，尚不能确认删除结果"); return; }
    response[n] = 0;
    if (strcmp(response, "OK")) { error_response(400, response); return; }
    { cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "deleted", 1); json_response(200, j); }
}
static void api_gpio(void) {
    const char *cmd = value("command"); int fd, lock; struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval timeout = { 2, 0 }; char response[160]; ssize_t n;
    if (strcmp(cmd, "reset") && strcmp(cmd, "rom") && strcmp(cmd, "normal") && strcmp(cmd, "reset-low") && strcmp(cmd, "reset-release") && strcmp(cmd, "boot-low") && strcmp(cmd, "boot-high")) { error_response(400, "控制命令无效"); return; }
    lock = open("/run/gateway-flash.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { if (lock >= 0) close(lock); error_response(409, "烧录运行期间暂不能改变目标控制线"); return; }
    fd = socket(AF_UNIX, SOCK_STREAM, 0); strcpy(address.sun_path, "/run/gateway-control.sock");
    if (fd < 0) { close(lock); error_response(503, "目标控制服务不可用"); return; }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) n = -1;
    else { n = send(fd, cmd, strlen(cmd), MSG_NOSIGNAL); if (n > 0) n = recv(fd, response, sizeof(response) - 1, 0); }
    close(fd); close(lock);
    if (n <= 0) { error_response(503, "目标控制服务未响应"); return; }
    response[n] = 0;
    if (strncmp(response, "OK ", 3)) { error_response(500, response); return; }
    { cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "control", response); cJSON_AddItemToObject(j, "gpio", gpio_object(response)); json_response(200, j); }
}
static void api_usb_power(void) {
    const char *state = value("state"); int lock, fd, expected; ssize_t written;
    if (strcmp(state, "on") && strcmp(state, "off")) { error_response(400, "USB 供电状态必须是 on 或 off"); return; }
    if (usb_power_state() < 0) { error_response(503, "当前内核尚未提供 USB 供电控制"); return; }
    lock = open("/run/gateway-flash.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { if (lock >= 0) close(lock); error_response(409, "烧录或目标操作期间暂不能改变 USB 供电"); return; }
    expected = !strcmp(state, "on"); state = expected ? "enabled" : "disabled";
    fd = open(USB_POWER_STATE, O_WRONLY); written = fd < 0 ? -1 : write(fd, state, strlen(state));
    if (fd >= 0) close(fd);
    if (written != (ssize_t)strlen(state) || usb_power_state() != expected) { close(lock); error_response(500, "USB 供电驱动未确认所需状态"); return; }
    close(lock);
    { cJSON *j = cJSON_CreateObject(); cJSON_AddItemToObject(j, "usb_power", usb_power_object()); json_response(200, j); }
}
static void api_target(void) {
    const char *target = value("target"); cJSON *j;
    if (*target) {
        char text[12];
        if (strcmp(target, "f103") && strcmp(target, "f407")) { error_response(400, "目标必须是 f103 或 f407"); return; }
        snprintf(text, sizeof(text), "%s\n", target);
        if (gw_atomic("/etc/gateway/target.conf", text, strlen(text)) < 0) { error_response(500, "目标设置保存失败"); return; }
    }
    j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "configured_target", default_target());
    cJSON_AddBoolToObject(j, "detected", 0); json_response(200, j);
}
static void api_target_reset(void) {
    const char *target = *value("target") ? value("target") : default_target();
    int lock, status; char command[300], *output; cJSON *j;
    if (strcmp(target, "f103") && strcmp(target, "f407")) { error_response(400, "目标型号无效"); return; }
    lock = open("/run/gateway-flash.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { if (lock >= 0) close(lock); error_response(409, "已有目标操作运行"); return; }
    snprintf(command, sizeof(command), "/bin/busybox timeout -s KILL 15 /usr/bin/openocd -f %s -c 'init; reset run; shutdown' 2>&1",
             !strcmp(target, "f103") ? "/etc/gateway/openocd-f103.cfg" : "/etc/gateway/openocd-f407-0.12.cfg");
    output = command_result(command, &status); close(lock);
    j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "target", target); cJSON_AddStringToObject(j, "via", "stlink");
    cJSON_AddStringToObject(j, "output", output ? output : ""); cJSON_AddNumberToObject(j, "exit_code", status);
    cJSON_AddBoolToObject(j, "reset_command_completed", status == 0); free(output);
    if (status) { cJSON_AddStringToObject(j, "code", "target_reset_failed"); cJSON_AddStringToObject(j, "error", "ST-Link 目标复位失败，详见 output"); }
    json_response(status ? 503 : 200, j);
}
static void api_capabilities(void) {
    cJSON *j = cJSON_CreateObject(), *commands = cJSON_CreateObject(), *param = cJSON_CreateObject();
    const char *names[] = {"status", "diagnose", "flash_hex", "flash_elf", "reset_stlink", "reset_gpio", "bootloader_gpio", "gpio", "log", "log_delete", "log_download", "log_export_text", "log_export_hex", "test_run", "profiles", "activity", "mcu_rpc", "mcu_cached", "param_get", "param_set", "param_save", "param_rollback"};
    size_t i; for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) cJSON_AddBoolToObject(commands, names[i], 1);
    cJSON_AddBoolToObject(commands, "usb_power", usb_power_state() >= 0);
    cJSON_AddBoolToObject(commands, "inertial_bench", 1);
    cJSON_AddBoolToObject(commands, "navigation_diagnostics", 1);
    cJSON_AddBoolToObject(commands, "differential_drive_rpc", 1);
    cJSON_AddItemToObject(j, "commands", commands); cJSON_AddStringToObject(j, "version", "1.3.0");
    cJSON_AddStringToObject(j, "configured_target", default_target());
    cJSON_AddBoolToObject(param, "ready", 1); cJSON_AddStringToObject(param, "protocol", "F407 v1 / UART1 / CRC16-CCITT-FALSE");
    cJSON_AddStringToObject(param, "storage", "set 修改主控 RAM；save 写入主控 Flash；rollback 读取上次 Flash 参数");
    cJSON_AddItemToObject(j, "parameters", param); cJSON_AddNumberToObject(j, "flash_max_bytes", 4194304);
    cJSON_AddNumberToObject(j, "flash_history_limit", 64); cJSON_AddStringToObject(j, "http_endpoint", "/cgi-bin/gateway-api?op=COMMAND");
    cJSON_AddStringToObject(j, "write_method", "POST"); cJSON_AddStringToObject(j, "write_token", "status.token");
    json_response(200, j);
}
static void api_test(void) {
    cJSON *j = cJSON_CreateObject(), *s = status_object(), *logger = cJSON_GetObjectItem(s, "logger"), *network = cJSON_GetObjectItem(s, "network"), *stlink = cJSON_GetObjectItem(s, "stlink"), *checks = cJSON_CreateObject();
    cJSON *alive = cJSON_GetObjectItem(s, "logger_alive"), *wifi = cJSON_GetObjectItem(network, "wpa_state"), *present = cJSON_GetObjectItem(stlink, "present");
    int log_ok = cJSON_IsTrue(alive), net_ok = cJSON_IsString(wifi) && !strcmp(wifi->valuestring, "COMPLETED");
    char *ecc = command_text("cat /sys/class/mtd/mtd1/ecc_failures /sys/class/mtd/mtd2/ecc_failures 2>/dev/null");
    cJSON_AddBoolToObject(checks, "logger_running", log_ok); cJSON_AddBoolToObject(checks, "wifi_associated", net_ok);
    cJSON_AddBoolToObject(checks, "stlink_enumerated", cJSON_IsTrue(present));
    cJSON_AddItemToObject(checks, "uart_error", cJSON_Duplicate(cJSON_GetObjectItem(logger, "error"), 1));
    cJSON_AddStringToObject(checks, "nand_ecc_failures", ecc ? ecc : ""); free(ecc);
    cJSON_AddStringToObject(j, "test", "diagnostics"); cJSON_AddBoolToObject(j, "read_only", 1);
    cJSON_AddItemToObject(j, "checks", checks); cJSON_AddItemToObject(j, "status", s);
    cJSON_AddStringToObject(j, "result", log_ok && net_ok ? "core_ready" : "attention_required"); json_response(200, j);
}
static void dispatch(const char *op, const char *method, size_t length) {
    int post = !strcmp(method, "POST");
    request_operation = op; request_method = method;
    record_response = strcmp(op, "activity") && (cli_mode || post || !strcmp(op, "download") || !strcmp(value("observe"), "1"));
    if (!strcmp(op, "status")) api_status();
    else if (!strcmp(op, "diagnose")) api_diagnose();
    else if (!strcmp(op, "activity")) api_activity();
    else if (!strcmp(op, "capabilities")) api_capabilities();
    else if (!strcmp(op, "test")) api_test();
    else if (!strcmp(op, "logs")) api_logs();
    else if (!strcmp(op, "view")) api_view();
    else if (!strcmp(op, "download")) api_download();
    else if (!strcmp(op, "log-delete") && post) api_delete_log();
    else if (!strcmp(op, "gpio") && post) api_gpio();
    else if (!strcmp(op, "usb-power") && post) api_usb_power();
    else if (!strcmp(op, "target-reset") && post) api_target_reset();
    else if (!strcmp(op, "target")) { if (!post && *value("target")) error_response(405, "保存目标设置需要 POST"); else api_target(); }
    else if (!strcmp(op, "settings")) { if (!post && *value("mode")) error_response(405, "保存配置需要 POST"); else api_settings(); }
    else if (!strcmp(op, "flash") && post) api_flash(length);
    else if (!strcmp(op, "jobs")) api_jobs();
    else if (!strcmp(op, "job")) api_job();
    else if (!strcmp(op, "profiles")) api_profiles(op);
    else if (!strncmp(op, "profile-", 8) && post) api_profiles(op);
    else if (!strcmp(op,"mcu-cached") || !strcmp(op,"param-get") ||
             ((!strcmp(op,"mcu-rpc") || !strcmp(op,"param-set") || !strcmp(op,"param-save") || !strcmp(op,"param-rollback")) && post)) api_mcu(op);
    else error_response(404, "接口不存在或请求方法错误");
}
static volatile sig_atomic_t snapshot_stop;
static void snapshot_signal(int sig) { (void)sig; snapshot_stop = 1; }
static void cache_command(const char *path, const char *command) {
    char temp[256], *text = command_text(command); FILE *f;
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()); f = fopen(temp, "w");
    if (f) { if (text) fwrite(text, 1, strlen(text), f); fclose(f); rename(temp, path); }
    free(text);
}
static void status_snapshot(void) {
    unsigned long counter = 0; int lock = open("/run/gateway-state.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { if (lock >= 0) close(lock); return; }
    signal(SIGTERM, snapshot_signal); signal(SIGINT, snapshot_signal);
    while (!snapshot_stop) {
        cJSON *status; char *text;
        mcu_update_cache();
        if (counter % 5 == 0) {
            cache_command("/run/gateway-wifi-status", "/bin/busybox timeout 3 wpa_cli -i wlan0 status 2>/dev/null");
            cache_command("/run/gateway-usb-status", "/bin/busybox timeout 3 lsusb 2>/dev/null");
            cache_command("/run/gateway-target-status", "/bin/busybox timeout 3 gateway-ctl status 2>/dev/null");
        }
        status = status_object(); cJSON_AddNumberToObject(status, "sequence", ++counter); text = cJSON_PrintUnformatted(status);
        FILE *f = fopen("/run/gateway-status.json.tmp", "w");
        if (f) { if (text) fwrite(text, 1, strlen(text), f); fclose(f); rename("/run/gateway-status.json.tmp", "/run/gateway-status.json"); }
        free(text); cJSON_Delete(status); sleep(1);
    }
    close(lock);
}
#include "gateway-cli.inc"
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--recover")) { gw_prepare(); recover_jobs(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--snapshot")) { gw_prepare(); status_snapshot(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--cli")) return cli_main(argc - 1, argv + 1);
    { const char *base = strrchr(argv[0], '/'); if (!strcmp(base ? base + 1 : argv[0], "gateway")) return cli_main(argc, argv); }
    char *query = strdup(getenv("QUERY_STRING") ? getenv("QUERY_STRING") : ""), *body = NULL;
    const char *method = getenv("REQUEST_METHOD") ? getenv("REQUEST_METHOD") : "GET", *op; size_t length = 0;
    parse(query); op = value("op"); gw_prepare();
    if (getenv("CONTENT_LENGTH")) length = (size_t)strtoull(getenv("CONTENT_LENGTH"), NULL, 10);
    if (!strcmp(method, "POST")) {
        char *token;
        if (strcmp(op, "flash")) {
            size_t got = 0;
            if (length > 65536) { error_response(413, "请求过大"); return 0; }
            body = malloc(length + 1); if (!body) return 1;
            while (got < length) { ssize_t n = read(0, body + got, length - got); if (n <= 0) break; got += (size_t)n; }
            body[got] = 0; if (got != length) { error_response(400, "请求不完整"); return 0; } parse(body);
        }
        token = gw_read(GW_TOKEN, 100); if (token) token[strcspn(token, "\r\n")] = 0;
        if (!token || strlen(token) < 32 || strcmp(token, value("token"))) { free(token); error_response(403, "页面已更新，请刷新后再试"); return 0; } free(token);
    } else if (strcmp(method, "GET")) { error_response(405, "不支持该请求方法"); return 0; }
    dispatch(op, method, length);
    free(body); free(query); return 0;
}
