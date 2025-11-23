#pragma once
// PhaseHound — common stream/ring conventions
// - Stream "kind" (IQ, audio, frames, events, log)
// - Encoding enum (cf32, s16, hex, json, utf8, …)
// - Standard headers for IQ and audio rings
//
// This header is meant to be shared by producer/consumer addons:
//  - soapy (IQ producer)
//  - wfmd (IQ consumer + audio producer)
//  - audiosink (audio consumer)
//  - future filesink/filesource, ADS-B, TPMS, …

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- generic stream kind / encoding (for future metadata) ---- */

typedef enum {
    PH_STREAM_KIND_UNKNOWN = 0,
    PH_STREAM_KIND_IQ      = 1,
    PH_STREAM_KIND_AUDIO   = 2,
    PH_STREAM_KIND_FRAMES  = 3,  /* raw frames (hex, etc.)   */
    PH_STREAM_KIND_EVENTS  = 4,  /* parsed events/metadata   */
    PH_STREAM_KIND_LOG     = 5   /* human-readable log text  */
} ph_stream_kind_t;

typedef enum {
    PH_STREAM_ENCODING_UNKNOWN = 0,
    PH_STREAM_ENCODING_CF32    = 1,  /* complex float32 IQ   */
    PH_STREAM_ENCODING_CS16    = 2,  /* complex int16 IQ     */
    PH_STREAM_ENCODING_F32     = 3,  /* scalar float32 PCM   */
    PH_STREAM_ENCODING_S16     = 4,  /* scalar int16 PCM     */
    PH_STREAM_ENCODING_HEX     = 5,  /* ASCII hex frames     */
    PH_STREAM_ENCODING_JSON    = 6,  /* JSON objects         */
    PH_STREAM_ENCODING_UTF8    = 7   /* plain UTF-8 text     */
} ph_stream_encoding_t;

/* ---- IQ ring v0 ----------------------------------------------------- */

#define PH_PROTO_IQ_RING "phasehound.iq-ring.v0"

enum {
    PHIQ_MAGIC   = 0x51494850u, /* 'P''H''I''Q' */
    PHIQ_VERSION = 1u
};

typedef enum {
    PHIQ_FMT_CF32 = 1, /* interleaved I,Q float32 (8 bytes/frame) */
    PHIQ_FMT_CS16 = 2  /* interleaved I,Q int16  (4 bytes/frame) */
} phiq_fmt_t;

typedef struct {
    uint32_t magic;           /* PHIQ_MAGIC */
    uint32_t version;         /* PHIQ_VERSION */
    _Atomic uint64_t seq;     /* increments per write */
    _Atomic uint64_t wpos;    /* ABSOLUTE bytes written */
    _Atomic uint64_t rpos;    /* ABSOLUTE bytes read (by consumer) */
    uint32_t capacity;        /* ring size in bytes (data[]) */
    uint32_t used;            /* optional mirror; producer-owned */
    uint32_t bytes_per_samp;  /* bytes per COMPLEX frame (I+Q together) */
    uint32_t channels;        /* complex streams: usually 1 */
    double   sample_rate;     /* Hz */
    double   center_freq;     /* Hz */
    uint32_t fmt;             /* phiq_fmt_t */
    uint8_t  reserved[64];
    uint8_t  data[];
} phiq_hdr_t;

/* ---- Audio ring v0 -------------------------------------------------- */

#define PH_PROTO_AUDIO_RING "phasehound.audio-ring.v0"

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
    uint32_t magic, version;  /* PHAU_MAGIC / PHAU_VER */
    _Atomic uint64_t seq;
    _Atomic uint64_t wpos;
    _Atomic uint64_t rpos;
    uint32_t capacity;        /* bytes in data[] */
    uint32_t used;            /* producer-written bytes (may be <=capacity) */
    uint32_t bytes_per_samp;  /* e.g. 4 for f32 */
    uint32_t channels;        /* 1 mono, 2 stereo */
    double   sample_rate;     /* Hz */
    uint32_t fmt;             /* PHAU_FMT_* */
    uint8_t  reserved[64];
    uint8_t  data[];
} phau_hdr_t;

#ifdef __cplusplus
}
#endif
