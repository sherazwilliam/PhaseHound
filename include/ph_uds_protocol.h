#ifndef PH_UDS_PROTOCOL_H
#define PH_UDS_PROTOCOL_H

// C11
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Simple framed protocol over UDS:
// [u32 length BE][JSON UTF-8 payload bytes]
// The JSON object always has a "type" field.
// Types: "publish", "subscribe", "create_feed", "unsubscribe", "command", "pong", "ping".
// Example publish:
// {"type":"publish","feed":"A-in","data":"hello bytes base64 or utf8","encoding":"utf8"}
// For binary payloads use encoding="base64".
// The server never alters payloads, only routes them based on "feed".

#define PH_SOCK_PATH "/tmp/.PhaseHound-broker.sock"

// Max sizes (defensive). Adjust for your use-case.
enum { POC_MAX_FEED = 64, POC_MAX_JSON = 65536 };

// Minimal logging levels
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

// Utilities (implemented in common.c)
void log_msg(log_level_t lvl, const char *fmt, ...);
int  set_nonblock(int fd);
int  uds_listen_create(const char *path);
int  uds_connect(const char *path);

// Framing helpers
int  send_frame_json(int fd, const char *json_str, size_t len);
int  recv_frame_json(int fd, char *buf, size_t bufcap, int timeout_ms);

// Base64 helpers (URL-safe = false, standard alphabet)
size_t b64_encoded_len(size_t bin_len);
size_t b64_decoded_maxlen(size_t b64_len);
int    b64_encode(const uint8_t *in, size_t inlen, char *out, size_t *outlen);
int    b64_decode(const char *in, size_t inlen, uint8_t *out, size_t *outlen);

#endif // POC_PROTOCOL_H


// --- FD-passing (SCM_RIGHTS) helpers ---
int send_frame_json_with_fds(int fd, const char *json_str, size_t len, const int *fds, size_t nfds);
int recv_frame_json_with_fds(int fd, char *json_out, size_t outcap, int *fds_out, size_t *nfds_inout, int timeout_ms);
