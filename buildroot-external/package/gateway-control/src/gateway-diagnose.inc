/* On-demand bounded evidence collection. Never probes SWD or changes controls. */
static void diagnose_number(cJSON *json, const char *key, const char *path) {
    char *text = gw_read(path, 64), *end; unsigned long long number;
    if (!text) { cJSON_AddNullToObject(json, key); return; }
    errno = 0; number = strtoull(text, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    if (errno || !isdigit((unsigned char)*text) || *end) cJSON_AddNullToObject(json, key);
    else cJSON_AddNumberToObject(json, key, (double)number);
    free(text);
}
static void diagnose_recent_string(cJSON *array, const char *line, unsigned limit) {
    char text[384]; size_t length = strlen(line);
    if (length >= sizeof(text)) { length = sizeof(text) - 1; while (length && ((unsigned char)line[length] & 0xc0) == 0x80) length--; }
    memcpy(text, line, length); text[length] = 0;
    if ((unsigned)cJSON_GetArraySize(array) >= limit) cJSON_DeleteItemFromArray(array, 0);
    cJSON_AddItemToArray(array, cJSON_CreateString(text));
}
static int diagnose_contains(const char *text, const char *pattern) {
    size_t length = strlen(pattern);
    for (; *text; text++) if (!strncasecmp(text, pattern, length)) return 1;
    return 0;
}
static void diagnose_kernel_lines(cJSON *json, char *text) {
    const char *keys[] = {"spi_checksum", "spi_transaction", "transport_recovery", "wifi_disconnect"};
    const char *patterns[] = {"ESP_RX_CHECKSUM_ERROR", "SPI Transaction failed", "Transport recovery:", "process_disconnect_event:"};
    unsigned counts[4] = {0}; size_t i; char *next = text;
    cJSON *counters = cJSON_CreateObject(), *messages = cJSON_CreateArray(), *clock = cJSON_CreateArray();
    while (next && *next) {
        char *line = next; int interesting = 0; next = strchr(next, '\n'); if (next) *next++ = 0;
        for (i = 0; i < 4; i++) if (strstr(line, patterns[i])) { counts[i]++; interesting = 1; }
        if (strstr(line, "Config - SPI clock[") || strstr(line, "ESP Reconfigure SPI CLK")) diagnose_recent_string(clock, line, 3);
        if ((strstr(line, "esp32_spi") || strstr(line, "UBIFS") || strstr(line, "Out of memory") || strstr(line, "watchdog")) &&
            (diagnose_contains(line, "error") || diagnose_contains(line, "failed") || diagnose_contains(line, "timeout") || diagnose_contains(line, "out of memory"))) interesting = 1;
        if (interesting) diagnose_recent_string(messages, line, 20);
    }
    for (i = 0; i < 4; i++) cJSON_AddNumberToObject(counters, keys[i], counts[i]);
    cJSON_AddStringToObject(json, "counter_scope", "matching lines in retained kernel messages; not lifetime counters");
    cJSON_AddItemToObject(json, "matches", counters); cJSON_AddItemToObject(json, "recent_messages", messages); cJSON_AddItemToObject(json, "spi_clock_messages", clock);
}
static cJSON *diagnose_kernel(void) {
    cJSON *json = cJSON_CreateObject(); int total = klogctl(10, NULL, 0), bytes, capacity = total > 65536 ? 65536 : total;
    char *text = capacity > 0 ? calloc(1, (size_t)capacity + 1) : NULL;
    bytes = text ? klogctl(3, text, capacity) : -1;
    cJSON_AddBoolToObject(json, "available", bytes >= 0);
    cJSON_AddStringToObject(json, "source", "kernel_ring_buffer_read_without_clear");
    if (bytes >= 0) {
        text[bytes] = 0; cJSON_AddNumberToObject(json, "bytes", bytes); cJSON_AddNumberToObject(json, "buffer_capacity_bytes", total);
        cJSON_AddBoolToObject(json, "collection_truncated", bytes == capacity && total > capacity); diagnose_kernel_lines(json, text);
    } else cJSON_AddStringToObject(json, "reason", "kernel messages unavailable; counters are unknown");
    free(text); return json;
}
static cJSON *diagnose_failures(void) {
    HistoryFile files[128]; size_t count = history_files(files), i; cJSON *list = cJSON_CreateArray(); char path[256];
    for (i = count; i > 0 && cJSON_GetArraySize(list) < 5; i--) {
        cJSON *record, *ok, *result, *state; snprintf(path, sizeof(path), "%s/%s", GW_HISTORY, files[i - 1].name);
        record = json_file(path); if (!record) continue;
        ok = cJSON_GetObjectItem(record, "ok"); result = cJSON_GetObjectItem(record, "result"); state = cJSON_GetObjectItem(result, "state");
        if (cJSON_IsFalse(ok) || (cJSON_IsString(state) && (!strcmp(state->valuestring, "failed") || !strcmp(state->valuestring, "interrupted")))) cJSON_AddItemToArray(list, record);
        else cJSON_Delete(record);
    }
    return list;
}
static cJSON *diagnose_object(void) {
    uint64_t started = gw_mono_us(); char id[33], path[256], *build; unsigned i;
    const char *fields[] = {"rx_bytes", "tx_bytes", "rx_errors", "tx_errors", "rx_dropped", "tx_dropped"};
    const char *mtd_fields[] = {"size", "corrected_bits", "ecc_failures", "bad_blocks"};
    cJSON *json = cJSON_CreateObject(), *status = status_object(), *network = cJSON_CreateObject(), *mtd = cJSON_CreateArray();
    cJSON *services = cJSON_CreateObject(), *cached = cJSON_CreateObject();
    cJSON_DeleteItemFromObject(status, "token"); new_id(id);
    cJSON_AddNumberToObject(json, "diagnostic_version", 1); cJSON_AddStringToObject(json, "diagnostic_id", id);
    cJSON_AddBoolToObject(json, "read_only", 1); cJSON_AddStringToObject(json, "scope", "gateway evidence; MCU execution and wiring are not tested");
    cJSON_AddItemToObject(json, "status", status);
    build = gw_read("/etc/gateway/build-info", 4096); if (build) cJSON_AddStringToObject(json, "build_info", build); else cJSON_AddNullToObject(json, "build_info"); free(build);
    cJSON_AddStringToObject(network, "interface", "wlan0"); cJSON_AddStringToObject(network, "counter_scope", "current interface lifetime");
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) { snprintf(path, sizeof(path), "/sys/class/net/wlan0/statistics/%s", fields[i]); diagnose_number(network, fields[i], path); }
    cJSON_AddItemToObject(json, "network_counters", network);
    {
        const char *names[] = {"wifi", "usb"}, *paths[] = {"/run/gateway-wifi-status", "/run/gateway-usb-status"};
        uint64_t utc = gw_utc_us();
        for (i = 0; i < 2; i++) {
            struct stat info; cJSON *entry = cJSON_CreateObject(); int exists = stat(paths[i], &info) == 0;
            cJSON_AddBoolToObject(entry, "available", exists);
            if (exists && utc && info.st_mtime > 0 && utc / 1000000 >= (uint64_t)info.st_mtime) cJSON_AddNumberToObject(entry, "age_seconds", utc / 1000000 - (uint64_t)info.st_mtime);
            else cJSON_AddNullToObject(entry, "age_seconds");
            cJSON_AddItemToObject(cached, names[i], entry);
        }
    }
    cJSON_AddItemToObject(json, "cached_inputs", cached);
    for (i = 0; i < 3; i++) {
        unsigned k; cJSON *entry = cJSON_CreateObject(); cJSON_AddNumberToObject(entry, "index", i);
        for (k = 0; k < sizeof(mtd_fields) / sizeof(mtd_fields[0]); k++) { snprintf(path, sizeof(path), "/sys/class/mtd/mtd%u/%s", i, mtd_fields[k]); diagnose_number(entry, mtd_fields[k], path); }
        cJSON_AddItemToArray(mtd, entry);
    }
    cJSON_AddItemToObject(json, "nand_counters", mtd); cJSON_AddItemToObject(json, "kernel", diagnose_kernel());
    {
        const char *names[] = {"logger", "control", "http", "snapshot", "ssh"};
        const char *pidfiles[] = {"/run/gateway-logger.pid", "/run/gateway-control.pid", "/run/gateway-httpd.pid", "/run/gateway-state.pid", "/run/dropbear.pid"};
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            char *text = gw_read(pidfiles[i], 32); pid_t pid = text ? (pid_t)strtol(text, NULL, 10) : 0; cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "pid", pid); cJSON_AddBoolToObject(entry, "pid_alive", pid > 1 && kill(pid, 0) == 0);
            cJSON_AddItemToObject(services, names[i], entry); free(text);
        }
        int fd = open("/run/gateway-flash.lock", O_RDONLY);
        if (fd >= 0) { int rc = flock(fd, LOCK_SH | LOCK_NB); if (!rc || errno == EWOULDBLOCK) cJSON_AddBoolToObject(json, "target_operation_busy", rc != 0); else cJSON_AddNullToObject(json, "target_operation_busy"); close(fd); }
        else if (errno == ENOENT) cJSON_AddBoolToObject(json, "target_operation_busy", 0);
        else cJSON_AddNullToObject(json, "target_operation_busy");
    }
    cJSON_AddItemToObject(json, "services", services); cJSON_AddItemToObject(json, "recent_failed_operations", diagnose_failures());
    cJSON_AddStringToObject(json, "failure_scope", "retained operation history, newest 5 failures; includes rejected test requests");
    cJSON_AddNumberToObject(json, "collection_ms", (gw_mono_us() - started) / 1000.0); return json;
}
static void api_diagnose(void) { json_response(200, diagnose_object()); }
