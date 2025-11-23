// wfmd.c — Wideband FM mono demodulator addon (new control-plane ABI, channelized)
// Strict FM practice: channelize (mix+complex LPF) → limiter → discriminator → audio LPF → deemphasis
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"  // shared helpers (advertise/dispatch/replies)
#include "ph_stream.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- small compat for shared memory fd (memfd/shm_open fallback) ---------- */
static int xmemfd_create(const char *name, unsigned int flags){
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    (void)name; (void)flags; errno = ENOSYS; return -1;
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

/* ---------- global state ---------- */
static _Atomic int g_run = 0;
/* start/stop gating — default to stopped for consistency with other addons */
static _Atomic bool g_active = false;
static const char *g_sock = NULL;
static pthread_t   g_thr;
static ph_ctrl_t   g_ctrl;         // control-plane ctx

/* runtime toggles */
static _Atomic bool g_swapiq = false;   // swap I/Q
static _Atomic bool g_flipq  = false;   // flip sign of Q
static _Atomic bool g_neg    = false;   // negate discriminator
static _Atomic bool g_deemph = true;    // apply deemphasis
static _Atomic int  g_taps1  = 101;     // first post-disc LPF taps (odd)
static _Atomic int  g_debug  = 0;       // periodic diagnostics

static _Atomic float  g_gain = 4.0f;    // now atomic to avoid races
static _Atomic double g_fs   = 2400000.0;

/* new knobs */
static _Atomic double g_foff_hz = 0.0;   // digital fine-tune (Hz) pre-disc
static _Atomic double g_bw_hz   = 110e3; // complex LPF cutoff (Hz) for WFM mono ~110 kHz
static _Atomic int    g_tau_us  = 50;    // de-emphasis µs (50 EU / 75 US)

/* IQ shared map */
typedef struct { int memfd; phiq_hdr_t *hdr; size_t map_bytes; } iq_ring_t;
static iq_ring_t g_iq = { .memfd=-1, .hdr=NULL, .map_bytes=0 };
static char g_iq_feed[128] = {0};
static void iq_ring_close(iq_ring_t *r){
    if(!r) return;
    if(r->hdr && r->hdr!=MAP_FAILED) munmap(r->hdr, r->map_bytes);
    if(r->memfd>=0) close(r->memfd);
    r->hdr=NULL; r->map_bytes=0; r->memfd=-1;
}

/* audio ring */
typedef struct { int memfd; phau_hdr_t *hdr; size_t map_bytes; } ring_t;
static ring_t g_ring = { .memfd=-1, .hdr=NULL, .map_bytes=0 };

static void ring_close(ring_t *r){
    if(!r) return;
    if(r->hdr && r->hdr!=MAP_FAILED) munmap(r->hdr, r->map_bytes);
    if(r->memfd>=0) close(r->memfd);
    r->hdr=NULL; r->map_bytes=0; r->memfd=-1;
}
static int ring_open(ring_t *r, size_t audio_capacity_bytes, double fs){
    memset(r, 0, sizeof *r);
    int fd = create_shm_fd("ph-wfmd-audio", sizeof(phau_hdr_t)+audio_capacity_bytes);
    if(fd<0) return -1;
    void *map = mmap(NULL, sizeof(phau_hdr_t)+audio_capacity_bytes,
                     PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(!map || map==MAP_FAILED){ int e=errno; close(fd); errno=e; return -1; }
    r->memfd = fd; r->hdr = (phau_hdr_t*)map; r->map_bytes = sizeof(phau_hdr_t)+audio_capacity_bytes;

    memset(r->hdr, 0, r->map_bytes);
    r->hdr->magic          = PHAU_MAGIC;
    r->hdr->version        = PHAU_VER;
    atomic_store(&r->hdr->seq,  0);
    atomic_store(&r->hdr->wpos, 0);
    atomic_store(&r->hdr->rpos, 0);
    r->hdr->capacity       = (uint32_t)audio_capacity_bytes;
    r->hdr->bytes_per_samp = 4;
    r->hdr->channels       = 1;
    r->hdr->sample_rate    = fs;                  // will be kept in-sync dynamically
    r->hdr->fmt            = PHAU_FMT_F32;
    return 0;
}
static void ring_push_f32(ring_t *r, const float *x, size_t n_frames){
    if(!r->hdr || !x || !n_frames) return;
    const size_t bps = r->hdr->bytes_per_samp;   // 4
    const size_t ch  = r->hdr->channels;         // 1
    const size_t fsz = bps * ch;
    const size_t bytes = n_frames * fsz;

    uint64_t w = atomic_load(&r->hdr->wpos);
    uint64_t rp= atomic_load(&r->hdr->rpos);
    size_t cap = r->hdr->capacity;

    if(bytes > cap) return; // oversized burst, drop

    // make space if needed (single-producer policy: advance rpos absolutely)
    if((w - rp) + bytes > cap){
        uint64_t new_r = (w + bytes) - cap;
        atomic_store(&r->hdr->rpos, new_r);
        rp = new_r;
    }

    size_t wp = (size_t)(w % cap);
    size_t first = bytes;
    if(wp + bytes > cap) first = cap - wp;

    memcpy(r->hdr->data + wp, x, first);
    if(bytes > first) memcpy(r->hdr->data, ((const uint8_t*)x)+first, bytes-first);

    atomic_store(&r->hdr->wpos, w + bytes);
    atomic_fetch_add(&r->hdr->seq, 1);
}

/* ---------- DSP helpers ---------- */
/* scalar FIR decimator (for discriminator→audio) */
typedef struct {
    float *taps; int ntaps;
    float *zb;   int zpos;
    int R;
} firdec_t;

static void   firdec_free(firdec_t *d){ if(!d) return; free(d->taps); free(d->zb); d->taps=NULL; d->zb=NULL; d->ntaps=0; d->zpos=0; d->R=1; }
static int    firdec_init(firdec_t *d, int ntaps, float fs_in, float fc, int R){
    memset(d,0,sizeof(*d)); if(ntaps<31) ntaps=31; d->ntaps = ntaps|1; d->R = R<1?1:R;
    d->taps = (float*)malloc((size_t)d->ntaps*sizeof(float));
    d->zb   = (float*)calloc((size_t)d->ntaps, sizeof(float));
    if(!d->taps || !d->zb) return -1;
    int M=d->ntaps, m2=(M-1)/2;
    double fn=(double)fc/(double)fs_in; if(fn>0.499) fn=0.499;
    double sum=0.0;
    for(int n=0;n<M;n++){
        int k=n-m2;
        double w=0.54 - 0.46*cos(2.0*M_PI*n/(M-1));
        double x=(k==0)?(2.0*fn):(sin(2.0*M_PI*fn*k)/(M_PI*k));
        double h=w*x; d->taps[n]=(float)h; sum+=h;
    }
    for(int n=0;n<M;n++) d->taps[n]=(float)(d->taps[n]/sum);
    d->zpos=0; return 0;
}
static size_t firdec_push(firdec_t *d, const float *in, size_t ns, float *out, size_t out_cap){
    size_t out_n=0;
    for(size_t i=0;i<ns;i++){
        d->zb[d->zpos]=in[i];
        d->zpos=(d->zpos+1)%d->ntaps;
        if(((i+1)%d->R)==0){
            float acc=0.0f; int idx=d->zpos;
            for(int t=0;t<d->ntaps;t++){ idx--; if(idx<0) idx=d->ntaps-1; acc += d->taps[t]*d->zb[idx]; }
            if(out_n<out_cap) out[out_n++]=acc;
        }
    }
    return out_n;
}

/* NCO */
typedef struct { double ph, dph; } nco_t;
static inline void nco_set(nco_t *n, double fs, double foff){ n->dph = 2.0*M_PI*foff / fs; }
static inline void nco_advance(nco_t *n, float *cs, float *sn){
    n->ph += n->dph;
    if(n->ph >  M_PI) n->ph -= 2.0*M_PI;
    if(n->ph < -M_PI) n->ph += 2.0*M_PI;
    *cs = (float)cos(n->ph); *sn = (float)sin(n->ph);
}

/* complex FIR decimator (pre-discriminator channel filter) */
typedef struct {
    float *taps; int ntaps;
    float *ziI,*ziQ; int zpos;
    int R;
} cfirdec_t;

static void cfirdec_free(cfirdec_t *d){
    if(!d) return;
    free(d->taps);
    free(d->ziI);
    free(d->ziQ);
    d->taps = NULL;
    d->ziI  = NULL;
    d->ziQ  = NULL;
    d->ntaps = 0;
    d->zpos  = 0;
    d->R     = 1;
}
static int cfirdec_init(cfirdec_t *d, int ntaps, float fs_in, float fc, int R){
    memset(d,0,sizeof(*d));
    if(ntaps < 63) ntaps = 63;
    d->ntaps = ntaps | 1;
    d->R = (R < 1) ? 1 : R;
    d->taps=(float*)malloc((size_t)d->ntaps*sizeof(float));
    d->ziI =(float*)calloc((size_t)d->ntaps,sizeof(float));
    d->ziQ =(float*)calloc((size_t)d->ntaps,sizeof(float));
    if(!d->taps||!d->ziI||!d->ziQ) return -1;
    int M=d->ntaps, m2=(M-1)/2; double fn=fc/fs_in; if(fn>0.49) fn=0.49;
    double sum=0; for(int n=0;n<M;n++){ int k=n-m2;
        double w=0.54-0.46*cos(2.0*M_PI*n/(M-1));
        double x=(k==0)?(2.0*fn):(sin(2.0*M_PI*fn*k)/(M_PI*k));
        double h=w*x; d->taps[n]=(float)h; sum+=h;
    }
    for(int n=0;n<M;n++) d->taps[n]/=(float)sum;
    return 0;
}

/* mix→complex FIR decimate; returns #complex frames written to outIQ (interleaved)
   NOTE: limiter removed from here (applied AFTER channel filtering) */
static size_t cfirdec_mix_and_push(cfirdec_t *d, nco_t *nco,
                                   const float *iq, size_t ns,
                                   float *outIQ, size_t out_cap /* interleaved I,Q */)
{
    size_t out_n=0;
    const int R = d->R;
    for(size_t i=0;i<ns;i++){
        float I=iq[2*i+0], Q=iq[2*i+1];

        /* optional swap/flip */
        if(atomic_load(&g_swapiq)){ float t=I; I=Q; Q=t; }
        if(atomic_load(&g_flipq)) Q = -Q;

        /* NCO mix (shift desired to DC) */
        float cs,sn; nco_advance(nco,&cs,&sn);
        float Ir =  I*cs + Q*sn;
        float Qr = -I*sn + Q*cs;

        /* complex FIR */
        d->ziI[d->zpos]=Ir; d->ziQ[d->zpos]=Qr;
        d->zpos = (d->zpos + 1) % d->ntaps;

        if(((i+1)%R)==0){
            float accI=0, accQ=0; int idx=d->zpos;
            for(int t=0;t<d->ntaps;t++){ idx--; if(idx<0) idx=d->ntaps-1;
                accI += d->taps[t]*d->ziI[idx];
                accQ += d->taps[t]*d->ziQ[idx];
            }
            if(out_n+2 <= out_cap){
                outIQ[out_n+0]=accI; outIQ[out_n+1]=accQ; out_n+=2;
            }
        }
    }
    return out_n/2; /* complex frames */
}

/* ---------- work buffers (persistent) ---------- */
typedef struct {
    float *bb;     size_t bb_cap;     /* I,Q after channel decim (interleaved) */
    float *dphi;   size_t dphi_cap;   /* discriminator output @ fs_ch */
    float *y1;     size_t y1_cap;     /* after a1 */
    float *y2;     size_t y2_cap;     /* after a2 (final audio to push) */
    float *tmp_f;  size_t tmp_f_cap;  /* CS16→float conversion scratch */
} workbuf_t;

static workbuf_t g_wb = {0};

static int ensure_cap(float **ptr, size_t *cap, size_t need){
    if(*cap >= need) return 0;
    size_t ncap = (*cap ? *cap : 1024);
    while(ncap < need) ncap <<= 1;
    void *p = realloc(*ptr, ncap * sizeof(float));
    if(!p) return -1;
    *ptr = (float*)p; *cap = ncap; return 0;
}

/* ---------- demod ---------- */
static cfirdec_t rf_ch; static nco_t nco;
static firdec_t  a1, a2;   // audio post-discriminator filters
static int       ch_inited=0, ainit=0;
static double    last_fs_in=0, last_bw=0, last_fo=0;
static int       last_D1=0, last_D2=0, last_taps1=0;
static float     dc_x1=0.0f, dc_y1=0.0f;

static void push_audio(const float *y, size_t n){ ring_push_f32(&g_ring, y, n); }

/* post-channel limiter (constant envelope), vectorized */
static inline void limit_iq_vec(float *iq, size_t n_complex){
    for(size_t i=0;i<n_complex;i++){
        float I = iq[2*i+0], Q = iq[2*i+1];
        float m2 = I*I + Q*Q;
        if(m2 > 0.0f){
            float m = sqrtf(m2) + 1e-12f;
            iq[2*i+0] = I / m;
            iq[2*i+1] = Q / m;
        }else{
            iq[2*i+0] = 0.0f; iq[2*i+1] = 0.0f;
        }
    }
}

static void demod_block(const float *iq, size_t nsamp, double fs_in){
    if(nsamp < 32) return;

    /* ---- Stage A: channelize BEFORE discriminator ---- */
    /* Aim for ~240kS/s baseband */
    int Rch = (int)floor(fs_in / 240000.0); if(Rch<1) Rch=1;
    double fs_ch = fs_in / (double)Rch;

    double foff = atomic_load(&g_foff_hz);
    double bw   = atomic_load(&g_bw_hz);

    /* (re)init channelizer when fs/bw/fo changed */
    if(!ch_inited || fabs(last_fs_in - fs_in) > 1.0 || fabs(last_bw - bw) > 1.0 || last_fo != foff){
        cfirdec_free(&rf_ch);
        if(cfirdec_init(&rf_ch, 151, (float)fs_in, (float)bw, Rch)!=0) return;
        nco.ph=0.0; nco_set(&nco, fs_in, foff);
        ch_inited=1; last_fs_in=fs_in; last_bw=bw; last_fo=foff;
    }else{
        nco_set(&nco, fs_in, foff);
    }

    /* channelize to fs_ch */
    size_t max_out = nsamp/Rch + 8;
    if(ensure_cap(&g_wb.bb, &g_wb.bb_cap, max_out*2)) return;
    size_t nbb = cfirdec_mix_and_push(&rf_ch, &nco, iq, nsamp, g_wb.bb, max_out*2);
    if(nbb==0) return;

    /* limiter AFTER channel LPF (on decimated IQ) */
    limit_iq_vec(g_wb.bb, nbb);

    /* ---- Discriminator at fs_ch ---- */
    if(ensure_cap(&g_wb.dphi, &g_wb.dphi_cap, nbb)) return;
    static float ip=0.0f, qp=0.0f;
    const bool neg = atomic_load(&g_neg);
    for(size_t i=0;i<nbb;i++){
        float I0=g_wb.bb[2*i+0], Q0=g_wb.bb[2*i+1];
        float re = ip*I0 + qp*Q0;
        float im = ip*Q0 - qp*I0;
        float ph = (re==0.0f && im==0.0f) ? 0.0f : atan2f(im, re);
        g_wb.dphi[i]  = neg ? -ph : ph;
        ip = I0; qp = Q0;
    }

    /* ---- Stage B: audio LP + decimate to ~48 kHz ---- */
    /* Compute total decimation close to 48k and derive D1,D2. Enforce anti-alias fc before each stage. */
    int Dtot = (int)floor(fs_ch/48000.0 + 0.5); if(Dtot<1) Dtot=1;
    int D1 = (Dtot >= 5) ? 5 : Dtot;
    int D2 = (D1>0) ? (Dtot / D1) : 1; if(D2<1) D2=1;
    double fs1 = fs_ch / (double)D1;
    double fs2 = fs1   / (double)D2;  /* final audio Fs */

    int cur_taps1 = atomic_load(&g_taps1);

    /* re-init audio filters when fs_in changed, or D1/D2, or taps1 changed */
    if(!ainit || fabs(last_fs_in - fs_in) > 1.0 || last_D1!=D1 || last_D2!=D2 || last_taps1!=cur_taps1){
        firdec_free(&a1); firdec_free(&a2);

        /* Correct anti-alias cutoffs:
           a1 runs at fs_ch and decimates by D1 → fc1 ≤ 0.45*(fs_ch/D1).
           a2 runs at fs1 and decimates by D2 → fc2 ≤ 0.45*(fs1/D2), also cap ~17 kHz for audio shape. */
        float fc1 = (float)(0.45 * (fs_ch / (double)D1));
        float fc2 = (float)(0.45 * (fs1   / (double)D2));
        if(fc2 > 17000.0f) fc2 = 17000.0f;

        if(firdec_init(&a1, (cur_taps1|1), (float)fs_ch,  fc1, D1)!=0) { return; }
        if(firdec_init(&a2,  63,          (float)fs1,     fc2, D2)!=0) { firdec_free(&a1); return; }

        ainit=1; dc_x1=dc_y1=0.0f;
        last_D1=D1; last_D2=D2; last_taps1=cur_taps1;
    }

    /* run a1 */
    size_t cap1 = nbb/(size_t)D1 + 8;
    if(ensure_cap(&g_wb.y1, &g_wb.y1_cap, cap1)) return;
    size_t n1 = firdec_push(&a1, g_wb.dphi, nbb, g_wb.y1, cap1);

    /* run a2 */
    size_t cap2 = n1/(size_t)D2 + 8;
    if(ensure_cap(&g_wb.y2, &g_wb.y2_cap, cap2)) return;
    size_t n2 = firdec_push(&a2, g_wb.y1, n1, g_wb.y2, cap2);

    float Fs_audio = (float)(fs2>0.0?fs2:48000.0f);

    /* keep audio ring metadata in sync if drifted */
    if(g_ring.hdr && fabs(g_ring.hdr->sample_rate - (double)Fs_audio) > 0.5){
        g_ring.hdr->sample_rate = (double)Fs_audio;
    }

    /* de-emphasis (single-pole IIR) + DC blocker + gain + clip */
    int tau_us = atomic_load(&g_tau_us); if(tau_us!=50 && tau_us!=75) tau_us=50;
    float a = expf((float)(-1.0/(Fs_audio*((float)tau_us*1e-6f))));
    static float y_em = 0.0f;
    const bool do_deemph = atomic_load(&g_deemph);
    const float gain = atomic_load(&g_gain);
    for(size_t i=0;i<n2;i++){
        float xin = g_wb.y2[i];
        /* DC blocker */
        const float r=0.995f; float ydc = xin - dc_x1 + r*dc_y1; dc_x1=xin; dc_y1=ydc;
        float x = ydc;
        if(do_deemph) y_em = a*y_em + (1.0f - a)*x; else y_em = x;
        float y = gain * y_em;
        if(y >  1.0f) y =  1.0f;
        if(y < -1.0f) y = -1.0f;
        g_wb.y2[i]=y;
    }
    if(n2) push_audio(g_wb.y2, n2);

    if(atomic_load(&g_debug)){
        static unsigned dbg=0; if(++dbg % 10 == 0){
            double rms=0; for(size_t ii=0;ii<n2;ii++){ double v=g_wb.y2[ii]; rms+=v*v; }
            rms = n2? sqrt(rms/n2) : 0.0;
            uint64_t aw = g_ring.hdr? atomic_load(&g_ring.hdr->wpos):0;
            uint64_t ar = g_ring.hdr? atomic_load(&g_ring.hdr->rpos):0;
            fprintf(stderr,"[wfmd] ns_in=%zu nbb=%zu fs_in=%.0f fs_ch=%.0f D1=%d D2=%d fc1=%.0f fc2=%.0f tau=%dus audio_fs=%.1f audio_rms=%.4f aW=%llu aR=%llu\n",
                nsamp, nbb, fs_in, fs_ch, D1, D2, 
                (double)(0.45*(fs_ch/(double)D1)), 
                (double)fmin(0.45*(fs1/(double)D2),17000.0),
                tau_us, (double)Fs_audio, rms,
                (unsigned long long)aw, (unsigned long long)ar);
        }
    }
}

/* ---------- IQ ring drain ---------- */
static void demod_from_iq_ring(void){
    /* Do not read/advance the IQ ring when inactive: behave like a real stop. */
    if (!atomic_load(&g_active)) return;

    phiq_hdr_t *h = g_iq.hdr;
    if(!h) return;
    const uint32_t cap = h->capacity;
    const uint32_t bps = h->bytes_per_samp; // bytes per complex frame
    if(cap==0 || bps==0) return;

    /* derive available from absolute counters */
    uint64_t r = atomic_load(&h->rpos);
    uint64_t w = atomic_load(&h->wpos);
    uint64_t avail = (w > r) ? (w - r) : 0;
    if(avail < bps) return;

    const size_t max_bytes = 1u<<18; // ~256KB per tick
    size_t bytes = (size_t)((avail > max_bytes) ? max_bytes : avail);
    bytes -= bytes % bps;
    if(bytes==0) return;

    size_t cap_bytes = (size_t)cap;
    size_t mod = (size_t)(r % cap_bytes);
    size_t first = (mod + bytes > cap_bytes) ? (cap_bytes - mod) : bytes;

    uint8_t *tmp = (uint8_t*)malloc(bytes);
    if(!tmp) return;

    memcpy(tmp, h->data + mod, first);
    if(first < bytes) memcpy(tmp + first, h->data, bytes - first);

    /* advance absolute read pointer; consumer never writes 'used' */
    atomic_store(&h->rpos, r + bytes);

    size_t nsamp = bytes / bps;          // number of complex frames
    double fs = h->sample_rate; if(!(fs>0.0)) fs = atomic_load(&g_fs);

    /* handle formats */
    if(h->fmt == PHIQ_FMT_CF32){
        float *f = (float*)tmp;          // interleaved I,Q float32
        demod_block((const float*)f, nsamp, fs);
    }else if(h->fmt == PHIQ_FMT_CS16){
        /* convert to float interleaved */
        size_t need = nsamp * 2;
        if(ensure_cap(&g_wb.tmp_f, &g_wb.tmp_f_cap, need)){ free(tmp); return; }
        const int16_t *s = (const int16_t*)tmp;
        const float scale = 1.0f/32768.0f;
        for(size_t i=0;i<nsamp;i++){
            g_wb.tmp_f[2*i+0] = (float)s[2*i+0] * scale;
            g_wb.tmp_f[2*i+1] = (float)s[2*i+1] * scale;
        }
        demod_block((const float*)g_wb.tmp_f, nsamp, fs);
    }else{
        // unknown format; drop
    }

    free(tmp);
}

static void wfmd_publish_memfd(int fd){
    if(!g_ring.hdr) return;
    char js[POC_MAX_JSON];

    const char *enc = "f32";
    unsigned ch = g_ring.hdr->channels ? g_ring.hdr->channels : 1;
    double   fs = g_ring.hdr->sample_rate;

    int n = snprintf(js, sizeof js,
        "{"
          "\"type\":\"publish\","
          "\"feed\":\"%s\","
          "\"subtype\":\"shm_map\","
          "\"proto\":\"" PH_PROTO_AUDIO_RING "\","
          "\"version\":\"0.1\","
          "\"size\":%u,"
          "\"mode\":\"rw\","
          "\"kind\":\"audio\","
          "\"encoding\":\"%s\","
          "\"sample_rate\":%.0f,"
          "\"channels\":%u,"
          "\"desc\":\"WFMD audio ring (f32)\""
        "}",
        "wfmd.audio-info",
        g_ring.hdr->capacity,
        enc,
        fs,
        ch);
    int fds[1] = { g_ring.memfd };
    send_frame_json_with_fds(fd, js, (size_t)n, fds, 1);
}

/* ---------- command handler (new ABI) ---------- */
static void on_cmd(ph_ctrl_t *c, const char *line, void *user){
    (void)user;
    while(*line==' '||*line=='\t') line++;

    if(strncmp(line,"help",4)==0){
        ph_reply(c, "{\"ok\":true,"
                     "\"help\":\"help|open|start|stop|status|"
                              "subscribe <usage> <feed>|unsubscribe <usage>|"
                              "gain <f>|swapiq <0|1>|flipq <0|1>|neg <0|1>|deemph <0|1>|"
                              "taps1 <odd>|debug <int>|foff <Hz>|bw <Hz>|tau <50|75>\"}");
        return;
    }
    if(strncmp(line,"open",4)==0){ wfmd_publish_memfd(c->fd); ph_reply_ok(c,"republished"); return; }

    if(strncmp(line,"subscribe ",10)==0){
        const char *p = line+10;
        while(*p==' '||*p=='\t') p++;
        char usage[32]={0};
        char feed[128]={0};
        if(sscanf(p,"%31s %127s", usage, feed)!=2){
            ph_reply_err(c, "subscribe <usage> <feed>");
            return;
        }
        if(strcmp(usage,"iq-source")!=0){
            ph_reply_err(c, "unknown usage (expected iq-source)");
            return;
        }
        if(g_iq_feed[0]){
            ph_unsubscribe(c->fd, g_iq_feed);
            g_iq_feed[0]='\0';
        }
        snprintf(g_iq_feed, sizeof g_iq_feed, "%s", feed);
        ph_subscribe(c->fd, g_iq_feed);
        ph_reply_okf(c, "iq-source=%s", g_iq_feed);
        return;
    }

    if(strncmp(line,"unsubscribe ",12)==0){
        const char *p = line+12;
        while(*p==' '||*p=='\t') p++;
        char usage[32]={0};
        if(sscanf(p,"%31s", usage)!=1){
            ph_reply_err(c, "unsubscribe <usage>");
            return;
        }
        if(strcmp(usage,"iq-source")!=0){
            ph_reply_err(c, "unknown usage (expected iq-source)");
            return;
        }
        if(g_iq_feed[0]){
            ph_unsubscribe(c->fd, g_iq_feed);
            g_iq_feed[0]='\0';
        }
        ph_reply_ok(c, "unsubscribed iq-source");
        return;
    }
    if(strncmp(line,"swapiq ",7)==0){ int v=atoi(line+7); g_swapiq=(v!=0); ph_reply_okf(c,"swapiq=%d",(int)g_swapiq); return; }
    if(strncmp(line,"flipq ",6)==0){ int v=atoi(line+6); g_flipq=(v!=0);  ph_reply_okf(c,"flipq=%d",(int)g_flipq);  return; }
    if(strncmp(line,"neg ",4)==0){   int v=atoi(line+4); g_neg=(v!=0);     ph_reply_okf(c,"neg=%d",(int)g_neg);      return; }
    if(strncmp(line,"deemph ",7)==0){int v=atoi(line+7); g_deemph=(v!=0);  ph_reply_okf(c,"deemph=%d",(int)g_deemph);return; }
    if(strncmp(line,"taps1 ",6)==0){
        int v=atoi(line+6); if(v<31) v=31; if(!(v&1)) v++; g_taps1=v;
        ph_reply_okf(c,"taps1=%d", v); return;
    }
    if(strncmp(line,"debug ",6)==0){ int v=atoi(line+6); g_debug=v; ph_reply_okf(c,"debug=%d", (int)g_debug); return; }

    if(strncmp(line,"gain ",5)==0){
        float g = strtof(line+5,NULL);
        if(g < 0.1f) { g = 0.1f; }
        if(g > 16.0f){ g = 16.0f; }
        atomic_store(&g_gain, g);
        ph_reply_okf(c, "gain=%.3f", (double)g);
        return;
    }
    if(strncmp(line,"foff ",5)==0){
        double f = strtod(line+5,NULL);
        g_foff_hz = f;
        ph_reply_okf(c,"foff=%.1f Hz", f);
        return;
    }
    if(strncmp(line,"bw ",3)==0){
        double b = strtod(line+3,NULL);
        if(b <  60000.0)  b =  60000.0;
        if(b > 200000.0)  b = 200000.0;
        g_bw_hz = b;
        ph_reply_okf(c,"bw=%.0f Hz", b);
        return;
    }
    if(strncmp(line,"tau ",4)==0){
        int t = atoi(line+4);
        if(t!=50 && t!=75) { ph_reply_err(c,"tau must be 50 or 75"); return; }
        g_tau_us = t; ph_reply_okf(c,"tau=%d us", t);
        return;
    }

    if(strncmp(line,"status",6)==0){
        char js[512];
        snprintf(js,sizeof js,
            "{\"ok\":true,"
              "\"gain\":%.3f,\"fs_hint\":%.1f,"
              "\"swapiq\":%d,\"flipq\":%d,\"neg\":%d,\"deemph\":%d,"
              "\"taps1\":%d,\"debug\":%d,"
              "\"foff_hz\":%.1f,\"bw_hz\":%.1f,\"tau_us\":%d,"
              "\"active\":%d}",
            (double)atomic_load(&g_gain), atomic_load(&g_fs),
            (int)g_swapiq,(int)g_flipq,(int)g_neg,(int)g_deemph,
            (int)atomic_load(&g_taps1),(int)g_debug,
            (double)atomic_load(&g_foff_hz),(double)atomic_load(&g_bw_hz),(int)atomic_load(&g_tau_us),
            (int)atomic_load(&g_active));
        ph_reply(c, js);
        return;
    }

    if(strncmp(line,"start",5)==0){ g_active = true;  ph_reply_ok(c,"started"); return; }
    if(strncmp(line,"stop",4)==0){  g_active = false; ph_reply_ok(c,"stopped"); return; }

    ph_reply_err(c, "unknown");
}

/* ---------- worker (single thread: ctrl + IQ) ---------- */
static void *run(void *arg){
    (void)arg;
    int fd = -1;
    for(int i=0;i<50;i++){ fd = uds_connect(g_sock ? g_sock : PH_SOCK_PATH); if(fd>=0) break; sleep_ms(100); }
    if(fd<0) return NULL;

    /* control-plane advertise */
    ph_ctrl_init(&g_ctrl, fd, "wfmd");
    ph_ctrl_advertise(&g_ctrl);
    ph_create_feed(fd, "wfmd.audio-info");

    /* setup audio ring and publish its FD once */
    const double audio_fs = 48000.0;
    const size_t audio_sec = 2;
    const size_t ring_bytes = (size_t)(audio_fs * audio_sec * sizeof(float));
    ring_close(&g_ring);
    if(ring_open(&g_ring, ring_bytes, audio_fs)==0) wfmd_publish_memfd(fd);

    atomic_store(&g_run, 1);
    char js[POC_MAX_JSON];

    while(atomic_load(&g_run)){
        /* Only demod/drain when active */
        if (atomic_load(&g_active)) {
            for (int k = 0; k < 8; k++) demod_from_iq_ring();
        }

        int infd=-1; size_t nfds=1;
        int got = recv_frame_json_with_fds(fd, js, sizeof js, &infd, &nfds, 10); /* shorter poll */
        if(got<=0) continue;

        /* 1) commands to wfmd.config.in */
        if (ph_ctrl_dispatch(&g_ctrl, js, (size_t)got, on_cmd, NULL)) {
            if(infd>=0) close(infd);
            continue;
        }

        /* 2) map incoming IQ ring from soapy (publish + 1 FD) */
        const char *p_type = strstr(js, "\"type\"");
        const char *p_feed = strstr(js, "\"feed\"");
        if(p_type && p_feed){
            char type[16]={0}, feed[128]={0};
            const char *t = strchr(p_type, ':'); if(t){ t++; while(*t==' '||*t=='\"'){ if(*t=='\"'){ t++; break; } t++; }
                size_t i=0; while(*t && *t!='\"' && i+1<sizeof type){ type[i++]=*t++; } type[i]='\0'; }
            const char *f = strchr(p_feed, ':'); if(f){ f++; while(*f==' '||*f=='\"'){ if(*f=='\"'){ f++; break; } f++; }
                size_t i=0; while(*f && *f!='\"' && i+1<sizeof feed){ feed[i++]=*f++; } feed[i]='\0'; }

            if(strcmp(type,"publish")==0 && g_iq_feed[0] && strcmp(feed,g_iq_feed)==0){
                if(nfds==1 && infd>=0){
                    iq_ring_close(&g_iq);
                    struct stat st;
                    if(fstat(infd, &st)==0 && st.st_size > (off_t)sizeof(phiq_hdr_t)){
                        void *base = mmap(NULL, (size_t)st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, infd, 0);
                        if(base && base != MAP_FAILED){
                            g_iq.memfd = infd; infd=-1;
                            g_iq.hdr = (phiq_hdr_t*)base;
                            g_iq.map_bytes = (size_t)st.st_size;
                        }
                    }
                }
            }
        }
        if(infd>=0) close(infd);
    }

    close(fd);
    iq_ring_close(&g_iq);
    ring_close(&g_ring);
    /* free DSP states and work buffers */
    cfirdec_free(&rf_ch); firdec_free(&a1); firdec_free(&a2);
    free(g_wb.bb);   free(g_wb.dphi); free(g_wb.y1); free(g_wb.y2); free(g_wb.tmp_f);
    memset(&g_wb, 0, sizeof(g_wb));
    return NULL;
}

/* ---------- plugin ABI ---------- */
const char* plugin_name(void){ return "wfmd"; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    PH_ENSURE_ABI(ctx);
    static const char *CONS[] = { "wfmd.config.in", NULL };
    static const char *PROD[] = { "wfmd.config.out","wfmd.audio-info", NULL };
    g_sock = ctx->sock_path;
    if(out){
        out->caps_size = sizeof(*out);
        out->name = plugin_name();
        out->version = "0.4.0";
        out->consumes = CONS;
        out->produces = PROD;
        out->feat_bits = PH_FEAT_PCM;
    }
    return true;
}

bool plugin_start(void){
    return pthread_create(&g_thr, NULL, run, NULL)==0;
}

void plugin_stop(void){
    atomic_store(&g_run, 0);
    pthread_join(g_thr, NULL);
    /* free DSP states to help leak detectors (also done in run exit path) */
    cfirdec_free(&rf_ch); firdec_free(&a1); firdec_free(&a2);
    iq_ring_close(&g_iq);
    ring_close(&g_ring);
    free(g_wb.bb);   free(g_wb.dphi); free(g_wb.y1); free(g_wb.y2); free(g_wb.tmp_f);
    memset(&g_wb, 0, sizeof(g_wb));
}

