// soapy.c — SoapySDR IQ producer (unified control-plane ABI + absolute ring counters)
// Uses ctrlmsg helpers: ph_ctrl_init/adverise + ph_ctrl_dispatch; produces soapy.IQ-info (memfd)

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp
#include <time.h>

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Version.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"   // control-plane helpers
#include "ph_stream.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

#define PLUGIN_NAME "soapy"

/* feeds (control-plane helpers auto-create soapy.config.{in,out}) */
#define FEED_IQ_INFO  "soapy.IQ-info"

/* ---------- small compat for memfd/shm_open ---------- */
static int xmemfd_create(const char *name, unsigned int flags){
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    (void)name; (void)flags; errno=ENOSYS; return -1;
#endif
}
static int create_shm_fd(const char *tag, size_t bytes){
    int fd = xmemfd_create(tag, MFD_CLOEXEC);
    if(fd>=0){
        if(ftruncate(fd, (off_t)bytes)<0){ int e=errno; close(fd); errno=e; return -1; }
        return fd;
    }
    char name[64]; snprintf(name, sizeof name, "/%s-%d", tag, getpid());
    fd = shm_open(name, O_CREAT|O_RDWR, 0600);
    if(fd<0) return -1;
    shm_unlink(name);
    if(ftruncate(fd, (off_t)bytes)<0){ int e=errno; close(fd); errno=e; return -1; }
    return fd;
}
static void sleep_ms(int ms){ struct timespec ts={ ms/1000, (ms%1000)*1000000L }; nanosleep(&ts,NULL); }

/* ---------- addon state ---------- */
static const char *g_sock = NULL;
static pthread_t   g_thr;       // control-plane + bus
static pthread_t   g_rxthr;     // SDR RX thread
static _Atomic int g_run = 0;

static ph_ctrl_t   g_ctrl;      // control-plane ctx (provides .fd)

/* soapy device state */
typedef struct {
    SoapySDRDevice *dev;
    SoapySDRStream *rx;
    double sr, cf, bw;
    int    chan;
} soapy_t;
static soapy_t g_dev = {0};

static _Atomic int g_active = 0;   // start/stop gating

/* IQ shm map */
static int        g_memfd = -1;
static phiq_hdr_t *g_hdr  = NULL;
static size_t     g_map_bytes = 0;

/* ---------- helpers ---------- */
static void publish_txt(const char *feed, const char *txt){
    char esc[POC_MAX_JSON/2];
    size_t bi=0;
    for(const char *p=txt; *p && bi+2<sizeof esc; ++p){
        if(*p=='"'||*p=='\\') esc[bi++]='\\';
        esc[bi++]=*p;
    }
    esc[bi]=0;
    char js[POC_MAX_JSON];
    snprintf(js,sizeof js,"{\"txt\":\"%s\"}",esc);
    ph_publish(g_ctrl.fd, feed, js);
}

static void publish_iq_memfd(void){
    if(g_memfd<0 || !g_hdr) return;
    char js[POC_MAX_JSON];

    const char *enc = "unknown";
    if (g_hdr->fmt == PHIQ_FMT_CF32) enc = "cf32";
    else if (g_hdr->fmt == PHIQ_FMT_CS16) enc = "cs16";

    /* Normalized SHM meta: phasehound.iq-ring.v0 + stream hints */
    int n = snprintf(js, sizeof js,
        "{"
          "\"type\":\"publish\","
          "\"feed\":\"%s\","
          "\"subtype\":\"shm_map\","
          "\"proto\":\"" PH_PROTO_IQ_RING "\","
          "\"version\":\"0.1\","
          "\"size\":%u,"
          "\"mode\":\"r\","
          "\"kind\":\"iq\","
          "\"encoding\":\"%s\","
          "\"sample_rate\":%.0f,"
          "\"channels\":%u,"
          "\"center_freq\":%.0f,"
          "\"desc\":\"Soapy IQ ring (cf=%.3f MHz,sr=%.3f Msps)\""
        "}",
        FEED_IQ_INFO,
        g_hdr->capacity,
        enc,
        g_hdr->sample_rate,
        g_hdr->channels,
        g_hdr->center_freq,
        g_hdr->center_freq/1e6,
        g_hdr->sample_rate/1e6);
    int fds[1]={ g_memfd };
    send_frame_json_with_fds(g_ctrl.fd, js, (size_t)n, fds, 1);
}

