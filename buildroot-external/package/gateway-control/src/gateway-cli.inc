/* Included by gateway-api.c: SSH CLI and HTTP share dispatch and operations. */
static int cli_error(const char *message) { error_response(400, message); return response_exit; }
static const char *uart_number(const char *text) {
    if (!strcmp(text, "uart1") || !strcmp(text, "1")) return "1";
    if (!strcmp(text, "uart2") || !strcmp(text, "2")) return "2";
    return NULL;
}
static int cli_log(int argc, char **argv) {
    const char *port, *op; int i; char name[64] = "", start[32];
    if (argc < 3) return cli_error("需要 uart1/uart2、list、view、download 或 delete");
    port = uart_number(argv[2]);
    if (port) {
        GwFile *files; size_t count = gw_files(&files), n; struct stat st; char path[256]; unsigned long tail = 8192;
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--tail") && i + 1 < argc) { tail = strtoul(argv[++i], NULL, 10); if (!tail || tail > 8192) { free(files); return cli_error("tail 必须在 1..8192 字节之间"); } }
            else { free(files); return cli_error("日志选项无效"); }
        }
        for (n = count; n > 0; n--) if (files[n - 1].port == (unsigned)atoi(port)) { strcpy(name, files[n - 1].name); break; }
        free(files);
        if (!*name) { cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "uart", port); cJSON_AddBoolToObject(j, "empty", 1); cJSON_AddStringToObject(j, "hex", ""); json_response(200, j); return response_exit; }
        gw_path(path, sizeof(path), name, ".bin");
        if (stat(path, &st) < 0) return cli_error("日志文件已变化，请重试");
        snprintf(start, sizeof(start), "%llu", (unsigned long long)(st.st_size > (off_t)tail ? st.st_size - (off_t)tail : 0));
        set_param("name", name); set_param("start", start); snprintf(path, sizeof(path), "%lu", tail); set_param("limit", path);
        dispatch("view", "GET", 0); return response_exit;
    }
    if (!strcmp(argv[2], "list")) {
        if (argc > 4 || (argc == 4 && !uart_number(argv[3]))) return cli_error("list 后可选 uart1 或 uart2");
        if (argc == 4) set_param("uart", uart_number(argv[3]));
        dispatch("logs", "GET", 0); return response_exit;
    }
    if (argc < 4) return cli_error("需要日志文件名");
    if (!strcmp(argv[2], "view")) op = "view";
    else if (!strcmp(argv[2], "download")) op = "download";
    else if (!strcmp(argv[2], "delete")) op = "log-delete";
    else return cli_error("未知日志命令");
    set_param("name", argv[3]);
    for (i = 4; i < argc; i++) {
        if ((!strcmp(argv[i], "--start") || !strcmp(argv[i], "--limit") || !strcmp(argv[i], "--format") || !strcmp(argv[i], "--tz")) && i + 1 < argc) { const char *key = argv[i] + 2; set_param(key, argv[++i]); }
        else return cli_error("日志选项无效");
    }
    dispatch(op, !strcmp(op, "log-delete") ? "POST" : "GET", 0); return response_exit;
}
static int cli_main(int argc, char **argv) {
    const char *command = argc > 1 ? argv[1] : "help", *op = NULL, *method = "GET"; int i;
    cli_mode = 1; gw_prepare(); request_operation = command; request_method = "CLI"; record_response = 1;
    if (!strcmp(command, "help") || !strcmp(command, "--help")) {
        puts("gateway diagnose | status | capabilities | activity [--after SEQUENCE] | target [get|set f103|f407]\n"
             "gateway flash FILE.hex|FILE.elf [--target f103|f407] [--async] [--wait SECONDS] [--dry-run] [--request-id ID]\n"
             "gateway reset [--via stlink|gpio] [--target f103|f407] | bootloader\n"
             "gateway gpio nrst low|release | gpio boot low|high | gpio usb on|off\n"
             "gateway log uart1|uart2 [--tail BYTES] | log list [uart1|uart2]\n"
             "gateway log view|download|delete NAME [--start BYTES] [--limit BYTES] [--format text|hex|raw|timestamps|meta|index] [--tz MINUTES]\n"
             "gateway jobs | job ID | settings | test run | param get|set KEY VALUE|save|rollback\n"
             "gateway mcu COMMAND [ARGS ...] | sample inertial (read-only, no action history)\n"
             "gateway request OP [--post] [KEY=VALUE ...]"); return 0;
    }
    if (!strcmp(command, "flash")) {
        struct stat st; int fd; const char *name;
        if (argc < 3) return cli_error("需要 HEX/ELF 文件路径");
        set_param("target", default_target()); cli_wait_seconds = 240;
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--async")) cli_wait_seconds = 0;
            else if (!strcmp(argv[i], "--dry-run")) set_param("dry_run", "1");
            else if ((!strcmp(argv[i], "--target") || !strcmp(argv[i], "--request-id")) && i + 1 < argc) { const char *key = !strcmp(argv[i], "--target") ? "target" : "request_id"; set_param(key, argv[++i]); }
            else if (!strcmp(argv[i], "--wait") && i + 1 < argc) { cli_wait_seconds = atoi(argv[++i]); if (cli_wait_seconds < 1 || cli_wait_seconds > 600) return cli_error("wait 必须在 1..600 秒之间"); }
            else return cli_error("烧录选项无效");
        }
        fd = open(argv[2], O_RDONLY);
        if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 4194304) { if (fd >= 0) close(fd); return cli_error("文件不可读或超过 4 MiB"); }
        name = strrchr(argv[2], '/'); set_param("filename", name ? name + 1 : argv[2]);
        if (dup2(fd, STDIN_FILENO) < 0) { close(fd); return cli_error("文件打开失败"); } if (fd != STDIN_FILENO) close(fd);
        dispatch("flash", "POST", (size_t)st.st_size); return response_exit;
    }
    if (!strcmp(command, "log")) return cli_log(argc, argv);
    if (!strcmp(command, "activity")) {
        if (argc == 4 && !strcmp(argv[2], "--after")) set_param("after", argv[3]);
        else if (argc != 2) return cli_error("需要 activity 或 activity --after SEQUENCE");
        dispatch("activity", "GET", 0); return response_exit;
    }
    if (!strcmp(command, "reset")) {
        const char *via = "stlink";
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--via") && i + 1 < argc) via = argv[++i];
            else if (!strcmp(argv[i], "--target") && i + 1 < argc) set_param("target", argv[++i]);
            else return cli_error("复位选项无效");
        }
        if (!strcmp(via, "gpio")) { op = "gpio"; set_param("command", "reset"); }
        else if (!strcmp(via, "stlink")) op = "target-reset";
        else return cli_error("via 必须是 stlink 或 gpio");
        method = "POST";
    } else if (!strcmp(command, "bootloader") && argc == 2) { op = "gpio"; method = "POST"; set_param("command", "rom"); }
    else if (!strcmp(command, "gpio") && argc == 4) {
        method = "POST"; op = "gpio";
        if (!strcmp(argv[2], "nrst") && !strcmp(argv[3], "low")) set_param("command", "reset-low");
        else if (!strcmp(argv[2], "nrst") && !strcmp(argv[3], "release")) set_param("command", "reset-release");
        else if (!strcmp(argv[2], "boot") && !strcmp(argv[3], "low")) set_param("command", "boot-low");
        else if (!strcmp(argv[2], "boot") && !strcmp(argv[3], "high")) set_param("command", "boot-high");
        else if (!strcmp(argv[2], "usb")) { op = "usb-power"; set_param("state", argv[3]); }
        else return cli_error("GPIO 命令无效");
    } else if (!strcmp(command, "target")) {
        op = "target";
        if (argc == 4 && !strcmp(argv[2], "set")) { set_param("target", argv[3]); method = "POST"; }
        else if (argc != 2 && !(argc == 3 && !strcmp(argv[2], "get"))) return cli_error("需要 target get 或 target set f103|f407");
    } else if(!strcmp(command,"supervise") && argc==5 && !strcmp(argv[2],"vehicle")) {
        int descriptor=open("/run/gateway-flash.lock",O_RDWR|O_CREAT,0600);
        if(descriptor<0 || flock(descriptor,LOCK_SH|LOCK_NB)<0) { if(descriptor>=0)close(descriptor);return cli_error("目标正在烧录或复位，车辆测试未启动"); }
        if(descriptor!=9) { if(dup2(descriptor,9)<0) {close(descriptor);return cli_error("无法预留目标操作锁");}close(descriptor); }
        execl("/usr/bin/gateway-vehicle-test","gateway-vehicle-test","--locked",argv[3],argv[4],(char *)NULL);
        close(9);return cli_error("车辆测试脚本不可用");
    } else if(!strcmp(command,"supervise") && argc==3 && !strcmp(argv[2],"drive")) {
        set_param("command","drive.keepalive"); record_response=0; api_mcu("mcu-rpc"); return response_exit;
    } else if(!strcmp(command,"sample") && argc==3 && !strcmp(argv[2],"inertial")) {
        set_param("command","ins.get"); record_response=0; api_mcu("mcu-rpc"); return response_exit;
    } else if (!strcmp(command,"mcu") && argc>=3) {
        char text[100]={0}; size_t length=0;
        for(i=2;i<argc;i++) { size_t part=strlen(argv[i]); if(length+part+(i>2)>=sizeof(text)) return cli_error("命令过长");
            if(i>2) text[length++]=' '; memcpy(text+length,argv[i],part+1); length+=part; }
        set_param("command",text); dispatch("mcu-rpc","POST",0); return response_exit;
    } else if (!strcmp(command, "param") && argc >= 3) {
        if(!strcmp(argv[2],"get") && argc==3) op="param-get";
        else if(!strcmp(argv[2],"set") && argc==5) { op="param-set";method="POST";set_param("key",argv[3]);set_param("value",argv[4]); }
        else if(!strcmp(argv[2],"save") && argc==3) { op="param-save";method="POST"; }
        else if(!strcmp(argv[2],"rollback") && argc==3) { op="param-rollback";method="POST"; }
        else return cli_error("需要 param get|set KEY VALUE|save|rollback");
    } else if (!strcmp(command, "test") && argc == 3 && !strcmp(argv[2], "run")) op = "test";
    else if (!strcmp(command, "job") && argc == 3) { op = "job"; set_param("id", argv[2]); }
    else if ((!strcmp(command, "diagnose") || !strcmp(command, "status") || !strcmp(command, "capabilities") || !strcmp(command, "jobs") || !strcmp(command, "settings")) && argc == 2) op = command;
    else if (!strcmp(command, "request") && argc >= 3) {
        op = argv[2];
        for (i = 3; i < argc; i++) {
            char *equals;
            if (!strcmp(argv[i], "--post")) { method = "POST"; continue; }
            equals = strchr(argv[i], '='); if (!equals) return cli_error("请求参数需要 KEY=VALUE"); *equals++ = 0; set_param(argv[i], equals);
        }
    }
    if (!op) return cli_error("未知命令或参数，使用 gateway --help 查看支持的命令");
    dispatch(op, method, 0); return response_exit;
}
