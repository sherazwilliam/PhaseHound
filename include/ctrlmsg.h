#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward decl: your framing I/O helpers */
int send_frame_json(int fd, const char *buf, size_t n);

/* Simple control context every addon keeps */
typedef struct {
    int fd;                 /* connected UDS fd to broker */
    char name[64];          /* addon short name: e.g., "wfmd" */
    char feed_in[96];       /* "<name>.config.in"  */
    char feed_out[96];      /* "<name>.config.out" */
} ph_ctrl_t;

/* Lifecycle for the control plane */
void ph_ctrl_init(ph_ctrl_t *c, int fd, const char *addon_name);
/* Create feeds and subscribe to feed_in (call once after init) */
void ph_ctrl_advertise(ph_ctrl_t *c);

/* Helpers to send control-plane messages */
void ph_create_feed(int fd, const char *feed);
void ph_subscribe (int fd, const char *feed);
void ph_unsubscribe(int fd, const char *feed);
void ph_publish   (int fd, const char *feed, const char *data_json);
void ph_command   (int fd, const char *feed, const char *cmd);

/* Reply helpers (to feed_out) */
void ph_reply      (ph_ctrl_t *c, const char *json_obj);                  /* must be valid JSON object text */
void ph_reply_ok   (ph_ctrl_t *c, const char *msg);
void ph_reply_err  (ph_ctrl_t *c, const char *msg);
void ph_reply_okf  (ph_ctrl_t *c, const char *fmt, ...);
void ph_reply_errf (ph_ctrl_t *c, const char *fmt, ...);

/* Dispatch a single incoming JSON frame.
   Returns true if consumed (it was a command to this addon). */
bool ph_ctrl_dispatch(ph_ctrl_t *c, const char *json, size_t n,
                      void (*on_cmd)(ph_ctrl_t *c, const char *cmdline, void *user),
                      void *user);
