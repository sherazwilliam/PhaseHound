#pragma once
#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* PhaseHound Audio ring header (producer: wfmd/dmr) */
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
    uint32_t capacity;        /* bytes in data[] */
    uint32_t used;            /* producer-written bytes (may be <=capacity) */
    uint32_t bytes_per_samp;  /* e.g. 4 for f32 */
    uint32_t channels;        /* 1 mono, 2 stereo */
    double   sample_rate;     /* Hz */
    uint32_t fmt;             /* 1=f32le */
    uint8_t  reserved[64];
    uint8_t  data[];
} phau_hdr_t;

/* Runtime state for audiosink */
typedef struct {
    /* broker */
    int fd;                   /* UDS to core */
    char name[32];            /* "audiosink" */
    char feed_in[64];         /* "audiosink.config.in"  */
    char feed_out[64];        /* "audiosink.config.out" */

    /* subscription */
    char current_feed[128];   /* e.g. "wfmd.audio-info" */

    /* memfd ring */
    int         memfd;        /* -1 if none */
    phau_hdr_t *hdr;          /* mmap base */
    size_t      map_bytes;    /* mmap length */

    /* ALSA */
    char        alsa_dev[128];/* "default" or "hw:0,0" */
    snd_pcm_t  *pcm;
    unsigned    pcm_rate;
    unsigned    pcm_ch;

    /* threads */
    _Atomic bool play_run;
    _Atomic bool cmd_run;
    pthread_t   th_play;
    pthread_t   th_cmd;

    /* misc */
    _Atomic bool started;
} audiosink_t;

/* ring */
int  au_ring_map_from_fd(audiosink_t *s, int fd);
void au_ring_close(audiosink_t *s);
size_t au_ring_pop_f32(audiosink_t *s, float *dst, size_t max_frames);

/* alsa */
int  au_pcm_open(audiosink_t *s, unsigned rate, unsigned ch);
void au_pcm_close(audiosink_t *s);
