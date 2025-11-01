#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif
static int xmemfd_create(const char *name, unsigned int flags){
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    (void)name; (void)flags; errno = ENOSYS; return -1;
#endif
}
static int create_shm_fd(size_t bytes){
    int fd = xmemfd_create("ph-dummy", MFD_CLOEXEC);
    if(fd>=0){
        if(ftruncate(fd, (off_t)bytes)<0){ int e=errno; close(fd); errno=e; return -1; }
        return fd;
    }
    char name[64]; snprintf(name, sizeof name, "/ph-dummy-%d", getpid());
    fd = shm_open(name, O_CREAT|O_RDWR, 0600);
    if(fd<0) return -1;
    shm_unlink(name);
    if(ftruncate(fd, (off_t)bytes)<0){ int e=errno; close(fd); errno=e; return -1; }
    return fd;
}
struct shm_hdr { uint32_t magic, version; _Atomic uint64_t seq; uint32_t used, capacity; uint8_t data[]; };
static void sleep_ms(int ms){ struct timespec ts={ ms/1000, (ms%1000)*1000000L }; nanosleep(&ts,NULL); }


static const char *g_sock = NULL;
static pthread_t g_thr;
static int g_run = 0;

static void publish_utf8(int fd, const char *feed, const char *msg){
    char js[POC_MAX_JSON];
    // data is raw UTF-8 string; no escaping performed beyond basic JSON quotes needs
    // Simple escape of double-quotes and backslashes
    char buf[POC_MAX_JSON/2];
    size_t bi = 0;
    for(const char *p = msg; *p && bi+2 < sizeof buf; ++p){
        if(*p=='"' || *p=='\\'){
            buf[bi++]='\\';
        }
        buf[bi++]=*p;
    }
    buf[bi]=0;
    snprintf(js, sizeof js, "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"%s\",\"encoding\":\"utf8\"}", feed, buf);
    send_frame_json(fd, js, strlen(js));
}

static void create_feed(int fd, const char *feed){
    char js[256];
    snprintf(js, sizeof js, "{\"type\":\"create_feed\",\"feed\":\"%s\"}", feed);
    send_frame_json(fd, js, strlen(js));
}

static void subscribe(int fd, const char *feed){
    char js[256];
    snprintf(js, sizeof js, "{\"type\":\"subscribe\",\"feed\":\"%s\"}", feed);
    send_frame_json(fd, js, strlen(js));
}

static void handle_command(int fd, const char *cmdline){
    // trim leading spaces
    while(*cmdline==' '||*cmdline=='\t') cmdline++;
    if(strncmp(cmdline,"help",4)==0){
        publish_utf8(fd, "dummy.config.out",
            "dummy addon — commands:\\n"
            "  help                : show this help\\n"
            "  ping                : respond with pong\\n"
            "  foo [text]          : emit a message on dummy.foo (and ack here)\\n"
            "Hints: use `ph-cli pub dummy.config.in \"ping\"`");
    } else if(strncmp(cmdline,"ping",4)==0){
        publish_utf8(fd, "dummy.config.out", "pong");
    } else if(strncmp(cmdline,"foo",3)==0){
        const char *arg = cmdline+3;
        while(*arg==' '||*arg=='\t') arg++;
        if(*arg==0) arg = "bar";
        publish_utf8(fd, "dummy.foo", arg);
        char ack[512];
        snprintf(ack, sizeof ack, "foo => published \"%s\" to dummy.foo", arg);
        publish_utf8(fd, "dummy.config.out", ack);
    } else {
        publish_utf8(fd, "dummy.config.out", "unknown command — try: help, ping, foo");
    }
}

