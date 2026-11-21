#define main gateway_entry
#include "../src/gateway-api.c"
#undef main
#include <assert.h>

static void put(const char *path, const void *data, size_t length) {
    FILE *file = fopen(path, "wb"); assert(file); assert(fwrite(data, 1, length, file) == length); assert(!fclose(file));
}
static void elf_fixture(const char *path, uint32_t address, uint32_t offset, uint32_t size, uint32_t memory, uint16_t machine) {
    Elf32_Ehdr header = {0}; Elf32_Phdr segment = {0}; FILE *file = fopen(path, "wb");
    assert(file); memcpy(header.e_ident, ELFMAG, SELFMAG); header.e_ident[EI_CLASS] = ELFCLASS32;
    header.e_ident[EI_DATA] = ELFDATA2LSB; header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC; header.e_machine = machine; header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header); header.e_phoff = sizeof(header); header.e_phnum = 1; header.e_phentsize = sizeof(segment);
    segment.p_type = PT_LOAD; segment.p_offset = offset; segment.p_paddr = address; segment.p_vaddr = 0x20000000;
    segment.p_filesz = size; segment.p_memsz = memory;
    assert(fwrite(&header, sizeof(header), 1, file) == 1); assert(fwrite(&segment, sizeof(segment), 1, file) == 1);
    assert(!fseek(file, 256, SEEK_SET)); assert(fwrite("\x00\x01\x02\x03", 4, 1, file) == 1); assert(!fclose(file));
}
static void image_tests(void) {
    const char *path = GW_AUDIT "/fixture";
    const char valid[] = ":020000040800F2\n:0400000000010203F6\n:00000001FF\n";
    const char checksum[] = ":020000040800F2\n:0400000000010203F5\n:00000001FF\n";
    const char too_high[] = ":020000040801F1\n:0400000000010203F6\n:00000001FF\n";
    put(path, valid, strlen(valid)); assert(validate_hex(path, "f103"));
    put(path, valid, strlen(valid) - 12); assert(!validate_hex(path, "f103"));
    put(path, checksum, strlen(checksum)); assert(!validate_hex(path, "f103"));
    put(path, too_high, strlen(too_high)); assert(!validate_hex(path, "f103")); assert(validate_hex(path, "f407"));
    elf_fixture(path, 0x08000000, 256, 4, 4, EM_ARM); assert(validate_elf(path, "f103", 260));
    elf_fixture(path, 0x0800fffc, 256, 4, 4, EM_ARM); assert(validate_elf(path, "f103", 260));
    elf_fixture(path, 0x0800fffd, 256, 4, 4, EM_ARM); assert(!validate_elf(path, "f103", 260));
    elf_fixture(path, 0x08010000, 256, 4, 4, EM_ARM); assert(!validate_elf(path, "f103", 260)); assert(validate_elf(path, "f407", 260));
    elf_fixture(path, 0x20000000, 256, 4, 4, EM_ARM); assert(!validate_elf(path, "f103", 260));
    elf_fixture(path, 0x08000000, 260, 4, 4, EM_ARM); assert(!validate_elf(path, "f103", 260));
    elf_fixture(path, 0x08000000, 256, 4, 3, EM_ARM); assert(!validate_elf(path, "f103", 260));
    elf_fixture(path, 0x08000000, 256, 4, 4, EM_386); assert(!validate_elf(path, "f103", 260));
    elf_fixture(path, 0x08000000, 256, 0, 4, EM_ARM); assert(!validate_elf(path, "f103", 260));
    unlink(path); puts("HEX/ELF checksum, EOF, architecture, truncation, and target bounds: PASS");
}
static void log_fixture(unsigned id, unsigned port) {
    char name[64], path[256], data[100] = {0};
    snprintf(name, sizeof(name), "%020u-uart%u.bin", id, port); gw_path(path, sizeof(path), name, ".bin"); put(path, data, 100);
    gw_path(path, sizeof(path), name, ".idx"); put(path, data, 32); gw_path(path, sizeof(path), name, ".meta"); put(path, "{}", 2);
}
static int present(unsigned id, unsigned port, const char *ext) {
    char name[64], path[256]; snprintf(name, sizeof(name), "%020u-uart%u.bin", id, port); gw_path(path, sizeof(path), name, ext); return access(path, F_OK) == 0;
}
static void retention_tests(void) {
    GwConfig cfg = {0}; GwFile *files; const char *active1 = "00000000000000000002-uart1.bin", *active2 = "00000000000000000003-uart2.bin";
    assert(!gw_log_name("../../etc/shadow")); assert(!gw_log_name("1-uart3.bin")); assert(!gw_log_name("1-uart1.bin.."));
    log_fixture(1, 1); log_fixture(2, 1); log_fixture(3, 2); strcpy(cfg.mode, "files"); cfg.files = 1;
    assert(gw_prune(&cfg, active1, active2) == 1); assert(!present(1, 1, ".bin") && !present(1, 1, ".idx") && !present(1, 1, ".meta"));
    assert(present(2, 1, ".bin") && present(3, 2, ".bin"));
    log_fixture(1, 1); strcpy(cfg.mode, "none"); cfg.bytes = 1; assert(!gw_prune(&cfg, active1, active2)); assert(gw_files(&files) == 3); free(files);
    strcpy(cfg.mode, "bytes"); cfg.bytes = 269; assert(gw_prune(&cfg, active1, active2) == 1);
    cfg.bytes = 1; assert(!gw_prune(&cfg, active1, active2)); assert(gw_files(&files) == 2); free(files);
    puts("Retention by count/capacity/none, oldest-first, index accounting, and active protection: PASS");
}
static void audit_tests(void) {
    unsigned i, count = 0; char path[256]; DIR *directory; struct dirent *entry;
    for (i = 1; i <= 65; i++) {
        cJSON *record = cJSON_CreateObject(); cJSON_AddNumberToObject(record, "sequence", i); snprintf(path, sizeof(path), "%s/%032x.json", GW_AUDIT, i); assert(!save_json(path, record)); cJSON_Delete(record);
        snprintf(path, sizeof(path), "%s/%032x.txt", GW_AUDIT, i); put(path, "x", 1);
    }
    audit_trim(); directory = opendir(GW_AUDIT); assert(directory);
    while ((entry = readdir(directory))) if (strstr(entry->d_name, ".json")) count++;
    closedir(directory); assert(count == 64);
    snprintf(path, sizeof(path), "%s/%032x.json", GW_AUDIT, 1); assert(access(path, F_OK) < 0);
    snprintf(path, sizeof(path), "%s/%032x.txt", GW_AUDIT, 1); assert(access(path, F_OK) < 0);
    snprintf(path, sizeof(path), "%s/%032x.json", GW_AUDIT, 65); assert(!access(path, F_OK));
    puts("Flash history keeps newest 64 records and deletes matching output: PASS");
}
static void history_tests(void) {
    cJSON *result = cJSON_CreateObject(), *nested = cJSON_CreateObject(), *saved, *preview; unsigned i; char path[256], long_hex[600], *text;
    HistoryFile files[128]; memset(long_hex, 'f', sizeof(long_hex) - 1); long_hex[sizeof(long_hex) - 1] = 0;
    cJSON_AddStringToObject(result, "token", "SECRET-OUTER"); cJSON_AddStringToObject(nested, "token", "SECRET-INNER");
    cJSON_AddStringToObject(result, "hex", long_hex); cJSON_AddItemToObject(result, "status", nested); set_param("filename", "test.hex");
    for (i = 1; i <= 65; i++) assert(record_event("test", "cli", 200, result) == i);
    assert(history_files(files) == 64 && files[0].sequence == 2 && files[63].sequence == 65);
    snprintf(path, sizeof(path), "%s/%020u.json", GW_HISTORY, 65); text = gw_read(path, 10000); assert(text && !strstr(text, "SECRET-")); saved = cJSON_Parse(text); free(text);
    assert(saved && cJSON_IsTrue(cJSON_GetObjectItem(saved, "preview_truncated"))); preview = cJSON_GetObjectItem(saved, "result");
    assert(strlen(cJSON_GetObjectItem(preview, "hex")->valuestring) == 512); cJSON_Delete(saved); cJSON_Delete(result);
    puts("Operation history sequence, newest 64, bounded previews, and token omission: PASS");
}
static void export_tests(void) {
    GwStamp stamp = { .monotonic_us = 1234567 }; char label[96], output[512] = {0}; int saved; FILE *capture;
    ExportDecoder decoder = {0}; const unsigned char first[] = {0xe6,0xb1}, second[] = {0x89,0xe5,0xad,0x97,0,0xff,0xe4,0xb8};
    export_label(&stamp, 240, label); assert(!strcmp(label, "启动后 1.235 秒")); stamp.utc_us = 1700000000000000ULL;
    export_label(&stamp, 240, label); assert(!strcmp(label, "2023/11/14 18:13:20.000")); stamp.utc_us += 123000;
    export_label(&stamp, 240, label); assert(!strcmp(label, "2023/11/14 18:13:20.123")); export_label(NULL, 0, label); assert(!strcmp(label, "时间戳缺失"));
    fflush(stdout); saved = dup(STDOUT_FILENO); capture = tmpfile(); assert(saved >= 0 && capture && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    export_text_bytes(&decoder, first, sizeof(first), 0); export_text_bytes(&decoder, second, sizeof(second), 0); export_text_bytes(&decoder, NULL, 0, 1);
    fflush(stdout); rewind(capture); assert(fread(output, 1, sizeof(output)-1, capture) == strlen("汉字␀��")); assert(!strcmp(output, "汉字␀��"));
    assert(dup2(saved, STDOUT_FILENO) >= 0); close(saved); fclose(capture);
    puts("Combined export timezone, pre-sync labels, UTF-8 chunk boundaries, and invalid-byte display: PASS");
}
static char *text_export_fixture(const unsigned char *bytes, size_t length, const GwStamp *stamps, size_t count) {
    FILE *raw = tmpfile(), *index = tmpfile(), *capture = tmpfile(); int saved; long size; char *output;
    assert(raw && index && capture); assert(fwrite(bytes, 1, length, raw) == length);
    if (count) assert(fwrite(stamps, sizeof(*stamps), count, index) == count);
    rewind(raw); rewind(index); fflush(stdout); saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    export_text_body(raw, index, length, count * sizeof(*stamps), 0);
    fflush(stdout); size = ftell(capture); assert(size >= 0); output = calloc((size_t)size + 1, 1); assert(output);
    rewind(capture); assert(fread(output, 1, (size_t)size, capture) == (size_t)size);
    assert(dup2(saved, STDOUT_FILENO) >= 0); close(saved); fclose(raw); fclose(index); fclose(capture); return output;
}
static void text_line_tests(void) {
    const char message[] = "One complete sentence.\r\n中文温度正常。\r\ntail without LF";
    const size_t length = sizeof(message) - 1, second = strlen("One complete sentence.\r\n"), third = strlen("One complete sentence.\r\n中文温度正常。\r\n");
    GwStamp *stamps = calloc(length, sizeof(*stamps)); size_t i; char expected[1024], *output;
    assert(stamps);
    for (i = 0; i < length; i++) { stamps[i].offset = i; stamps[i].count = 1; stamps[i].monotonic_us = 1000000 + i * 1000; }
    output = text_export_fixture((const unsigned char *)message, length, stamps, length);
    snprintf(expected, sizeof(expected), "[启动后 1.000 秒 · @0] One complete sentence.\n[启动后 %.3f 秒 · @%zu] 中文温度正常。\n[启动后 %.3f 秒 · @%zu] tail without LF\n", 1 + second / 1000.0, second, 1 + third / 1000.0, third);
    assert(!strcmp(output, expected)); free(output); free(stamps);
    output = text_export_fixture((const unsigned char *)"raw\n", 4, NULL, 0);
    assert(!strcmp(output, "[时间戳缺失 · @0] raw\n")); free(output);
    {
        unsigned char *long_line = malloc(8200); GwStamp stamp = { .count = 8199, .monotonic_us = 1000000 };
        assert(long_line); memset(long_line, 'x', 8191); memcpy(long_line + 8191, "汉\r\nend", 8);
        output = text_export_fixture(long_line, 8199, &stamp, 1);
        assert(!strstr(output, "�") && strstr(output, "汉\n[启动后 1.000 秒 · @8196] end\n"));
        assert(strlen(output) < 8400); free(output); free(long_line);
    }
    {
        const unsigned char invalid[] = { 0xe6, '\r', '\n', 0, 0xff, '\n' };
        output = text_export_fixture(invalid, sizeof(invalid), NULL, 0);
        assert(!strcmp(output, "[时间戳缺失 · @0] �\n[时间戳缺失 · @3] ␀�\n")); free(output);
    }
    puts("Text export preserves complete lines across per-byte receive marks, split UTF-8/CRLF, missing timestamps and 8 KiB buffers: PASS");
}
static void diagnose_tests(void) {
    cJSON *json = cJSON_CreateObject(), *counts; char messages[2048] = "esp32_spi: Config - SPI clock[2MHz] mode[2] max[2000000Hz]\nESP_RX_CHECKSUM_ERROR\nSPI Transaction failed: -1\nTransport recovery: started\nprocess_disconnect_event: ignored\n";
    unsigned i; char path[256]; cJSON *list, *record;
    diagnose_kernel_lines(json, messages); counts = cJSON_GetObjectItem(json, "matches");
    assert(cJSON_GetObjectItem(counts, "spi_checksum")->valueint == 1 && cJSON_GetObjectItem(counts, "spi_transaction")->valueint == 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(json, "spi_clock_messages")) == 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(json, "recent_messages")) == 4); cJSON_Delete(json);
    json = cJSON_CreateArray(); for (i = 0; i < 30; i++) diagnose_recent_string(json, "test", 20); assert(cJSON_GetArraySize(json) == 20); cJSON_Delete(json);
    snprintf(path, sizeof(path), "%s/number-test", GW_HISTORY); put(path, "invalid", 7); json = cJSON_CreateObject(); diagnose_number(json, "unavailable", path); assert(cJSON_IsNull(cJSON_GetObjectItem(json, "unavailable"))); cJSON_Delete(json); unlink(path);
    json = cJSON_CreateObject(); cJSON_AddBoolToObject(json, "ok", 0); assert(record_event("expected-negative-test", "cli", 409, json) == 66); cJSON_Delete(json);
    json = cJSON_CreateObject(); cJSON_AddBoolToObject(json, "ok", 1); cJSON_AddStringToObject(json, "state", "failed"); assert(record_event("flash-result", "worker", 200, json) == 67); cJSON_Delete(json);
    list = diagnose_failures(); assert(cJSON_GetArraySize(list) == 2); record = cJSON_GetArrayItem(list, 0); assert(cJSON_GetObjectItem(record, "sequence")->valueint == 67); cJSON_Delete(list);
    puts("Diagnostics retained-message scope, bounded output, unknown counters, and failed task detection: PASS");
}
int main(void) { image_tests(); retention_tests(); audit_tests(); history_tests(); export_tests(); text_line_tests(); diagnose_tests(); return 0; }