/* ring create */
static int iq_ring_open(size_t capacity_bytes, double sr, double cf, phiq_fmt_t fmt){
    if(g_hdr){ munmap(g_hdr, g_map_bytes); g_hdr=NULL; }
    if(g_memfd>=0){ close(g_memfd); g_memfd=-1; }

    size_t total = sizeof(phiq_hdr_t) + capacity_bytes;
    int fd = create_shm_fd("ph-iq", total);
    if(fd<0) return -1;
    void *map = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(!map || map==MAP_FAILED){ int e=errno; close(fd); errno=e; return -1; }

    g_memfd = fd; g_hdr = (phiq_hdr_t*)map; g_map_bytes = total;
    memset(g_hdr, 0, sizeof(phiq_hdr_t));
    g_hdr->magic = PHIQ_MAGIC;
    g_hdr->version = PHIQ_VERSION;
    atomic_store(&g_hdr->seq,  0);
    atomic_store(&g_hdr->wpos, 0);
    atomic_store(&g_hdr->rpos, 0);
    g_hdr->capacity       = (uint32_t)capacity_bytes;
    g_hdr->fmt            = (uint32_t)fmt;
    g_hdr->bytes_per_samp = (fmt==PHIQ_FMT_CF32)? 8u : 4u;
    g_hdr->channels       = 1;
    g_hdr->sample_rate    = sr;
    g_hdr->center_freq    = cf;
    return 0;
}
static void iq_ring_close(void){
    if(g_hdr && g_hdr!=MAP_FAILED) munmap(g_hdr, g_map_bytes);
    if(g_memfd>=0) close(g_memfd);
    g_hdr=NULL; g_memfd=-1; g_map_bytes=0;
}

/* soapy open/config */
static int soapy_list(char *out, size_t outcap){
    size_t n=0; SoapySDRKwargs *res = SoapySDRDevice_enumerate(NULL,&n);
    size_t p=0; p+=(size_t)snprintf(out+p, outcap>p?outcap-p:0, "found=%zu\n", n);
    for(size_t i=0;i<n;i++){
        const SoapySDRKwargs *k=&res[i];
        p+=(size_t)snprintf(out+p,outcap>p?outcap-p:0,"[%zu] ",i);
        for(size_t j=0;j<k->size;j++)
            p+=(size_t)snprintf(out+p,outcap>p?outcap-p:0,"%s=%s ",k->keys[j],k->vals[j]);
        p+=(size_t)snprintf(out+p,outcap>p?outcap-p:0,"\n");
    }
    SoapySDRKwargsList_clear(res,n); return 0;
}
static int soapy_open_idx(int idx){
    size_t n=0; SoapySDRKwargs *res = SoapySDRDevice_enumerate(NULL,&n);
    if(idx<0 || (size_t)idx>=n){ SoapySDRKwargsList_clear(res,n); return -1; }
    g_dev.dev = SoapySDRDevice_make(&res[idx]);
    SoapySDRKwargsList_clear(res,n);
    if(!g_dev.dev) return -1;
    g_dev.chan=0; g_dev.sr=2.4e6; g_dev.cf=100e6; g_dev.bw=0.0;
    return 0;
}
static int soapy_apply_params(void){
    if(!g_dev.dev) return -1;
    if(g_dev.cf>0) SoapySDRDevice_setFrequency(g_dev.dev, SOAPY_SDR_RX, g_dev.chan, g_dev.cf, NULL);
    if(g_dev.sr>0) SoapySDRDevice_setSampleRate(g_dev.dev, SOAPY_SDR_RX, g_dev.chan, g_dev.sr);
    if(g_dev.bw>0) SoapySDRDevice_setBandwidth(g_dev.dev, SOAPY_SDR_RX, g_dev.chan, g_dev.bw);
    return 0;
}
static int soapy_start(phiq_fmt_t fmt){
    if(!g_dev.dev) return -1;
    if(!g_hdr){
        /* 8 MiB ring is ~0.33 s at 2.4 Msps CF32; tune as needed */
        size_t cap = 8u<<20;
        if(iq_ring_open(cap, g_dev.sr, g_dev.cf, fmt)!=0) return -1;
    }
    /* choose Soapy stream format */
    const char *soap_fmt = (fmt==PHIQ_FMT_CF32)? SOAPY_SDR_CF32 : SOAPY_SDR_CS16;
    size_t ch = (size_t)g_dev.chan;
    g_dev.rx = SoapySDRDevice_setupStream(g_dev.dev, SOAPY_SDR_RX, soap_fmt, &ch, 1, NULL);
    if(!g_dev.rx) return -1;
    if(SoapySDRDevice_activateStream(g_dev.dev, g_dev.rx, 0, 0, 0)!=0){
        SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx); g_dev.rx=NULL; return -1;
    }
    atomic_store(&g_active, 1);
    publish_iq_memfd(); /* publish memfd once started */
    return 0;
}
static void soapy_stop(void){
    atomic_store(&g_active, 0);
    if(g_dev.rx){
        SoapySDRDevice_deactivateStream(g_dev.dev, g_dev.rx, 0, 0);
        SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx);
        g_dev.rx=NULL;
    }
}

