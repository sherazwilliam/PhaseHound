#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <stdatomic.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Version.h>

#define PLUGIN_NAME "soapy"
#define FEED_CFG_IN   "soapy.config.in"
#define FEED_CFG_OUT  "soapy.config.out"
#define FEED_IQ_INFO  "soapy.IQ-info"

#define MAGIC_PHIQ 0x51494850u /* 'P''H''I''Q' */
#define PHIQ_VERSION 1u

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

typedef enum {
    PHIQ_FMT_CF32 = 1, // complex float32
    PHIQ_FMT_CS16 = 2  // complex int16 (not used yet)
} phiq_fmt_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    _Atomic uint64_t seq;
    _Atomic uint64_t wpos;
    _Atomic uint64_t rpos;
    uint32_t capacity;
    uint32_t used;
    uint32_t bytes_per_samp;
    uint32_t channels;
    double sample_rate;
    double center_freq;
    uint32_t fmt;
    uint8_t reserved[64];
    uint8_t data[];
} phiq_hdr_t;

static const char *g_sock_path = NULL;
static pthread_t g_thr_io, g_thr_ctrl;
static volatile int g_run = 0;

typedef struct {
    SoapySDRDevice *dev;
    SoapySDRStream *rx;
    double sr;
    double cf;
    double bw;
    int chan;
    int active;
} soapy_state_t;

static soapy_state_t G = {0};

static int g_shm_fd = -1;
static phiq_hdr_t *g_hdr = NULL;
static size_t g_map_bytes = 0;

static int try_memfd_create(const char *name, unsigned flags){
#if defined(__linux__) && defined(SYS_memfd_create)
    int fd = (int)syscall(SYS_memfd_create, name, flags);
    return fd;
#else
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

static int create_shm_fd(size_t bytes){
    int memfd = try_memfd_create("ph-iq", MFD_CLOEXEC);
    if(memfd >= 0){
        if(ftruncate(memfd, (off_t)bytes) != 0){
            int e = errno; close(memfd); errno=e; return -1;
        }
        return memfd;
    }
    char name[64]; snprintf(name, sizeof name, "/ph-iq-%d", getpid());
    int shmfd = shm_open(name, O_CREAT|O_RDWR, 0600);
    if(shmfd < 0) return -1;
    shm_unlink(name);
    if(ftruncate(shmfd, (off_t)bytes)!=0){ int e=errno; close(shmfd); errno=e; return -1; }
    return shmfd;
}

static void *map_shm(size_t bytes){
    void *p = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if(p==MAP_FAILED) return NULL;
    return p;
}

static void unmap_shm(void){
    if(g_hdr){
        munmap(g_hdr, g_map_bytes);
        g_hdr=NULL;
    }
    if(g_shm_fd>=0){
        close(g_shm_fd);
        g_shm_fd=-1;
    }
    g_map_bytes=0;
}

static void publish_utf8(int fd, const char *feed, const char *msg){
    char js[POC_MAX_JSON];
    char esc[POC_MAX_JSON/2];
    size_t bi=0;
    for(const char *p=msg; *p && bi+2<sizeof esc; ++p){
        if(*p=='"' || *p=='\\') esc[bi++]='\\';
        esc[bi++]=*p;
    }
    esc[bi]=0;
    snprintf(js, sizeof js, "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"%s\",\"encoding\":\"utf8\"}", feed, esc);
    send_frame_json(fd, js, strlen(js));
}

static void create_feed(int fd, const char *feed){
    char js[256];
    snprintf(js, sizeof js, "{\"type\":\"create_feed\",\"feed\":\"%s\"}", feed);
    send_frame_json(fd, js, strlen(js));
}

static int send_iq_info_with_fd(int fd){
    if(g_shm_fd<0 || !g_hdr) return -1;
    char js[512];
    int len = snprintf(js, sizeof js,
        "{"
        "\"type\":\"publish\","
        "\"feed\":\"%s\","
        "\"encoding\":\"utf8\","
        "\"data\":\"{"
            "\\\"fmt\\\":%u,"
            "\\\"bytes_per_samp\\\":%u,"
            "\\\"channels\\\":%u,"
            "\\\"sample_rate\\\":%.6f,"
            "\\\"center_freq\\\":%.6f,"
            "\\\"capacity\\\":%u"
        "}\""
        "}",
        FEED_IQ_INFO,
        g_hdr->fmt, g_hdr->bytes_per_samp, g_hdr->channels,
        g_hdr->sample_rate, g_hdr->center_freq, g_hdr->capacity
    );
    int fds[1] = { g_shm_fd };
    return send_frame_json_with_fds(fd, js, (size_t)len, fds, 1);
}

static void soapy_close(void){
    if(G.rx){
        SoapySDRDevice_deactivateStream(G.dev, G.rx, 0, 0);
        SoapySDRDevice_closeStream(G.dev, G.rx);
        G.rx=NULL;
    }
    if(G.dev){
        SoapySDRDevice_unmake(G.dev);
        G.dev=NULL;
    }
    G.active=0;
}

static int soapy_list(char *out, size_t outcap){
    size_t count = 0;
    SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &count);
    size_t p = 0;
    p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "found=%zu\n", count);
    for(size_t i=0;i<count;i++){
        const SoapySDRKwargs *k = &results[i];
        p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "[%zu] ", i);
        for(size_t j=0;j<k->size;j++){
            p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "%s=%s ", k->keys[j], k->vals[j]);
        }
        p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "\n");
    }
    SoapySDRKwargsList_clear(results, count);
    return 0;
}

