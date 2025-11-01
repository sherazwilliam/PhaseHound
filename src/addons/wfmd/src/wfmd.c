// wfmd.c — Wideband FM mono demodulator addon
#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

typedef struct {
    uint32_t magic, version;
    _Atomic uint64_t seq, wpos, rpos;
    uint32_t capacity, used;
    uint32_t bytes_per_samp;
    uint32_t channels;
    double   sample_rate;
    uint32_t fmt;
    uint8_t  reserved[64];
    uint8_t  data[];
} phau_hdr_t;

// --- IQ ring header (must match soapy addon's definition) ---
typedef enum {
    PHIQ_FMT_CF32 = 1,
    PHIQ_FMT_CS16 = 2
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
    double   sample_rate;
    double   center_freq;
    uint32_t fmt;
    uint8_t  reserved[64];
    uint8_t  data[];
} phiq_hdr_t;

enum { PHAU_FMT_F32 = 1 };
enum { PHAU_MAGIC = 0x50484155, PHAU_VER = 0x00010000 };

static atomic_bool g_run = false;
static char        g_sock_path[256] = {0};
static pthread_t   g_th_cmd, g_th_iq;

// Runtime toggles
static _Atomic bool g_swapiq = false;   // swap I/Q
static _Atomic bool g_flipq  = false;   // flip sign of Q
static _Atomic bool g_neg    = false;   // negate discriminator
static _Atomic bool g_deemph = true;    // apply deemphasis
static _Atomic int  g_taps1  = 101;     // first-stage LP taps (odd)
static _Atomic int  g_debug  = 0;       // periodic diagnostics

static float       g_gain = 4.0f;
static double      g_fs = 2400000.0;

static const char *CONS[] = { "wfmd.config.in", NULL };
static const char *PROD[] = { "wfmd.config.out", "wfmd.audio-info", NULL };

static void publish_utf8(int fd, const char *feed, const char *msg){
    char js[POC_MAX_JSON];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"%s\",\"encoding\":\"utf8\"}",
        feed, msg);
    if(n > 0) send_frame_json(fd, js, (size_t)n);
}
static void create_feed(int fd, const char *feed){
    char js[256];
    int n = snprintf(js, sizeof js, "{\"type\":\"create_feed\",\"feed\":\"%s\"}", feed);
    if(n > 0) send_frame_json(fd, js, (size_t)n);
}
static void subscribe_feed_msg(int fd, const char *feed){
    char js[256];
    int n = snprintf(js, sizeof js, "{\"type\":\"subscribe\",\"feed\":\"%s\"}", feed);
    if(n > 0) send_frame_json(fd, js, (size_t)n);
}
static void reply(int fd, const char *fmt, ...){
    char buf[POC_MAX_JSON];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    publish_utf8(fd, "wfmd.config.out", buf);
}


// --- IQ ring consumer ---
typedef struct {
    int         memfd;
    phiq_hdr_t *hdr;
    size_t      map_bytes;
} iq_ring_t;

static iq_ring_t g_iq = { .memfd=-1, .hdr=NULL, .map_bytes=0 };

static void iq_ring_close(iq_ring_t *r){
    if(!r) return;
    if(r->hdr && r->hdr != MAP_FAILED) munmap(r->hdr, r->map_bytes);
    r->hdr=NULL;
    if(r->memfd>=0) close(r->memfd);
    r->memfd=-1;
    r->map_bytes=0;
}
typedef struct {
    int         memfd;
    phau_hdr_t *hdr;
    size_t      map_bytes;
} ring_t;

static void ring_close(ring_t *r){
    if(!r) return;
    if(r->hdr && r->hdr != MAP_FAILED) munmap(r->hdr, r->map_bytes);
    if(r->memfd >= 0) close(r->memfd);
    r->hdr = NULL; r->map_bytes = 0; r->memfd = -1;
}