/* ---------- RX thread: read Soapy, write ring (absolute counters + overrun policy) ---------- */
static void *rx_thread(void *arg){
    (void)arg;
    uint8_t tmp[1<<16];
    void *buffs[1]={ tmp };

    while(atomic_load(&g_run)){
        if(!atomic_load(&g_active) || !g_dev.rx || !g_hdr){ sleep_ms(10); continue; }

        int flags=0; long long ts=0;
        int elems;
        if(g_hdr->fmt == PHIQ_FMT_CF32){
            elems = (int)( sizeof(tmp) / (2*sizeof(float)) );
        }else{
            elems = (int)( sizeof(tmp) / (2*sizeof(int16_t)) );
        }
        int got = SoapySDRDevice_readStream(g_dev.dev, g_dev.rx, buffs, elems, &flags, &ts, 10000);
        if(got <= 0) continue;

        const size_t bytes = (size_t)got * g_hdr->bytes_per_samp;
        const uint32_t cap = g_hdr->capacity;

        uint64_t w = atomic_load(&g_hdr->wpos);
        uint64_t r = atomic_load(&g_hdr->rpos);

        uint64_t prospective = w + (uint64_t)bytes;
        if (prospective - r > cap){
            uint64_t new_r = prospective - cap;
            atomic_store(&g_hdr->rpos, new_r);
            r = new_r;
        }

        size_t mod = (size_t)(w % cap);
        size_t first = bytes;
        if(mod + bytes > cap) first = cap - mod;

        memcpy(g_hdr->data + mod, tmp, first);
        if(first < bytes) memcpy(g_hdr->data, tmp + first, bytes - first);

        atomic_store(&g_hdr->wpos, w + bytes);
        uint64_t used = (w + bytes) - r; if(used > cap) used = cap; g_hdr->used = (uint32_t)used;
        atomic_fetch_add(&g_hdr->seq, 1);

        /* keep meta up to date */
        g_hdr->sample_rate = g_dev.sr;
        g_hdr->center_freq = g_dev.cf;
    }
    return NULL;
}

/* ---------- command handler (via ctrl dispatcher) ---------- */
static _Atomic phiq_fmt_t g_fmt = PHIQ_FMT_CF32;

static void on_cmd(ph_ctrl_t *c, const char *line, void *user){
    (void)user;
    while(*line==' '||*line=='\t') line++;

    if(strncmp(line,"help",4)==0){
        ph_reply(c, "{\"ok\":true,"
                      "\"help\":\"help|list|select <idx>|set sr=<Hz> cf=<Hz> [bw=<Hz>]|"
                               "fmt <cf32|cs16>|start|stop|open|status|"
                               "subscribe <feed>|unsubscribe <feed>\"}");
        return;
    }

    if(strncmp(line,"list",4)==0){
        char buf[4096]={0}; soapy_list(buf,sizeof buf);
        publish_txt("soapy.config.out", buf);
        ph_reply_ok(c,"listed");
        return;
    }

    if(strncmp(line,"select ",7)==0){
        int idx = atoi(line+7);
        if(soapy_open_idx(idx)==0){ soapy_apply_params(); ph_reply_ok(c,"selected"); }
        else ph_reply_err(c,"select failed");
        return;
    }

    if(strncmp(line,"set ",4)==0){
        double sr=g_dev.sr, cf=g_dev.cf, bw=g_dev.bw;
        const char *p=line+4;
        while(*p){
            while(*p==' ') p++;
            if(strncmp(p,"sr=",3)==0){ sr = strtod(p+3,(char**)&p); }
            else if(strncmp(p,"cf=",3)==0){ cf = strtod(p+3,(char**)&p); }
            else if(strncmp(p,"bw=",3)==0){ bw = strtod(p+3,(char**)&p); }
            else { while(*p && *p!=' ') p++; }
        }
        g_dev.sr=sr; g_dev.cf=cf; g_dev.bw=bw;
        soapy_apply_params();
        if(g_hdr){ g_hdr->sample_rate=g_dev.sr; g_hdr->center_freq=g_dev.cf; }
        ph_reply_okf(c,"set sr=%.0f cf=%.0f bw=%.0f", sr, cf, bw);
        return;
    }

    if(strncmp(line,"fmt ",4)==0){
        const char *p=line+4; while(*p==' '||*p=='\t') p++;
        if(strncasecmp(p,"cf32",4)==0){ g_fmt = PHIQ_FMT_CF32; ph_reply_ok(c,"fmt=CF32"); }
        else if(strncasecmp(p,"cs16",4)==0){ g_fmt = PHIQ_FMT_CS16; ph_reply_ok(c,"fmt=CS16"); }
        else ph_reply_err(c,"fmt arg");
        return;
    }

    if(strncmp(line,"start",5)==0){
        if(soapy_start(atomic_load(&g_fmt))==0){ publish_iq_memfd(); ph_reply_ok(c,"started"); }
        else ph_reply_err(c,"start failed");
        return;
    }

    if(strncmp(line,"stop",4)==0){
        soapy_stop(); ph_reply_ok(c,"stopped"); return;
    }

    if(strncmp(line,"open",4)==0){
        publish_iq_memfd(); ph_reply_ok(c,"republished"); return;
    }

    if(strncmp(line,"status",6)==0){
        char js[384];
        snprintf(js,sizeof js,
            "{\"ok\":true,\"sr\":%.1f,\"cf\":%.1f,\"bw\":%.1f,"
             "\"active\":%d,\"fmt\":%u,\"bps\":%u}",
            g_dev.sr,g_dev.cf,g_dev.bw,(int)atomic_load(&g_active),
            (unsigned)atomic_load(&g_fmt),
            g_hdr?g_hdr->bytes_per_samp:0u);
        ph_reply(c, js);
        return;
    }

    if(strncmp(line,"subscribe ",10)==0){
        const char *feed=line+10; while(*feed==' '||*feed=='\t') feed++;
        if(*feed){ ph_subscribe(c->fd, feed); ph_reply_okf(c,"subscribed %s",feed); }
        else ph_reply_err(c,"subscribe arg");
        return;
    }
    if(strncmp(line,"unsubscribe ",12)==0){
        const char *feed=line+12; while(*feed==' '||*feed=='\t') feed++;
        if(*feed){ ph_unsubscribe(c->fd, feed); ph_reply_okf(c,"unsubscribed %s",feed); }
        else ph_reply_err(c,"unsubscribe arg");
        return;
    }

    ph_reply_err(c,"unknown");
}

