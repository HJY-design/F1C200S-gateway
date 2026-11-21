/* Stream a fixed raw/index snapshot. Original bytes and timestamps stay intact. */
typedef struct { unsigned char carry[4]; size_t used; } ExportDecoder;
typedef struct { ExportDecoder decoder; int line_start, skip_lf; } ExportText;
static void export_label(const GwStamp *stamp, int timezone, char label[96]) {
    if (!stamp) { strcpy(label, "时间戳缺失"); return; }
    if (!stamp->utc_us) { snprintf(label, 96, "启动后 %.3f 秒", stamp->monotonic_us / 1000000.0); return; }
    { time_t seconds = (time_t)(stamp->utc_us / 1000000) - (time_t)timezone * 60; struct tm date;
      if (gmtime_r(&seconds, &date)) {
          size_t n = strftime(label, 96, "%Y/%m/%d %H:%M:%S", &date);
          snprintf(label + n, 96 - n, ".%03u", (unsigned)(stamp->utc_us / 1000 % 1000));
      } else strcpy(label, "时间戳无效"); }
}
static void export_text_bytes(ExportDecoder *decoder, const unsigned char *bytes, size_t length, int final) {
    unsigned char buffer[8196]; size_t total = decoder->used + length, position = 0;
    memcpy(buffer, decoder->carry, decoder->used); if (length) memcpy(buffer + decoder->used, bytes, length); decoder->used = 0;
    while (position < total) {
        unsigned char first = buffer[position]; size_t need, j; unsigned low = 0x80, high = 0xbf;
        if (first < 0x80) { if (!first) fputs("␀", stdout); else putchar(first); position++; continue; }
        if (first >= 0xc2 && first <= 0xdf) need = 2;
        else if (first >= 0xe0 && first <= 0xef) { need = 3; if (first == 0xe0) low = 0xa0; if (first == 0xed) high = 0x9f; }
        else if (first >= 0xf0 && first <= 0xf4) { need = 4; if (first == 0xf0) low = 0x90; if (first == 0xf4) high = 0x8f; }
        else { fputs("�", stdout); position++; continue; }
        for (j = 1; j < need && position + j < total; j++) {
            unsigned char next = buffer[position + j];
            if (next < (j == 1 ? low : 0x80) || next > (j == 1 ? high : 0xbf)) break;
        }
        if (j == need) { fwrite(buffer + position, 1, need, stdout); position += need; }
        else if (position + j == total && !final) { decoder->used = j; memcpy(decoder->carry, buffer + position, j); break; }
        else { fputs("�", stdout); position += j; }
    }
}
static int export_next_stamp(FILE *index, uint64_t *remaining, uint64_t raw_size, GwStamp *stamp) {
    while (*remaining >= sizeof(*stamp)) {
        *remaining -= sizeof(*stamp);
        if (fread(stamp, sizeof(*stamp), 1, index) != 1) return 0;
        if (stamp->count && stamp->offset < raw_size && stamp->count <= raw_size - stamp->offset) return 1;
    }
    return 0;
}
static void export_hex_body(FILE *raw, FILE *index, uint64_t raw_size, uint64_t index_size, int timezone) {
    uint64_t offset = 0, remaining = index_size; GwStamp stamp; int have = export_next_stamp(index, &remaining, raw_size, &stamp);
    unsigned char bytes[16];
    while (offset < raw_size) {
        size_t n = (size_t)(raw_size - offset > 16 ? 16 : raw_size - offset), i; char label[96];
        if (fread(bytes, 1, n, raw) != n) break;
        while (have && stamp.offset + stamp.count <= offset) have = export_next_stamp(index, &remaining, raw_size, &stamp);
        export_label(have && stamp.offset <= offset ? &stamp : NULL, timezone, label);
        printf("%08llx  ", (unsigned long long)offset);
        for (i = 0; i < 16; i++) { if (i < n) printf("%02x%s", bytes[i], i == 15 ? "" : " "); else fputs(i == 15 ? "  " : "   ", stdout); }
        fputs("  ", stdout); for (i = 0; i < n; i++) putchar(bytes[i] >= 32 && bytes[i] < 127 ? bytes[i] : '.');
        for (; i < 16; i++) putchar(' ');
        printf("  %s\n", label); offset += n;
    }
}
static void export_text_span(FILE *raw, uint64_t from, uint64_t to, const GwStamp *stamp, int timezone, ExportText *text) {
    unsigned char bytes[8192];
    if (fseek(raw, (long)from, SEEK_SET)) return;
    while (from < to) {
        size_t n = (size_t)(to - from > sizeof(bytes) ? sizeof(bytes) : to - from), position = 0;
        n = fread(bytes, 1, n, raw); if (!n) break;
        while (position < n) {
            size_t end;
            if (text->skip_lf) { text->skip_lf = 0; if (bytes[position] == '\n') { position++; continue; } }
            if (text->line_start) {
                char label[96]; export_label(stamp, timezone, label);
                printf("[%s · @%llu] ", label, (unsigned long long)(from + position));
                text->line_start = 0;
            }
            if (bytes[position] == '\r' || bytes[position] == '\n') {
                export_text_bytes(&text->decoder, NULL, 0, 1); putchar('\n');
                text->line_start = 1; text->skip_lf = bytes[position] == '\r'; position++; continue;
            }
            for (end = position; end < n && bytes[end] != '\r' && bytes[end] != '\n'; end++) {}
            export_text_bytes(&text->decoder, bytes + position, end - position, 0); position = end;
        }
        from += n;
    }
}
static void export_text_body(FILE *raw, FILE *index, uint64_t raw_size, uint64_t index_size, int timezone) {
    uint64_t offset = 0, remaining = index_size; GwStamp stamp; ExportText text = { .line_start = 1 };
    while (export_next_stamp(index, &remaining, raw_size, &stamp)) {
        uint64_t end = stamp.offset + stamp.count;
        if (stamp.offset > offset) export_text_span(raw, offset, stamp.offset, NULL, timezone, &text);
        if (end > offset) export_text_span(raw, stamp.offset > offset ? stamp.offset : offset, end, &stamp, timezone, &text);
        if (end > offset) offset = end;
    }
    if (offset < raw_size) export_text_span(raw, offset, raw_size, NULL, timezone, &text);
    export_text_bytes(&text.decoder, NULL, 0, 1);
    if (!text.line_start) putchar('\n');
}
static void download_record(const char *format, uint64_t length) {
    if (record_response && request_operation) {
        cJSON *result = cJSON_CreateObject(); cJSON_AddStringToObject(result, "name", value("name"));
        cJSON_AddStringToObject(result, "format", format);
        if (!strcmp(format, "text") || !strcmp(format, "hex")) cJSON_AddNumberToObject(result, "raw_snapshot_bytes", (double)length);
        cJSON_AddStringToObject(result, "state", ferror(stdout) ? "stream_failed" : "stream_completed");
        record_event("download", cli_mode ? "cli" : "http", ferror(stdout) ? 500 : 200, result); cJSON_Delete(result); record_response = 0;
    }
}
static void api_export(const char *format) {
    char path[256]; FILE *raw, *index; struct stat raw_stat, index_stat; cJSON *meta;
    char *timezone_end; long timezone_long = strtol(value("tz"), &timezone_end, 10); int timezone; uint64_t raw_size, index_size;
    if ((*value("tz") && (*timezone_end || timezone_long < -840 || timezone_long > 840)) || gw_path(path, sizeof(path), value("name"), ".bin") < 0) { error_response(400, "文件名或时区无效"); return; }
    timezone = (int)timezone_long;
    raw = fopen(path, "rb"); if (!raw) { error_response(404, "日志已删除或不存在"); return; }
    gw_path(path, sizeof(path), value("name"), ".idx"); index = fopen(path, "rb");
    if (!index || fstat(fileno(raw), &raw_stat) < 0 || fstat(fileno(index), &index_stat) < 0) {
        fclose(raw); if (index) fclose(index); error_response(409, "日志时间戳索引不可用"); return;
    }
    raw_size = (uint64_t)raw_stat.st_size; index_size = (uint64_t)index_stat.st_size;
    gw_path(path, sizeof(path), value("name"), ".meta"); meta = json_file(path);
    headers(200, "text/plain; charset=utf-8");
    if (!cli_mode) printf("Content-Disposition: attachment; filename=\"%.*s-%s.txt\"\r\n\r\n", (int)strlen(value("name")) - 4, value("name"), format);
    fwrite("\xef\xbb\xbf", 1, 3, stdout);
    printf("F1C200S 串口日志 · %s\n文件：%s\n原始字节快照：%llu 字节\n显示时区：UTC%c%02d:%02d（导出时浏览器偏移）\n", !strcmp(format, "hex") ? "HEX" : "UTF-8 文本", value("name"), (unsigned long long)raw_size, timezone <= 0 ? '+' : '-', abs(timezone) / 60, abs(timezone) % 60);
    if (meta) {
        cJSON *boot = cJSON_GetObjectItem(meta, "boot"), *baud = cJSON_GetObjectItem(meta, "baud");
        if (cJSON_IsString(boot)) printf("启动编号：%s\n", boot->valuestring);
        if (cJSON_IsNumber(baud)) printf("串口波特率：%d\n", baud->valueint);
        cJSON_Delete(meta);
    }
    fputs("校时前使用启动后时间；HEX 行保留字节值，文本按 UTF-8 解码。\n文本按实际换行合并，每行时间为首字节的接收时刻（毫秒），不在接收数据块之间插入时间戳。\n\n", stdout);
    if (!strcmp(format, "hex")) export_hex_body(raw, index, raw_size, index_size, timezone);
    else export_text_body(raw, index, raw_size, index_size, timezone);
    fclose(raw); fclose(index); fflush(stdout); download_record(format, raw_size);
}