static int ring_open(ring_t *r, size_t audio_capacity_bytes, double fs){
    memset(r, 0, sizeof *r);
    r->memfd = memfd_create("wfmd-audio", MFD_CLOEXEC);
    if(r->memfd < 0) return -1;

    size_t need = sizeof(phau_hdr_t) + audio_capacity_bytes;
    if(ftruncate(r->memfd, (off_t)need) != 0){ close(r->memfd); r->memfd = -1; return -1; }

    r->map_bytes = need;
    r->hdr = (phau_hdr_t*)mmap(NULL, r->map_bytes, PROT_READ|PROT_WRITE, MAP_SHARED, r->memfd, 0);
    if(r->hdr == MAP_FAILED){ close(r->memfd); r->memfd = -1; r->hdr=NULL; return -1; }

    memset(r->hdr, 0, r->map_bytes);
    r->hdr->magic          = PHAU_MAGIC;
    r->hdr->version        = PHAU_VER;
    atomic_store(&r->hdr->seq, 0);
    atomic_store(&r->hdr->wpos, 0);
    atomic_store(&r->hdr->rpos, 0);
    r->hdr->capacity       = (uint32_t)audio_capacity_bytes;
    r->hdr->bytes_per_samp = 4;
    r->hdr->channels       = 1;
    r->hdr->sample_rate    = fs;
    r->hdr->fmt            = PHAU_FMT_F32;
    return 0;
}

static void ring_push_f32(ring_t *r, const float *x, size_t n_frames){
    if(!r->hdr) return;
    const size_t bps = r->hdr->bytes_per_samp;
    const size_t ch  = r->hdr->channels;
    const size_t frame_bytes = bps * ch;
    const size_t bytes = n_frames * frame_bytes;

    uint64_t w = atomic_load(&r->hdr->wpos);
    uint64_t rpos = atomic_load(&r->hdr->rpos);
    size_t cap = r->hdr->capacity;
    if(bytes > cap){ return; }
    if((w - rpos) + bytes > cap){
        uint64_t new_r = (w + bytes) - cap;
        atomic_store(&r->hdr->rpos, new_r);
        rpos = new_r;
    }

    size_t wp = (size_t)(w % cap);
    size_t first = bytes;
    if(wp + bytes > cap) first = cap - wp;

    memcpy(r->hdr->data + wp, x, first);
    if(bytes > first) memcpy(r->hdr->data, ((const uint8_t*)x)+first, bytes - first);

    atomic_store(&r->hdr->wpos, w + bytes);
    atomic_store(&r->hdr->seq, atomic_load(&r->hdr->seq)+1);
}
// Re-publish audio memfd on demand (so late subscribers can map it)



typedef struct {
    float *taps;
    int    ntaps;
    float *zb;
    int    zpos;
    int    R;
} firdec_t;

static void firdec_free(firdec_t *d){
    if(!d) return;
    free(d->taps); d->taps=NULL;
    free(d->zb);   d->zb=NULL;
    d->ntaps=0; d->zpos=0; d->R=1;
}

static int firdec_init(firdec_t *d, int ntaps, float fs_in, float fc, int R){
    memset(d,0,sizeof(*d));
    if(ntaps < 31) ntaps = 31;
    d->ntaps = ntaps | 1;
    d->R = (R<1)?1:R;
    d->taps = (float*)malloc((size_t)d->ntaps*sizeof(float));
    d->zb   = (float*)calloc((size_t)d->ntaps, sizeof(float));
    if(!d->taps || !d->zb) return -1;
    int M = d->ntaps, m2 = (M-1)/2;
    double fn = (double)fc / (double)fs_in; if(fn > 0.499) fn = 0.499;
    double sum=0.0;
    for(int n=0;n<M;n++){
        int k = n - m2;
        double w = 0.54 - 0.46*cos(2.0*M_PI*n/(M-1));
        double x = (k==0) ? (2.0*fn) : (sin(2.0*M_PI*fn*k)/(M_PI*k));
        double h = w * x;
        d->taps[n] = (float)h;
        sum += h;
    }
    for(int n=0;n<M;n++) d->taps[n] = (float)(d->taps[n]/sum);
    d->zpos = 0;
    return 0;
}

