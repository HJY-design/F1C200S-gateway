/* The selected UART has one RX owner: gateway-logger. RPC only opens TX. */
#include <termios.h>

static unsigned mcu_uart(void) {
    FILE *file=fopen("/etc/gateway/mcu-uart","r"); unsigned port=1, configured;
    if(file) { if(fscanf(file,"%u",&configured)==1 && (configured==1 || configured==2)) port=configured; fclose(file); }
    return port;
}

typedef struct { char body[1200]; size_t used; int collecting; } McuDecoder;
static uint16_t mcu_crc(const char *text, size_t length) {
    uint16_t crc=0xffff; size_t i; unsigned bit;
    for(i=0;i<length;i++) { crc^=(uint16_t)(unsigned char)text[i]<<8;
        for(bit=0;bit<8;bit++) crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0)); }
    return crc;
}
static cJSON *mcu_decode(McuDecoder *decoder, unsigned char byte) {
    char *star; unsigned expected; cJSON *message, *version;
    if(byte=='@') { decoder->used=0; decoder->collecting=1; return NULL; }
    if(!decoder->collecting) return NULL;
    if(byte!='\n') {
        if(decoder->used+1>=sizeof(decoder->body)) decoder->collecting=0;
        else decoder->body[decoder->used++]=(char)byte;
        return NULL;
    }
    decoder->collecting=0;
    while(decoder->used && decoder->body[decoder->used-1]=='\r') decoder->used--;
    decoder->body[decoder->used]=0;
    if(decoder->used<6) return NULL;
    star=decoder->body+decoder->used-5;
    if(*star!='*' || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2]) ||
       !isxdigit((unsigned char)star[3]) || !isxdigit((unsigned char)star[4])) return NULL;
    expected=(unsigned)strtoul(star+1,NULL,16);
    if(mcu_crc(decoder->body,(size_t)(star-decoder->body))!=expected) return NULL;
    *star=0; message=cJSON_Parse(decoder->body); version=cJSON_GetObjectItem(message,"v");
    if(!cJSON_IsObject(message) || !cJSON_IsNumber(version) || version->valuedouble!=1) {
        cJSON_Delete(message); return NULL;
    }
    return message;
}
static int mcu_log_open(int old, char current[64], off_t initial_tail) {
    cJSON *stats=json_file(GW_STATS), *active=cJSON_GetObjectItem(stats,"active");
    unsigned port=mcu_uart();
    cJSON *name=cJSON_GetArrayItem(active,(int)port-1); char path[256],suffix[20]; int fd;
    snprintf(suffix,sizeof(suffix),"-uart%u.bin",port);
    if(!cJSON_IsString(name) || !gw_log_name(name->valuestring) ||
       !strstr(name->valuestring,suffix) || strlen(name->valuestring)>=64) {
        cJSON_Delete(stats); return old;
    }
    if(old>=0 && !strcmp(current,name->valuestring)) { cJSON_Delete(stats); return old; }
    gw_path(path,sizeof(path),name->valuestring,".bin"); fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd>=0) {
        struct stat st;
        if(old<0 && fstat(fd,&st)==0) {
            off_t offset=initial_tail<0?st.st_size:(st.st_size>initial_tail?st.st_size-initial_tail:0);
            (void)lseek(fd,offset,SEEK_SET);
        }
        if(old>=0) close(old);
        snprintf(current,64,"%s",name->valuestring); old=fd;
    }
    cJSON_Delete(stats); return old;
}
static void mcu_cache_message(cJSON *message) {
    const char *path=NULL; cJSON *cache;
    if(cJSON_IsString(cJSON_GetObjectItem(message,"firmware"))) path="/run/gateway-f407-status.json";
    else if(cJSON_IsString(cJSON_GetObjectItem(message,"frame")) && cJSON_GetObjectItem(message,"vehicle_pose_valid")) path="/run/gateway-f407-ins.json";
    else if(cJSON_IsObject(cJSON_GetObjectItem(message,"mpu6050"))) path="/run/gateway-f407-sensors.json";
    if(!path) return;
    cache=cJSON_CreateObject(); cJSON_AddNumberToObject(cache,"seen_us",(double)gw_mono_us());
    cJSON_AddItemToObject(cache,"data",cJSON_Duplicate(message,1)); (void)save_json(path,cache); cJSON_Delete(cache);
}
static void mcu_update_cache(void) {
    static int fd=-1; static char name[64]; static McuDecoder decoder;
    unsigned char bytes[4096]; ssize_t count; size_t total=0,i;
    fd=mcu_log_open(fd,name,65536);
    while(fd>=0 && total<65536 && (count=read(fd,bytes,sizeof(bytes)))>0) {
        total+=(size_t)count;
        for(i=0;i<(size_t)count;i++) {
            cJSON *message=mcu_decode(&decoder,bytes[i]);
            if(message) { mcu_cache_message(message); cJSON_Delete(message); }
        }
    }
}
static cJSON *mcu_cached(void) {
    cJSON *result=cJSON_CreateObject(), *status=json_file("/run/gateway-f407-status.json");
    cJSON *sensors=json_file("/run/gateway-f407-sensors.json"), *seen=cJSON_GetObjectItem(status,"seen_us");
    double age=cJSON_IsNumber(seen)?((double)gw_mono_us()-seen->valuedouble)/1000.0:-1;
    cJSON_AddBoolToObject(result,"online",age>=0 && age<4000);
    cJSON_AddNumberToObject(result,"gateway_uart",mcu_uart());
    cJSON_AddNumberToObject(result,"age_ms",age);
    cJSON_AddItemToObject(result,"status",status?status:cJSON_CreateNull());
    { cJSON *ins=json_file("/run/gateway-f407-ins.json"); cJSON_AddItemToObject(result,"inertial",ins?ins:cJSON_CreateNull()); }
    cJSON_AddItemToObject(result,"sensors",sensors?sensors:cJSON_CreateNull()); return result;
}
static int mcu_command_valid(const char *command) {
    static const char *allowed[]={"status","ping","sensors","sensors.probe","sensors.bmi_probe","fault.get",
        "stop","param.get","param.set","param.save","param.rollback","bench.all","bench.m3","bench.servo9",
        "ins.get","ins.zero","ins.reset","motion.get","motion.configure","motion.preview","motion.reset","imu.get","imu.mount","imu.zero","robot.get","nav.get","mission.get","control.get",
        "drive.get","drive.configure","drive.bounds","drive.imu_bind","drive.arm","drive.keepalive","drive.velocity","drive.test","drive.stop"};
    size_t i,length=strlen(command),first=strcspn(command," ");
    if(!length || length>96) return 0;
    for(i=0;i<length;i++) if((unsigned char)command[i]<32 || (unsigned char)command[i]>126 || command[i]=='@' || command[i]=='*') return 0;
    for(i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++) if(strlen(allowed[i])==first && !strncmp(command,allowed[i],first)) return 1;
    return 0;
}
static cJSON *mcu_rpc(const char *command, int *http_status, const char **error) {
    int lock=-1,flash=-1,serial=-1,log=-1; char name[64]={0},body[128],frame[144];
    unsigned char bytes[2048]; ssize_t count; size_t i; unsigned id; int n;
    uint64_t deadline; McuDecoder decoder={0}; cJSON *reply=NULL; struct termios settings;
    *http_status=503; *error="F407 串口未响应";
    if(!mcu_command_valid(command)) { *http_status=400; *error="F407 命令或参数无效"; return NULL; }
    flash=open("/run/gateway-flash.lock",O_RDWR|O_CREAT|O_CLOEXEC,0600);
    lock=open("/run/gateway-f407-tx.lock",O_RDWR|O_CREAT|O_CLOEXEC,0600);
    if(flash<0 || lock<0 || flock(flash,LOCK_SH|LOCK_NB)<0 || flock(lock,LOCK_EX|LOCK_NB)<0) {
        *http_status=409; *error="目标正在烧录或执行其他串口请求"; goto done;
    }
    log=mcu_log_open(-1,name,-1);
    if(log<0) { *error="所选串口记录服务尚未收到目标数据"; goto done; }
    { char device[32]; snprintf(device,sizeof(device),"/dev/ttyS%u",mcu_uart());
      serial=open(device,O_WRONLY|O_NOCTTY|O_NONBLOCK|O_CLOEXEC); }
    if(serial<0 || tcgetattr(serial,&settings)<0) { *error="所选串口发送端不可用"; goto done; }
    if(cfgetospeed(&settings)!=B115200 || (settings.c_cflag&CSIZE)!=CS8 ||
       (settings.c_cflag&(PARENB|CSTOPB))) { *http_status=409; *error="F407 联动需要所选串口配置为 115200 8N1"; goto done; }
    id=(unsigned)((gw_mono_us()^((uint64_t)getpid()<<12))&0x7fffffff); if(!id) id=1;
    n=snprintf(body,sizeof(body),"1 %u %s",id,command);
    n=snprintf(frame,sizeof(frame),"@%s*%04X\r\n",body,(unsigned)mcu_crc(body,(size_t)n));
    if(write(serial,frame,(size_t)n)!=n) { *error="UART1 命令发送未完成"; goto done; }
    deadline=gw_mono_us()+3000000;
    while(gw_mono_us()<deadline) {
        while((count=read(log,bytes,sizeof(bytes)))>0) for(i=0;i<(size_t)count;i++) {
            cJSON *message=mcu_decode(&decoder,bytes[i]), *number;
            if(!message) continue;
            number=cJSON_GetObjectItem(message,"id");
            if(cJSON_IsNumber(number) && number->valuedouble==id) {
                reply=message; *http_status=cJSON_IsTrue(cJSON_GetObjectItem(reply,"ok"))?200:422; goto done;
            }
            cJSON_Delete(message);
        }
        log=mcu_log_open(log,name,0);
        { struct timespec wait={0,20000000}; nanosleep(&wait,NULL); }
    }
done:
    if(serial>=0) close(serial);
    if(log>=0) close(log);
    if(lock>=0) close(lock);
    if(flash>=0) close(flash);
    return reply;
}
static void api_mcu(const char *operation) {
    char command[100]; const char *error; int status; cJSON *reply;
    if(!strcmp(operation,"mcu-cached")) { json_response(200,mcu_cached()); return; }
    if(!strcmp(operation,"param-get")) strcpy(command,"param.get");
    else if(!strcmp(operation,"param-save")) strcpy(command,"param.save");
    else if(!strcmp(operation,"param-rollback")) strcpy(command,"param.rollback");
    else if(!strcmp(operation,"param-set")) {
        const char *key=value("key"), *number=value("value"); char *end; long parsed; size_t i;
        if(!*key || strlen(key)>40 || !*number) { error_response(400,"需要参数名和整数值"); return; }
        for(i=0;key[i];i++) if(!isalnum((unsigned char)key[i]) && key[i]!='_' && key[i]!='.') { error_response(400,"参数名无效"); return; }
        errno=0; parsed=strtol(number,&end,10);
        if(errno || *end || parsed<INT32_MIN || parsed>INT32_MAX) { error_response(400,"参数值必须是32位整数"); return; }
        snprintf(command,sizeof(command),"param.set %s %ld",key,parsed);
    } else {
        if(strlen(value("command"))>=sizeof(command)) { error_response(400,"命令过长"); return; }
        strcpy(command,value("command"));
    }
    reply=mcu_rpc(command,&status,&error);
    if(reply) {
        cJSON_AddNumberToObject(reply,"gateway_uart",mcu_uart());
        if(status>=400 && !cJSON_GetObjectItem(reply,"error")) {
            cJSON *code=cJSON_GetObjectItem(reply,"code");
            cJSON_AddStringToObject(reply,"error",cJSON_IsString(code)?code->valuestring:"主控拒绝请求");
        }
        json_response(status,reply);
    } else error_response(status,error);
}
