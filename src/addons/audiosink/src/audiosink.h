#pragma once
#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ph_stream.h"

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
