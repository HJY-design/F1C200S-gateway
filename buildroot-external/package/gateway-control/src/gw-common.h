#ifndef GW_COMMON_H
#define GW_COMMON_H
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#ifndef GW_LOGDIR
#define GW_LOGDIR "/data/gateway/logs"
#endif
#ifndef GW_CONFIG
#define GW_CONFIG "/etc/gateway/uart-logs.conf"
#endif
#ifndef GW_STATS
#define GW_STATS "/run/gateway-log-stats.json"
#endif
#ifndef GW_TOKEN
#define GW_TOKEN "/run/gateway-web-token"
#endif
#ifndef GW_AUDIT
#define GW_AUDIT "/data/gateway/programming"
#endif
#ifndef GW_PROFILES
#define GW_PROFILES "/data/gateway/profiles"
#endif
#ifndef GW_HISTORY
#define GW_HISTORY "/data/gateway/actions"
#endif
#ifndef GW_HISTORY_LOCK
#define GW_HISTORY_LOCK "/run/gateway-actions.lock"
#endif
typedef struct {
    char mode[8];
    unsigned files, baud[2], segment;
    uint64_t bytes;
} GwConfig;
typedef struct {
    uint64_t id, bytes;
    unsigned port;
    char name[64];
} GwFile;
typedef struct {
    uint64_t offset, monotonic_us, utc_us;
    uint32_t count, reserved;
} GwStamp;
void gw_config(GwConfig *cfg);
int gw_config_save(const GwConfig *cfg);
int gw_atomic(const char *path, const void *data, size_t size);
char *gw_read(const char *path, size_t max);
uint64_t gw_mono_us(void);
uint64_t gw_utc_us(void);
void gw_iso(uint64_t micros, char *out, size_t size);
int gw_prepare(void);
size_t gw_files(GwFile **files);
int gw_log_name(const char *name);
int gw_path(char *out, size_t size, const char *name, const char *extension);
int gw_prune(const GwConfig *cfg, const char *active1, const char *active2);
int gw_baud(unsigned baud);
int gw_uart_raw(int fd, unsigned baud);
#endif
