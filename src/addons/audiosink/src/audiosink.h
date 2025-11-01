#ifndef AUDIOSINK_H
#define AUDIOSINK_H

#include <stdint.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>

// This struct MUST match the producer (wfmd).
// If wfmd already defines something equivalent in a header, you can delete
// this and include that instead. But right now we inline it so we compile.
typedef struct __attribute__((packed)) {
    uint32_t magic;          // producer sets some magic like 'PAU1'
    uint32_t sample_rate;    // Hz (e.g. 48000)
    uint32_t channels;       // 1 for mono
    uint32_t bytes_per_samp; // 4 for float32
    uint32_t fmt;            // 1 = float32 mono right now
    uint32_t ring_bytes;     // size of audio ring buffer in bytes
    uint32_t wr_idx;         // producer write cursor (mod ring_bytes)
    uint32_t rd_idx;         // consumer read cursor (we advance this)
} phau_hdr_t;

// runtime state for audiosink addon
typedef struct {
    // broker/IPC file descriptors etc. will live in here. We don't know all of
    // your broker types, so we just keep generic ints and strings.

    // feeds
    int feed_cfg_in;     // audiosink.config.in
    int feed_cfg_out;    // audiosink.config.out
    int feed_status;     // audiosink.status

    // subscription info for upstream audio feed
    int sub_feed_id;     // broker subscription handle / feed id
    int audio_fd;        // memfd received from wfmd
    phau_hdr_t *hdr;     // mmap'ed header
    uint8_t *ring;       // mmap'ed ring data
    size_t map_len;      // total mmap length

    // ALSA
    char device[128];    // "default", "hw:0,0", etc.
    snd_pcm_t *pcm;
    unsigned int pcm_rate;
    unsigned int pcm_channels;
    snd_pcm_format_t pcm_fmt;
    bool using_fallback;

    // stats
    uint32_t xruns;
    uint32_t frames_sent;
    uint32_t frames_lost;

    // run flag
    int running;
} audiosink_t;


// The core is expected to dlsym these 3:
int  addon_init(void);     // called once after dlopen
void addon_step(void);     // called repeatedly in core loop
void addon_fini(void);     // called before dlclose

#endif // AUDIOSINK_H