/* ---------- worker (single thread: ctrl + bus) ---------- */
static void *run(void *arg){
    (void)arg;
    int fd=-1;
    for(int i=0;i<50;i++){ fd = uds_connect(g_sock?g_sock:PH_SOCK_PATH); if(fd>=0) break; sleep_ms(100); }
    if(fd<0) return NULL;

    ph_ctrl_init(&g_ctrl, fd, "soapy");     // creates soapy.config.{in,out} and subscribes to .in
    ph_ctrl_advertise(&g_ctrl);
    ph_create_feed(fd, FEED_IQ_INFO);       // data feed produced by this addon

    /* launch RX thread */
    atomic_store(&g_run, 1);
    if(pthread_create(&g_rxthr, NULL, rx_thread, NULL)!=0){
        atomic_store(&g_run,0);
        close(fd);
        return NULL;
    }

    char js[POC_MAX_JSON];
    while(atomic_load(&g_run)){
        int got = recv_frame_json(fd, js, sizeof js, 100);
        if(got<=0) continue;

        /* delegate commands to on_cmd via shared dispatcher */
        if(ph_ctrl_dispatch(&g_ctrl, js, (size_t)got, on_cmd, NULL)){
            continue;
        }

        /* (Optional) could handle other subscribed data feeds here */
    }

    close(fd);
    return NULL;
}

/* ---------- plugin ABI ---------- */
const char* plugin_name(void){ return PLUGIN_NAME; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    PH_ENSURE_ABI(ctx);
    g_sock = ctx->sock_path;
    static const char *CONS[] = { "soapy.config.in", NULL };
    static const char *PROD[] = { "soapy.config.out", FEED_IQ_INFO, NULL };
    if(out){
        out->caps_size = sizeof(*out);
        out->name = plugin_name();
        out->version = "0.4.0";   // fix: proper create_shm_fd(tag, bytes) + strings.h
        out->consumes = CONS;
        out->produces = PROD;
        out->feat_bits = PH_FEAT_IQ;
    }
    return true;
}

bool plugin_start(void){
    return pthread_create(&g_thr, NULL, run, NULL)==0;
}

void plugin_stop(void){
    atomic_store(&g_run, 0);
    pthread_join(g_thr, NULL);

    /* stop RX + free resources */
    soapy_stop();
    if(g_dev.dev){
        if(g_dev.rx){ SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx); g_dev.rx=NULL; }
        SoapySDRDevice_unmake(g_dev.dev); g_dev.dev=NULL;
    }
    iq_ring_close();
}