static int soapy_open_idx(int idx){
    size_t count = 0;
    SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &count);
    if(idx < 0 || (size_t)idx >= count){
        SoapySDRKwargsList_clear(results, count);
        return -1;
    }
    G.dev = SoapySDRDevice_make(&results[idx]);
    SoapySDRKwargsList_clear(results, count);
    if(!G.dev) return -1;
    G.chan = 0;
    G.sr = 2.4e6;
    G.cf = 100e6;
    G.bw = 0.0;
    return 0;
}

static int soapy_apply_params(void){
    if(!G.dev) return -1;
    if(G.cf>0) SoapySDRDevice_setFrequency(G.dev, SOAPY_SDR_RX, G.chan, G.cf, NULL);
    if(G.sr>0) SoapySDRDevice_setSampleRate(G.dev, SOAPY_SDR_RX, G.chan, G.sr);
    if(G.bw>0) SoapySDRDevice_setBandwidth(G.dev, SOAPY_SDR_RX, G.chan, G.bw);
    return 0;
}

static int soapy_start_stream(void){
    if(!G.dev) return -1;
    if(!g_hdr){
        size_t capacity = 8u<<20; // 8 MiB
        size_t total = sizeof(phiq_hdr_t) + capacity;
        g_shm_fd = create_shm_fd(total);
        if(g_shm_fd < 0) return -1;
        g_map_bytes = total;
        g_hdr = (phiq_hdr_t*)map_shm(total);
        if(!g_hdr) { unmap_shm(); return -1; }
        memset(g_hdr, 0, sizeof(phiq_hdr_t));
        g_hdr->magic = MAGIC_PHIQ;
        g_hdr->version = PHIQ_VERSION;
        g_hdr->bytes_per_samp = 8u; // CF32
        g_hdr->channels = 1;
        g_hdr->fmt = PHIQ_FMT_CF32;
        g_hdr->capacity = (uint32_t)capacity;
        g_hdr->sample_rate = G.sr;
        g_hdr->center_freq = G.cf;
    }
    size_t chan = (size_t)G.chan;
    SoapySDRStream *rx = SoapySDRDevice_setupStream(G.dev, SOAPY_SDR_RX, SOAPY_SDR_CF32, &chan, 1, NULL);
    if(!rx) return -1;
    G.rx = rx;
    if(SoapySDRDevice_activateStream(G.dev, G.rx, 0, 0, 0) != 0){
        SoapySDRDevice_closeStream(G.dev, G.rx);
        G.rx=NULL;
        return -1;
    }
    G.active = 1;
    return 0;
}

static void soapy_stop_stream(void){
    if(G.rx){
        SoapySDRDevice_deactivateStream(G.dev, G.rx, 0, 0);
        SoapySDRDevice_closeStream(G.dev, G.rx);
        G.rx=NULL;
    }
    G.active=0;
}

static void *io_thread(void *arg){
    (void)arg;
    int fd = uds_connect(g_sock_path);
    if(fd < 0){ log_msg(LOG_ERROR, "["PLUGIN_NAME"] IO connect failed"); return NULL; }
    set_nonblock(fd);

    uint8_t tmpbuf[1<<16];
    void *buffs[1] = { tmpbuf };
    const size_t elems = sizeof(tmpbuf) / (sizeof(float)*2);
    while(g_run){
        if(G.active && G.rx){
            int flags=0;
            long long ts=0;
            int got = SoapySDRDevice_readStream(G.dev, G.rx, buffs, elems, &flags, &ts, 10000);
            if(got > 0 && g_hdr){
                size_t bytes = (size_t)got * g_hdr->bytes_per_samp;
                uint64_t w = g_hdr->wpos;
                uint8_t *dst = g_hdr->data;
                uint32_t cap = g_hdr->capacity;
                size_t first = bytes;
                uint64_t mod = (w % cap);
                if(mod + bytes > cap){
                    first = cap - mod;
                }
                memcpy(dst + mod, tmpbuf, first);
                if(first < bytes){
                    memcpy(dst, tmpbuf + first, bytes - first);
                }
                atomic_store(&g_hdr->wpos, (w + bytes) % cap);
                if(g_hdr->used + bytes > cap) atomic_store(&g_hdr->used, cap); else g_hdr->used += (uint32_t)bytes;
                atomic_store(&g_hdr->seq, atomic_load(&g_hdr->seq)+1);
            }
        } else {
            usleep(1000*10);
        }
    }
    close(fd);
    return NULL;
}