static size_t firdec_push(firdec_t *d, const float *in, size_t ns, float *out, size_t out_cap){
    size_t out_n = 0;
    for(size_t i=0;i<ns;i++){
        d->zb[d->zpos] = in[i];
        d->zpos = (d->zpos + 1) % d->ntaps;
        if(((i+1) % d->R) == 0){
            float acc = 0.0f;
            int idx = d->zpos;
            for(int t=0;t<d->ntaps;t++){
                idx--; if(idx<0) idx = d->ntaps-1;
                acc += d->taps[t] * d->zb[idx];
            }
            if(out_n < out_cap) out[out_n++] = acc;
        }
    }
    return out_n;
}

static ring_t g_ring;
static void wfmd_publish_memfd(int fd){
    if(!g_ring.hdr) return;
    int fds[1] = { g_ring.memfd };
    char js[256];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"info\",\"encoding\":\"utf8\"}",
        "wfmd.audio-info");
    send_frame_json_with_fds(fd, js, (size_t)n, fds, 1);
}
static void push_audio(const float *y, size_t n){
    if(n) ring_push_f32(&g_ring, y, n);
}

static firdec_t st1_state, st2_state;
static int st_inited = 0;
static double st_last_fs = 0.0;
static float dc_x1 = 0.0f, dc_y1 = 0.0f;
static void demod_block(const float *iq, size_t nsamp, double fs){
    if(nsamp < 2) return;

    static float ip=0.0f, qp=0.0f;
    float *dphi = (float*)malloc(nsamp*sizeof(float)); if(!dphi) return;

    for(size_t i=0;i<nsamp;i++){
        float i0 = iq[2*i+0], q0 = iq[2*i+1];
        if(g_swapiq){ float tmp=i0; i0=q0; q0=tmp; }
        if(g_flipq){ q0 = -q0; }
        float I0=i0, Q0=q0;
        float re = ip*I0 + qp*Q0 + 1e-20f;
        float im = ip*Q0 - qp*I0;
        float ph = atan2f(im, re);
        dphi[i] = g_neg ? -ph : ph;
        ip = I0; qp = Q0;
    }

    int Dtot = (int)floor(fs/48000.0 + 0.5); if(Dtot < 1) Dtot = 1;
    int D1 = 10;
    int D2 = (Dtot + D1/2) / D1; if(D2 < 1){ D2 = 1; D1 = Dtot; }
    double fs1 = fs  / (double)D1;
    double fs2 = fs1 / (double)D2;

    firdec_t *st1 = &st1_state; firdec_t *st2 = &st2_state;
    if(!st_inited || fabs(fs - st_last_fs) > 1.0){
        firdec_free(st1);
        firdec_free(st2);
        if(firdec_init(st1, (g_taps1|1), (float)fs,  100000.0f, D1)!=0){ free(dphi); return; }
        if(firdec_init(st2,  63,  (float)fs1, 18000.0f, D2)!=0){ firdec_free(st1); free(dphi); return; }
        st_last_fs = fs; st_inited = 1;
        dc_x1 = 0.0f; dc_y1 = 0.0f;
    }

    size_t cap1 = nsamp / (size_t)D1 + 8;
    float *y1 = (float*)malloc(cap1*sizeof(float));
    if(!y1){ firdec_free(st1); firdec_free(st2); free(dphi); return; }
    size_t n1 = firdec_push(st1, dphi, nsamp, y1, cap1);

    size_t cap2 = n1 / (size_t)D2 + 8;
    float *y2 = (float*)malloc(cap2*sizeof(float));
    if(!y2){ free(y1); firdec_free(st1); firdec_free(st2); free(dphi); return; }
    size_t n2 = firdec_push(st2, y1, n1, y2, cap2);

    float Fs_audio = (float)(fs2 > 0.0 ? fs2 : 48000.0);
    float a = expf(-1.0f/(Fs_audio*50e-6f));
    static float y_em = 0.0f;
    for(size_t i=0;i<n2;i++){
        float xin = y2[i];
        // DC blocker: y = xin - x1 + r*y1, r ~ 0.995
        const float r = 0.995f;
        float ydc = xin - dc_x1 + r*dc_y1;
        dc_x1 = xin; dc_y1 = ydc;
        float x = ydc;
        if(g_deemph){ y_em = a*y_em + (1.0f - a)*x; } else { y_em = x; }
        float y = g_gain * y_em;
        if(y >  1.0f) y = 1.0f;
        if(y < -1.0f) y = -1.0f;
        y2[i] = y;
    }

    if(g_debug){
        static unsigned dbg=0; if(++dbg % 10 == 0){
            double rms=0; for(size_t ii=0;ii<n2;ii++){ double v=y2[ii]; rms+=v*v; }
            rms = n2? sqrt(rms/n2) : 0.0;
            uint64_t aw = g_ring.hdr? atomic_load(&g_ring.hdr->wpos):0;
            uint64_t ar = g_ring.hdr? atomic_load(&g_ring.hdr->rpos):0;
            fprintf(stderr,"[wfmd] nsamp=%zu n2=%zu fs=%.0f D1=%d D2=%d audio_rms=%.4f aW=%llu aR=%llu\n",
                nsamp, n2, fs, D1, D2, rms, (unsigned long long)aw, (unsigned long long)ar);
        }
    }
    if(n2) push_audio(y2, n2);

    free(y2);
    free(y1);
    free(dphi);
}


