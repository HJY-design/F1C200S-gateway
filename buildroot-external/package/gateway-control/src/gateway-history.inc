/* Immutable bounded operation records; normal Web polling never records events. */
typedef struct { uint64_t sequence; char name[26]; } HistoryFile;
static int history_compare(const void *a, const void *b) {
    const HistoryFile *x = a, *y = b; return x->sequence < y->sequence ? -1 : x->sequence > y->sequence;
}
static size_t history_files(HistoryFile files[128]) {
    DIR *directory = opendir(GW_HISTORY); struct dirent *entry; size_t count = 0;
    while (directory && (entry = readdir(directory))) {
        if (strlen(entry->d_name) != 25 || strspn(entry->d_name, "0123456789") != 20 || strcmp(entry->d_name + 20, ".json")) continue;
        if (count >= 128) break;
        files[count].sequence = strtoull(entry->d_name, NULL, 10); memcpy(files[count++].name, entry->d_name, 26);
    }
    if (directory) closedir(directory);
    qsort(files, count, sizeof(*files), history_compare); return count;
}
static void preview_trim(cJSON *json, int *truncated) {
    cJSON *child = json ? json->child : NULL; unsigned count = 0;
    while (child) {
        cJSON *next = child->next;
        if ((child->string && !strcmp(child->string, "token")) || (cJSON_IsArray(json) && count >= 12)) {
            cJSON_DetachItemViaPointer(json, child); cJSON_Delete(child); *truncated = 1;
        } else if (cJSON_IsString(child) && child->valuestring) {
            size_t cap = child->string && !strcmp(child->string, "hex") ? 512 : child->string && !strcmp(child->string, "output") ? 2048 : 1024;
            if (strlen(child->valuestring) > cap) {
                while (cap && ((unsigned char)child->valuestring[cap] & 0xc0) == 0x80) cap--;
                child->valuestring[cap] = 0; *truncated = 1;
            }
        } else preview_trim(child, truncated);
        count++; child = next;
    }
}
static uint64_t record_event(const char *operation, const char *source, int status, const cJSON *result) {
    int lock = open(GW_HISTORY_LOCK, O_RDWR | O_CREAT, 0600), truncated = 0; uint64_t sequence = 0;
    cJSON *record, *arguments, *preview; char path[256], number[40], iso[40], *old, *boot, *text;
    const char *keys[] = {"target", "command", "state", "name", "uart", "start", "limit", "filename", "request_id", "dry_run", "id", "mode", "files", "bytes", "baud1", "baud2", "action", "format", "tz"};
    size_t i; HistoryFile files[128]; size_t count;
    if (lock < 0 || flock(lock, LOCK_EX) < 0) { if (lock >= 0) close(lock); return 0; }
    old = gw_read(GW_HISTORY "/.sequence", 40); sequence = old ? strtoull(old, NULL, 10) + 1 : 1; free(old);
    snprintf(number, sizeof(number), "%llu\n", (unsigned long long)sequence);
    if (gw_atomic(GW_HISTORY "/.sequence", number, strlen(number)) < 0) { close(lock); return 0; }
    record = cJSON_CreateObject(); arguments = cJSON_CreateObject(); preview = cJSON_Duplicate(result, 1);
    cJSON_AddNumberToObject(record, "sequence", (double)sequence); cJSON_AddStringToObject(record, "operation", operation);
    cJSON_AddStringToObject(record, "source", source); cJSON_AddStringToObject(record, "method", !strcmp(source, "worker") ? "BACKGROUND" : request_method ? request_method : "CLI");
    cJSON_AddNumberToObject(record, "http_status", status); cJSON_AddBoolToObject(record, "ok", status < 400);
    gw_iso(gw_utc_us(), iso, sizeof(iso)); cJSON_AddStringToObject(record, "utc", iso); cJSON_AddNumberToObject(record, "uptime", gw_mono_us() / 1000000.0);
    boot = gw_read("/proc/sys/kernel/random/boot_id", 80); if (boot) { boot[strcspn(boot, "\r\n")] = 0; cJSON_AddStringToObject(record, "boot_id", boot); free(boot); }
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (*value(keys[i])) cJSON_AddStringToObject(arguments, keys[i], value(keys[i]));
    cJSON_AddItemToObject(record, "arguments", arguments); preview_trim(preview, &truncated); preview_trim(arguments, &truncated);
    text = cJSON_PrintUnformatted(preview);
    if (text && strlen(text) <= 4096) cJSON_AddItemToObject(record, "result", preview);
    else { cJSON_Delete(preview); if (text) { size_t cap = 4096; while (cap && ((unsigned char)text[cap] & 0xc0) == 0x80) cap--; text[cap] = 0; cJSON_AddStringToObject(record, "result_preview", text); } truncated = 1; }
    free(text); cJSON_AddBoolToObject(record, "preview_truncated", truncated);
    snprintf(path, sizeof(path), "%s/%020llu.json", GW_HISTORY, (unsigned long long)sequence);
    if (save_json(path, record) < 0) sequence = 0;
    cJSON_Delete(record);
    count = history_files(files); for (i = 0; i + 64 < count; i++) { snprintf(path, sizeof(path), "%s/%s", GW_HISTORY, files[i].name); unlink(path); }
    close(lock); return sequence;
}
static void api_activity(void) {
    HistoryFile files[128]; size_t count = history_files(files), i, shown = 0; unsigned limit = (unsigned)strtoul(value("limit"), NULL, 10);
    uint64_t after = strtoull(value("after"), NULL, 10); cJSON *json = cJSON_CreateObject(), *records = cJSON_CreateArray(); char path[256], *device;
    if (!limit || limit > 64) limit = 20;
    if (*value("after")) {
        for (i = 0; i < count && shown < limit; i++) if (files[i].sequence > after) { cJSON *record; snprintf(path, sizeof(path), "%s/%s", GW_HISTORY, files[i].name); record = json_file(path); if (record) { cJSON_AddItemToArray(records, record); shown++; } }
    } else {
        i = count > limit ? count - limit : 0;
        for (; i < count; i++) { cJSON *record; snprintf(path, sizeof(path), "%s/%s", GW_HISTORY, files[i].name); record = json_file(path); if (record) { cJSON_AddItemToArray(records, record); shown++; } }
    }
    cJSON_AddItemToObject(json, "records", records); cJSON_AddNumberToObject(json, "total", count);
    cJSON_AddNumberToObject(json, "latest_sequence", count ? (double)files[count - 1].sequence : 0);
    cJSON_AddNumberToObject(json, "oldest_sequence", count ? (double)files[0].sequence : 0); cJSON_AddNumberToObject(json, "history_limit", 64);
    device = gw_read("/sys/class/net/wlan0/address", 40); if (device) { device[strcspn(device, "\r\n")] = 0; cJSON_AddStringToObject(json, "device_id", device); free(device); }
    json_response(200, json);
}
