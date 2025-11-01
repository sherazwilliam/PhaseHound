#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <time.h>

#ifndef PHAU_MAGIC
#define PHAU_MAGIC 0x50484155u /* "PHAU" */
#endif
#ifndef PHAU_VER
#define PHAU_VER   0x00010000u
#endif
#ifndef PHAU_FMT_F32
#define PHAU_FMT_F32 1u
#endif

typedef struct {
    uint32_t magic, version;
    _Atomic uint64_t seq;
    _Atomic uint64_t wpos;
    _Atomic uint64_t rpos;
    uint32_t capacity;
    uint32_t used;
    uint32_t bytes_per_samp;
    uint32_t channels;
    double   sample_rate;
    uint32_t fmt;
    uint8_t  reserved[64];
    uint8_t  data[];
} phau_hdr_t;

typedef struct {
    int         memfd;
    phau_hdr_t *hdr;
    size_t      map_bytes;
} ring_t;

static atomic_bool g_plugin_alive = false;

static pthread_t   g_cmd_th;
static atomic_bool g_cmd_run = false;
static char        g_sock_path[256];

static pthread_t   g_play_th;
static atomic_bool g_play_run = false;

static char        g_alsa_dev[128] = "default";
static snd_pcm_t  *g_pcm          = NULL;
static unsigned    g_pcm_rate     = 48000;
static unsigned    g_pcm_ch       = 1;

static ring_t g_ring = { .memfd = -1, .hdr = NULL, .map_bytes = 0 };

// -----------------------------------------------------------------------------
// tiny utils

static void msleep(int ms){
    struct timespec ts;
    ts.tv_sec = ms/1000;
    ts.tv_nsec = (ms%1000)*1000000L;
    nanosleep(&ts,NULL);
}

// -----------------------------------------------------------------------------
// ring map / pop

static void ring_close(ring_t *r){
    if(r->hdr && r->hdr != MAP_FAILED){
        munmap(r->hdr, r->map_bytes);
    }
    if(r->memfd >= 0){
        close(r->memfd);
    }
    r->hdr      = NULL;
    r->map_bytes= 0;
    r->memfd    = -1;
}