static void demod_from_iq_ring(void){
    phiq_hdr_t *h = g_iq.hdr;
    if(!h) return;

    const uint32_t cap = h->capacity;
    const uint32_t bps = h->bytes_per_samp; // 8 for CF32
    if(cap == 0 || bps == 0) return;

    // Determine how many bytes are available
    uint64_t w = atomic_load(&h->wpos);
    uint64_t r = atomic_load(&h->rpos);
    uint32_t used = (uint32_t)h->used;

    if(used == 0 || w == r) return;

    // Drain at most a chunk to bound latency
    size_t avail_bytes = (size_t)used;
    const size_t max_bytes = 1<<18; // ~256 KB per tick (more audio per drain)
    size_t bytes = avail_bytes > max_bytes ? max_bytes : avail_bytes;
    // Align to whole complex samples (bps bytes per sample)
    bytes -= bytes % bps;
    if(bytes == 0) return;


    size_t cap_bytes = (size_t)cap;
    uint8_t *src = h->data;

    size_t mod = (size_t)(r % cap_bytes);
    size_t first = bytes;
    if(mod + bytes > cap_bytes) first = cap_bytes - mod;

    // Allocate temp contiguous buffer
    uint8_t *tmp = (uint8_t*)malloc(bytes);
    if(!tmp) return;

    memcpy(tmp, src + mod, first);
    if(first < bytes){
        memcpy(tmp + first, src, bytes - first);
    }

    // Advance rpos/used
    uint64_t new_r = (r + bytes) % cap_bytes;
    atomic_store(&h->rpos, new_r);
    if(used > bytes) atomic_store(&h->used, used - (uint32_t)bytes);
    else atomic_store(&h->used, 0u);

    // Interpret as interleaved CF32 (I,Q) float
    size_t nsamp = bytes / bps;        // complex samples count
    float *f = (float*)tmp;

    double fs = h->sample_rate;
    if(!(fs > 0.0)) fs = g_fs;

    demod_block((const float*)f, nsamp, fs);

    free(tmp);
}
static void *run_iq(void *arg){
    const char *sock = (const char*)arg;
    int fd = uds_connect(sock);
    if(fd < 0){ fprintf(stderr,"[wfmd] iq connect failed: %s\n", strerror(errno)); return NULL; }

    subscribe_feed_msg(fd, "soapy.IQ-info");
    create_feed(fd, "wfmd.audio-info");

    const double audio_fs = 48000.0;
    const size_t audio_sec = 2;
    const size_t ring_bytes = (size_t)(audio_fs * audio_sec * sizeof(float));
    ring_close(&g_ring);
    if(ring_open(&g_ring, ring_bytes, audio_fs)!=0){
        fprintf(stderr,"[wfmd] ring_open failed\n");
    }else{
        int fds[1] = { g_ring.memfd };
        char js[256];
        int n = snprintf(js, sizeof js, "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"info\",\"encoding\":\"utf8\"}", "wfmd.audio-info");
        send_frame_json_with_fds(fd, js, (size_t)n, fds, 1);
    }

    char js[POC_MAX_JSON];
    while(atomic_load(&g_run)){
        // Aggressively drain IQ ring
        for(int k=0;k<8;k++) demod_from_iq_ring();

        int infd = -1; size_t nfds = 1;
        int n = recv_frame_json_with_fds(fd, js, sizeof js, &infd, &nfds, 5);
        if(n > 0){
            char typ[32]={0};
            if(json_get_type(js, typ, sizeof typ)==0 && strcmp(typ,"publish")==0){
                char feed[128]={0};
                if(json_get_string(js,"feed",feed,sizeof feed)==0){
                    if(strcmp(feed,"soapy.IQ-info")==0){
                        if(nfds==1 && infd>=0){
                            // Map the IQ shared ring once for continuous draining
                            iq_ring_close(&g_iq);
                            off_t len = lseek(infd, 0, SEEK_END);
                            if(len > (off_t)sizeof(phiq_hdr_t)){
                                void *base = mmap(NULL, (size_t)len, PROT_READ|PROT_WRITE, MAP_SHARED, infd, 0);
                                if(base && base != MAP_FAILED){
                                    g_iq.memfd = infd; infd = -1;
                                    g_iq.hdr = (phiq_hdr_t*)base;
                                    g_iq.map_bytes = (size_t)len;
                                }
                            }
                        }
                    }
                }
            }
        }
        if(infd>=0) close(infd);
        for(int k=0;k<8;k++) demod_from_iq_ring();
    }

    close(fd);
    return NULL;
}

