// dummy.c — unified control-plane version (start/stop + subscribe/unsubscribe)
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"          // <— shared helpers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
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

/* --- addon state --- */
static const char *g_sock = NULL;
static pthread_t g_thr;
static _Atomic int g_run = 0;
static ph_ctrl_t g_ctrl;      // control-plane context

static void publish_utf8(int fd, const char *feed, const char *msg) {
    // tiny JSON string escaper for quotes and backslashes
    char esc[POC_MAX_JSON/2];
    size_t bi = 0;
    for (const char *p = msg; *p && bi + 2 < sizeof esc; ++p) {
        if (*p == '"' || *p == '\\')
            esc[bi++] = '\\';
        esc[bi++] = *p;
    }
    esc[bi] = '\0';

    char js[POC_MAX_JSON];
    snprintf(js, sizeof js, "{\"txt\":\"%s\"}", esc);  // wrap object for ph_publish
    ph_publish(fd, feed, js);
}

/* command handler (invoked by ph_ctrl_dispatch) */
static void on_cmd(ph_ctrl_t *c, const char *line, void *user){
    (void)user;
    // trim
    while(*line==' '||*line=='\t') line++;

    if(strncmp(line,"help",4)==0){
        ph_reply(c, "{\"ok\":true,"
                      "\"help\":\"help|ping|foo [text]|subscribe <feed>|unsubscribe <feed>|shm-demo\"}");
        return;
    }

    if(strncmp(line,"ping",4)==0){
        ph_reply_ok(c, "pong");
        return;
    }

    if(strncmp(line,"subscribe ",10)==0){
        const char *feed = line+10; while(*feed==' '||*feed=='\t') feed++;
        if(*feed){ ph_subscribe(c->fd, feed); ph_reply_okf(c, "subscribed %s", feed); }
        else      ph_reply_err(c, "subscribe arg");
        return;
    }

    if(strncmp(line,"unsubscribe ",12)==0){
        const char *feed = line+12; while(*feed==' '||*feed=='\t') feed++;
        if(*feed){ ph_unsubscribe(c->fd, feed); ph_reply_okf(c, "unsubscribed %s", feed); }
        else      ph_reply_err(c, "unsubscribe arg");
        return;
    }

    if(strncmp(line,"foo",3)==0){
        const char *arg = line+3; while(*arg==' '||*arg=='\t') arg++;
        if(*arg==0) arg="bar";
        publish_utf8(c->fd, "dummy.foo", arg);
        ph_reply_okf(c, "foo => published \"%s\" to dummy.foo", arg);
        return;
    }

    if(strncmp(line,"shm-demo",8)==0){
        size_t cap = 1<<20;
        int sfd = create_shm_fd(sizeof(struct shm_hdr)+cap);
        if(sfd<0){ ph_reply_err(c, "memfd/shm_open failed"); return; }
        void *map = mmap(NULL, sizeof(struct shm_hdr)+cap, PROT_READ|PROT_WRITE, MAP_SHARED, sfd, 0);
        if(!map || map==MAP_FAILED){ close(sfd); ph_reply_err(c, "mmap failed"); return; }

        struct shm_hdr *hdr = (struct shm_hdr*)map;
        hdr->magic=0x50485348; hdr->version=1;
        atomic_store(&hdr->seq, 0); hdr->used=0; hdr->capacity=(uint32_t)cap;
        for(size_t i=0;i<cap;i++) hdr->data[i]=(uint8_t)(i&0xFF);
        hdr->used = (uint32_t)cap;

        char jsmap[POC_MAX_JSON];
        int len = snprintf(jsmap, sizeof jsmap,
            "{\"type\":\"publish\",\"feed\":\"dummy.foo\",\"subtype\":\"shm_map\",\"size\":%u,\"desc\":\"dummy 1MiB buffer\",\"mode\":\"rw\"}",
            hdr->capacity);
        int fds[1]={sfd};
        send_frame_json_with_fds(c->fd, jsmap, (size_t)len, fds, 1);

        for(int r=0;r<3 && atomic_load(&g_run); r++){
            sleep_ms(200);
            uint64_t q = atomic_fetch_add(&hdr->seq, 1)+1;
            int l2 = snprintf(jsmap, sizeof jsmap,
                "{\"type\":\"publish\",\"feed\":\"dummy.foo\",\"subtype\":\"shm_ready\",\"seq\":%llu,\"bytes\":%u}",
                (unsigned long long)q, hdr->used);
            send_frame_json(c->fd, jsmap, (size_t)l2);
        }
        munmap(map, sizeof(struct shm_hdr)+cap);
        close(sfd);
        ph_reply_ok(c, "shm demo sent");
        return;
    }

    ph_reply_err(c, "unknown");
}

/* worker thread */
static void *run(void *arg){
    (void)arg;
    int fd = -1;
    for(int i=0;i<50;i++){ fd = uds_connect(g_sock ? g_sock : PH_SOCK_PATH); if(fd>=0) break; sleep_ms(100); }
    if(fd<0) return NULL;

    ph_ctrl_init(&g_ctrl, fd, "dummy");
    ph_ctrl_advertise(&g_ctrl);       // creates dummy.config.{in,out} and subscribes to .in
    ph_create_feed(fd, "dummy.foo");  // data feed produced by this addon

    atomic_store(&g_run, 1);
    char js[POC_MAX_JSON];

    while(atomic_load(&g_run)){
        int got = recv_frame_json(fd, js, sizeof js, 250);
        if(got<=0) continue;

        // 1) Let the shared dispatcher consume "command" frames to dummy.config.in
        if (ph_ctrl_dispatch(&g_ctrl, js, (size_t)got, on_cmd, NULL))
            continue;

        // 2) (Optional) handle other frames here if the dummy ever subscribes to data feeds
        //    e.g., parse {"type":"publish","feed":"something", "data":...}
    }

    close(fd);
    return NULL;
}

/* --- plugin ABI --- */
const char* plugin_name(void){ return "dummy"; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    if(!ctx || ctx->abi!=PLUGIN_ABI) return false;
    static const char *cons[] = { "dummy.config.in", NULL };
    static const char *prod[] = { "dummy.config.out","dummy.foo", NULL };
    g_sock = ctx->sock_path;
    if(out){
        out->name = plugin_name();
        out->version = "0.2.0";
        out->consumes = cons;
        out->produces = prod;
    }
    return true;
}

bool plugin_start(void){
    return pthread_create(&g_thr, NULL, run, NULL)==0;
}

void plugin_stop(void){
    atomic_store(&g_run, 0);
    pthread_join(g_thr, NULL);
}