static void *run(void *arg){
    (void)arg;
    int fd = -1;
    // Connect retry loop
    for(int i=0;i<50;i++){
        fd = uds_connect(g_sock ? g_sock : PH_SOCK_PATH);
        if(fd>=0) break;
        sleep_ms(100);
    }
    if(fd<0) return NULL;

    // Create feeds and subscribe
    create_feed(fd, "dummy.config.in");
    create_feed(fd, "dummy.config.out");
    create_feed(fd, "dummy.foo");
    subscribe(fd, "dummy.config.in");

    g_run = 1;
    char js[POC_MAX_JSON];
    while(g_run){
        int got = recv_frame_json(fd, js, sizeof js, 250);
        if(got<=0) continue;
        // expect {"type":"publish","feed":"dummy.config.in",...}
        char type[32]={0};
        char feed[128]={0};
        if(json_get_type(js, type, sizeof type)==0 &&
           strcmp(type,"publish")==0 &&
           json_get_string(js,"feed", feed, sizeof feed)==0 &&
           strcmp(feed,"dummy.config.in")==0){
            char enc[32]={0};
            char data[POC_MAX_JSON]={0};
            if(json_get_string(js,"encoding",enc,sizeof enc)!=0){
                strcpy(enc,"utf8");
            }
            if(json_get_string(js,"data",data,sizeof data)==0){
                if(strcmp(enc,"base64")==0){
                    // decode base64 to utf8 command (best-effort)
                    size_t maxlen = b64_decoded_maxlen(strlen(data));
                    char *tmp = (char*)malloc(maxlen+1);
                    size_t outlen = 0;
                    if(tmp && b64_decode(data, strlen(data), (uint8_t*)tmp, &outlen)==0){
                        tmp[outlen]=0;
                        handle_command(fd, tmp);
                    } else {
                        publish_utf8(fd, "dummy.config.out", "decode error");
                    }
                    if(tmp) free(tmp);
                } else {
                    if(strncmp(data,"shm-demo",8)==0 || strncmp(data,"ping",4)==0 || strncmp(data,"foo",3)==0){
                    size_t cap = 1<<20;
                    int sfd = create_shm_fd(sizeof(struct shm_hdr)+cap);
                    if(sfd>=0){
                        void *map = mmap(NULL, sizeof(struct shm_hdr)+cap, PROT_READ|PROT_WRITE, MAP_SHARED, sfd, 0);
                        if(map && map!=MAP_FAILED){
                            struct shm_hdr *hdr = (struct shm_hdr*)map;
                            hdr->magic=0x50485348; hdr->version=1; atomic_store(&hdr->seq, 0); hdr->used=0; hdr->capacity=(uint32_t)cap;
                            for(size_t i=0;i<cap;i++) hdr->data[i]=(uint8_t)(i&0xFF);
                            hdr->used = (uint32_t)cap;
                            char jsmap[POC_MAX_JSON];
                            int len = snprintf(jsmap, sizeof jsmap,
                                "{\"type\":\"publish\",\"feed\":\"dummy.foo\",\"subtype\":\"shm_map\",\"size\":%u,\"desc\":\"dummy 1MiB buffer\",\"mode\":\"rw\"}",
                                hdr->capacity);
                            int fds[1]={sfd};
                            send_frame_json_with_fds(fd, jsmap, (size_t)len, fds, 1);
                            for(int r=0;r<3 && g_run;r++){
                                sleep_ms(200);
                                uint64_t q = atomic_fetch_add(&hdr->seq, 1)+1;
                                int l2 = snprintf(jsmap, sizeof jsmap,
                                    "{\"type\":\"publish\",\"feed\":\"dummy.foo\",\"subtype\":\"shm_ready\",\"seq\":%llu,\"bytes\":%u}",
                                    (unsigned long long)q, hdr->used);
                                send_frame_json(fd, jsmap, (size_t)l2);
                            }
                            munmap(map, sizeof(struct shm_hdr)+cap);
                        }
                        close(sfd);
                    }
                } else {
                    handle_command(fd, data);
                }
                }
            }
        }
    }

    close(fd);
    return NULL;
}

const char* plugin_name(void){ return "dummy"; }
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){ if(!ctx || ctx->abi!=PLUGIN_ABI) return false; static const char *cons[] = {"dummy.config.in", NULL}; static const char *prod[] = {"dummy.config.out","dummy.foo", NULL}; g_sock = ctx->sock_path; if(out){ out->name = plugin_name(); out->version = "0.1.0"; out->consumes = cons; out->produces = prod; } return true; }
bool plugin_start(void){ return pthread_create(&g_thr, NULL, run, NULL)==0; }
void plugin_stop(void){ g_run = 0; pthread_join(g_thr, NULL); }