static void handle_cmd(int fd, const char *line){
    if(strncmp(line,"open",4)==0){ wfmd_publish_memfd(fd); reply(fd, "{\"ok\":true,\"msg\":\"republished\"}"); return; }
    if(strncmp(line,"help",4)==0){
        reply(fd, "{\"ok\":true,\"help\":\"help|start|stop|status|gain <f>\"}");
        return;
    }
    if(strncmp(line,"swapiq ",7)==0){ int v=atoi(line+7); g_swapiq = (v!=0); reply(fd,"{\"ok\":true,\"swapiq\":%d}", (int)g_swapiq); return; }
    if(strncmp(line,"flipq ",6)==0){ int v=atoi(line+6); g_flipq = (v!=0); reply(fd,"{\"ok\":true,\"flipq\":%d}", (int)g_flipq); return; }
    if(strncmp(line,"neg ",4)==0){ int v=atoi(line+4); g_neg = (v!=0); reply(fd,"{\"ok\":true,\"neg\":%d}", (int)g_neg); return; }
    if(strncmp(line,"deemph ",7)==0){ int v=atoi(line+7); g_deemph = (v!=0); reply(fd,"{\"ok\":true,\"deemph\":%d}", (int)g_deemph); return; }
    if(strncmp(line,"taps1 ",6)==0){ int v=atoi(line+6); if(v<31) v=31; if(!(v&1)) v++; g_taps1=v; st_inited=0; reply(fd,"{\"ok\":true,\"taps1\":%d}", v); return; }
    if(strncmp(line,"debug ",6)==0){ int v=atoi(line+6); g_debug = v; reply(fd,"{\"ok\":true,\"debug\":%d}", (int)g_debug); return; }
    if(strncmp(line,"status",6)==0){
        reply(fd, "{\"ok\":true,\"gain\":%.3f,\"fs_in\":%.1f,\"swapiq\":%d,\"flipq\":%d,\"neg\":%d,\"deemph\":%d,\"taps1\":%d,\"debug\":%d}", g_gain, g_fs, (int)g_swapiq,(int)g_flipq,(int)g_neg,(int)g_deemph,(int)g_taps1,(int)g_debug);
        return;
    }
    if(strncmp(line,"gain ",5)==0){
        float g = strtof(line+5,NULL);
        if(g < 0.1f) g = 0.1f;
        if(g > 16.0f) g = 16.0f;
        g_gain = g;
        reply(fd, "{\"ok\":true,\"gain\":%.3f}", g_gain);
        return;
    }
    if(strncmp(line,"start",5)==0){
        reply(fd, "{\"ok\":true,\"msg\":\"started\"}");
        return;
    }
    if(strncmp(line,"stop",4)==0){
        reply(fd, "{\"ok\":true,\"msg\":\"stopped\"}");
        return;
    }
    reply(fd, "{\"ok\":false,\"err\":\"unknown\"}");
}