static int ring_map_from_fd(ring_t *r, int fd){
    ring_close(r);
    r->memfd = fd;

    const size_t PROBE = 65536;
    phau_hdr_t *probe_hdr = mmap(NULL, PROBE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(probe_hdr == MAP_FAILED){
        fprintf(stderr,"[audiosink] mmap probe failed: %s\n", strerror(errno));
        close(fd);
        r->memfd = -1;
        return -1;
    }
    if(probe_hdr->magic != PHAU_MAGIC){
        fprintf(stderr,"[audiosink] bad magic in audio ring\n");
        munmap(probe_hdr, PROBE);
        close(fd);
        r->memfd = -1;
        return -1;
    }

    size_t need = sizeof(phau_hdr_t) + probe_hdr->capacity;
    munmap(probe_hdr, PROBE);

    phau_hdr_t *full = mmap(NULL, need, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(full == MAP_FAILED){
        fprintf(stderr,"[audiosink] mmap full failed: %s\n", strerror(errno));
        close(fd);
        r->memfd = -1;
        return -1;
    }

    r->hdr       = full;
    r->map_bytes = need;

    // align consumer cursor to producer so we start "live"
    atomic_store(&r->hdr->rpos, atomic_load(&r->hdr->wpos));

    fprintf(stderr,"[audiosink] ring mapped: cap=%uB rate=%.1f ch=%u fmt=%u\n",
            r->hdr->capacity,
            r->hdr->sample_rate,
            r->hdr->channels,
            r->hdr->fmt);

    return 0;
}

static size_t ring_pop_f32(float *dst, size_t max_frames){
    if(!g_ring.hdr) return 0;

    phau_hdr_t *h = g_ring.hdr;

    const size_t frame_bytes = (size_t)h->bytes_per_samp * (size_t)h->channels;
    uint64_t w = atomic_load(&h->wpos);
    uint64_t r = atomic_load(&h->rpos);

    if(w <= r){
        return 0;
    }

    size_t avail_bytes  = (size_t)(w - r);
    size_t avail_frames = avail_bytes / frame_bytes;
    if(avail_frames == 0) return 0;

    if(avail_frames > max_frames) avail_frames = max_frames;
    size_t bytes = avail_frames * frame_bytes;

    size_t cap = h->capacity;
    size_t rp  = (size_t)(r % cap);

    size_t first = bytes;
    if(rp + bytes > cap){
        first = cap - rp;
    }

    memcpy(dst, h->data + rp, first);

    if(bytes > first){
        memcpy(((uint8_t*)dst)+first, h->data, bytes-first);
    }

    uint64_t new_r = r + bytes;
    atomic_store(&h->rpos, new_r);

    return avail_frames;
}

// -----------------------------------------------------------------------------
// ALSA side

static void close_pcm(void){
    if(g_pcm){
        snd_pcm_drain(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
}

static int open_pcm(unsigned rate, unsigned ch){
    if(g_pcm){
        close_pcm();
    }

    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, g_alsa_dev, SND_PCM_STREAM_PLAYBACK, 0);
    if(rc < 0){
        fprintf(stderr,"[audiosink] snd_pcm_open(%s): %s\n",
                g_alsa_dev, snd_strerror(rc));
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, ch);

    unsigned rr = rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rr, 0);

    snd_pcm_uframes_t period = 480;  // ~10ms @48k
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);

    snd_pcm_uframes_t bufsize = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsize);

    rc = snd_pcm_hw_params(pcm, hw);
    if(rc < 0){
        fprintf(stderr,"[audiosink] hw_params: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        return -1;
    }

    g_pcm        = pcm;
    g_pcm_rate   = rr;
    g_pcm_ch     = ch;

    fprintf(stderr,"[audiosink] ALSA ready dev=%s rate=%u ch=%u period=%lu buf=%lu\n",
            g_alsa_dev, g_pcm_rate, g_pcm_ch,
            (unsigned long)period, (unsigned long)bufsize);

    return 0;
}

// -----------------------------------------------------------------------------
// Playback thread

static void *play_thread(void *arg){
    (void)arg;
    float framebuf[2048];

    while(atomic_load(&g_play_run)){
        if(!g_ring.hdr || !g_pcm){
            msleep(5);
            continue;
        }

        size_t nframes = ring_pop_f32(framebuf, sizeof(framebuf)/sizeof(framebuf[0]));
        if(nframes == 0){
            msleep(2);
            continue;
        }

        snd_pcm_sframes_t wrote = snd_pcm_writei(g_pcm, framebuf, nframes);
        if(wrote < 0){
            if(wrote == -EPIPE){
                snd_pcm_prepare(g_pcm);
                continue;
            }else if(wrote == -ESTRPIPE){
                while((wrote = snd_pcm_resume(g_pcm)) == -EAGAIN){
                    msleep(1);
                }
                if(wrote < 0){
                    snd_pcm_prepare(g_pcm);
                }
                continue;
            }else{
                fprintf(stderr,"[audiosink] writei error: %s\n", snd_strerror(wrote));
                msleep(5);
                continue;
            }
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// control plane helpers

static void publish_utf8(int fd, const char *feed, const char *msg){
    char js[512];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"%s\",\"encoding\":\"utf8\"}",
        feed, msg);
    if(n > 0){
        send_frame_json(fd, js, (size_t)n);
    }
}

static void create_feed(int fd, const char *feed){
    char js[256];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"create_feed\",\"feed\":\"%s\"}", feed);
    if(n > 0){
        send_frame_json(fd, js, (size_t)n);
    }
}

static void subscribe_feed_msg(int fd, const char *feed){
    char js[256];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"subscribe\",\"feed\":\"%s\"}", feed);
    if(n > 0){
        send_frame_json(fd, js, (size_t)n);
    }
}

// ask wfmd to resend its audio memfd
static void request_ring_from_wfmd(int fd){
    // NOTE: if your wfmd control feed is actually named something else
    // (ex: "wfmd.config.in" or "wfmd.ctrl.in"), update here.
    // Contract: wfmd on receiving "attach" must immediately republish
    // wfmd.audio-info with SCM_RIGHTS memfd.
    publish_utf8(fd, "wfmd.audio-ctl", "attach");
}

// clean copy of ALSA device token with trimming
static void copy_alsa_dev(const char *src){
    while(*src==' ' || *src=='\t'){
        src++;
    }

    char tmp[128];
    size_t w = 0;

    while(*src && w < sizeof(tmp)-1){
        unsigned char c = (unsigned char)*src++;
        if(c=='\r' || c=='\n') break;
        if(c==' '  || c=='\t') break;
        tmp[w++] = (char)c;
    }
    tmp[w] = '\0';

    if(w){
        memcpy(g_alsa_dev, tmp, w+1);
    }
}

// -----------------------------------------------------------------------------
// command parser for audiosink.config.in

static void handle_cmd(const char *cmd, int ctl_fd){
    if(strncmp(cmd,"device ",7)==0){
        const char *dev = cmd+7;
        copy_alsa_dev(dev);

        if(g_ring.hdr){
            open_pcm((unsigned)g_ring.hdr->sample_rate,
                     (unsigned)g_ring.hdr->channels);
        }else{
            open_pcm(g_pcm_rate, g_pcm_ch);
        }
        publish_utf8(ctl_fd, "audiosink.config.out", "ok device");
        return;
    }

    if(strncmp(cmd,"subscribe ",10)==0){
        const char *feed = cmd+10;
        if(*feed){
            // 1/ subscribe to wfmd.audio-info
            subscribe_feed_msg(ctl_fd, feed);

            // 2/ immediately ping wfmd to resend us the memfd
            request_ring_from_wfmd(ctl_fd);

            publish_utf8(ctl_fd, "audiosink.config.out", "ok subscribe");
        }else{
            publish_utf8(ctl_fd, "audiosink.config.out", "err subscribe arg");
        }
        return;
    }

    publish_utf8(ctl_fd, "audiosink.config.out", "err ?");
}

// -----------------------------------------------------------------------------
// cmd thread: UDS I/O + fd passing

static void *cmd_thread(void *arg){
    (void)arg;

    int fd = uds_connect(g_sock_path);
    if(fd < 0){
        fprintf(stderr,"[audiosink] uds_connect failed: %s\n", strerror(errno));
        return NULL;
    }

    create_feed(fd, "audiosink.config.out");
    subscribe_feed_msg(fd, "audiosink.config.in");

    char js[4096];

    while(atomic_load(&g_cmd_run)){
        int infd = -1;
        size_t nfds = 1;

        int n = recv_frame_json_with_fds(fd, js, sizeof js, &infd, &nfds, 50);
        if(n <= 0){
            continue;
        }

        char typ[32]={0};
        if(json_get_type(js, typ, sizeof typ)!=0){
            if(infd>=0) close(infd);
            continue;
        }

        char feed[128]={0};
        if(json_get_string(js,"feed",feed,sizeof feed)!=0){
            feed[0]='\0';
        }

        if(strcmp(typ,"publish")==0){
            // commands for us
            if(strcmp(feed,"audiosink.config.in")==0){
                char data[512]={0};
                if(json_get_string(js,"data",data,sizeof data)==0){
                    handle_cmd(data, fd);
                }
                if(infd>=0) close(infd);
                continue;
            }

            // wfmd.audio-info etc, carrying the memfd
            if(nfds==1 && infd>=0){
                if(ring_map_from_fd(&g_ring, infd)==0 && g_ring.hdr){
                    open_pcm((unsigned)g_ring.hdr->sample_rate,
                             (unsigned)g_ring.hdr->channels);
                }else{
                    close(infd);
                }
                continue;
            }

            if(infd>=0) close(infd);
        }else{
            if(infd>=0) close(infd);
        }
    }

    close(fd);
    return NULL;
}

// -----------------------------------------------------------------------------
// plugin ABI

const char* plugin_name(void){
    return "audiosink";
}

static const char *CONS[] = { "audiosink.config.in", NULL };
static const char *PROD[] = { "audiosink.config.out", NULL };

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    snprintf(g_sock_path, sizeof g_sock_path, "%s", ctx->sock_path);

    if(out){
        out->name     = "audiosink";
        out->version  = "0.3.0";
        out->consumes = CONS;
        out->produces = PROD;
    }
    return true;
}

bool plugin_start(void){
    atomic_store(&g_plugin_alive, true);

    g_play_run = true;
    pthread_create(&g_play_th, NULL, play_thread, NULL);

    g_cmd_run  = true;
    pthread_create(&g_cmd_th, NULL, cmd_thread, NULL);

    return true;
}

void plugin_stop(void){
    atomic_store(&g_plugin_alive, false);

    g_play_run = false;
    g_cmd_run  = false;

    pthread_join(g_play_th, NULL);
    pthread_join(g_cmd_th, NULL);

    close_pcm();
    ring_close(&g_ring);
}