static void handle_cmd(int fd, const char *cmd){
    if(strncmp(cmd,"list",4)==0){
        char buf[4096]={0};
        soapy_list(buf, sizeof buf);
        publish_utf8(fd, FEED_CFG_OUT, buf);
        return;
    }
    if(strncmp(cmd,"select ",7)==0){
        int idx = atoi(cmd+7);
        if(soapy_open_idx(idx)==0){
            soapy_apply_params();
            publish_utf8(fd, FEED_CFG_OUT, "selected");
        } else {
            publish_utf8(fd, FEED_CFG_OUT, "select failed");
        }
        return;
    }
    if(strncmp(cmd,"set ",4)==0){
        double sr=G.sr, cf=G.cf, bw=G.bw;
        const char *p = cmd+4;
        while(*p){
            while(*p==' ') p++;
            if(strncmp(p,"sr=",3)==0){ sr = strtod(p+3,(char**)&p); }
            else if(strncmp(p,"cf=",3)==0){ cf = strtod(p+3,(char**)&p); }
            else if(strncmp(p,"bw=",3)==0){ bw = strtod(p+3,(char**)&p); }
            else { while(*p && *p!=' ') p++; }
        }
        G.sr=sr; G.cf=cf; G.bw=bw;
        soapy_apply_params();
        if(g_hdr){ g_hdr->sample_rate=G.sr; g_hdr->center_freq=G.cf; }
        publish_utf8(fd, FEED_CFG_OUT, "ok");
        return;
    }
    if(strcmp(cmd,"start")==0){
        if(soapy_start_stream()==0){
            (void)send_iq_info_with_fd(fd);
            publish_utf8(fd, FEED_CFG_OUT, "started");
        } else publish_utf8(fd, FEED_CFG_OUT, "start failed");
        return;
    }
    if(strcmp(cmd,"stop")==0){
        soapy_stop_stream();
        publish_utf8(fd, FEED_CFG_OUT, "stopped");
        return;
    }
    publish_utf8(fd, FEED_CFG_OUT, "unknown");
}

static void *ctrl_thread(void *arg){
    (void)arg;
    int fd = uds_connect(g_sock_path);
    if(fd < 0){ log_msg(LOG_ERROR, "["PLUGIN_NAME"] ctrl connect failed"); return NULL; }
    create_feed(fd, FEED_CFG_OUT);
    create_feed(fd, FEED_IQ_INFO);
    char js[256];
    snprintf(js, sizeof js, "{\"type\":\"subscribe\",\"feed\":\"%s\"}", FEED_CFG_IN);
    send_frame_json(fd, js, strlen(js));

    g_run = 1;
    char rbuf[POC_MAX_JSON];
    while(g_run){
        int n = recv_frame_json(fd, rbuf, sizeof rbuf, 250);
        if(n <= 0) continue;
        char type[32]={0}, feed[128]={0};
        if(json_get_type(rbuf, type, sizeof type)!=0) continue;
        if(strcmp(type,"publish")==0){
            if(json_get_string(rbuf,"feed", feed, sizeof feed)==0 && strcmp(feed, FEED_CFG_IN)==0){
                char enc[32]={0};
                char data[POC_MAX_JSON]={0};
                if(json_get_string(rbuf,"encoding",enc,sizeof enc)!=0){ strcpy(enc,"utf8"); }
                if(json_get_string(rbuf,"data",data,sizeof data)==0){
                    if(strcmp(enc,"utf8")==0){
                        handle_cmd(fd, data);
                    } else if(strcmp(enc,"base64")==0){
                        size_t maxlen = b64_decoded_maxlen(strlen(data));
                        char *tmp = (char*)malloc(maxlen+1);
                        if(tmp){
                            size_t outlen=0;
                            if(b64_decode(data, strlen(data), (uint8_t*)tmp, &outlen)==0){
                                tmp[outlen]=0;
                                handle_cmd(fd, tmp);
                            }
                            free(tmp);
                        }
                    }
                }
            }
        }
    }
    close(fd);
    return NULL;
}

static const char *g_name = PLUGIN_NAME;

const char* plugin_name(void){ return g_name; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    if(!ctx) return false;
    g_sock_path = ctx->sock_path;
    static const char *cons[] = { FEED_CFG_IN, NULL };
    static const char *prod[] = { FEED_CFG_OUT, FEED_IQ_INFO, NULL };
    if(out){
        out->name = g_name;
        out->version = "0.1.2";
        out->consumes = cons;
        out->produces = prod;
    }
    return true;
}

bool plugin_start(void){
    g_run = 1;
    if(pthread_create(&g_thr_ctrl, NULL, ctrl_thread, NULL)!=0) return false;
    if(pthread_create(&g_thr_io, NULL, io_thread, NULL)!=0){ g_run=0; pthread_join(g_thr_ctrl,NULL); return false; }
    return true;
}

void plugin_stop(void){
    g_run = 0;
    pthread_join(g_thr_io, NULL);
    pthread_join(g_thr_ctrl, NULL);
    soapy_stop_stream();
    soapy_close();
    unmap_shm();
}