static void *run_cmd(void *arg){
    const char *sock = (const char*)arg;
    int fd = uds_connect(sock);
    if(fd < 0){ fprintf(stderr,"[wfmd] cmd connect failed: %s\n", strerror(errno)); return NULL; }

    create_feed(fd, "wfmd.config.in");
    create_feed(fd, "wfmd.config.out");
    subscribe_feed_msg(fd, "wfmd.config.in");

    char js[POC_MAX_JSON];
    while(atomic_load(&g_run)){
        int infd=-1; size_t nfds=1;
        int n = recv_frame_json_with_fds(fd, js, sizeof js, &infd, &nfds, 200);
        if(n <= 0) continue;

        char typ[32]={0}; if(json_get_type(js, typ, sizeof typ)!=0) continue;
        if(strcmp(typ,"publish")!=0) continue;

        char feed[128]={0}; if(json_get_string(js,"feed",feed,sizeof feed)!=0) continue;
        if(strcmp(feed,"wfmd.config.in")==0){
            char enc[16]={0}; char data[POC_MAX_JSON]={0};
            if(json_get_string(js,"encoding",enc,sizeof enc)!=0) strcpy(enc,"utf8");
            if(json_get_string(js,"data",data,sizeof data)==0){
                if(strcmp(enc,"utf8")==0) handle_cmd(fd, data);
                else publish_utf8(fd,"wfmd.config.out","{\"ok\":false,\"err\":\"decode\"}");
            }
        }
        if(infd>=0) close(infd);
        for(int k=0;k<8;k++) demod_from_iq_ring();
    }
    close(fd);
    return NULL;
}

const char* plugin_name(void){ return "wfmd"; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    out->name = "wfmd";
    out->version = "0.1.4";
    out->consumes = CONS;
    out->produces = PROD;
    if(ctx && ctx->sock_path){
        strncpy(g_sock_path, ctx->sock_path, sizeof g_sock_path - 1);
        g_sock_path[sizeof g_sock_path - 1] = 0;
    } else g_sock_path[0]=0;
    return true;
}

bool plugin_start(void){
    if(!g_sock_path[0]) return false;
    atomic_store(&g_run, true);
    if(pthread_create(&g_th_cmd, NULL, run_cmd, g_sock_path)!=0){ atomic_store(&g_run,false); return false; }
    if(pthread_create(&g_th_iq,  NULL, run_iq,  g_sock_path)!=0){ atomic_store(&g_run,false); return false; }
    return true;
}

void plugin_stop(void){
    atomic_store(&g_run, false);
    if(g_th_cmd) pthread_join(g_th_cmd, NULL);
    if(g_th_iq)  pthread_join(g_th_iq,  NULL);
    ring_close(&g_ring);
}
