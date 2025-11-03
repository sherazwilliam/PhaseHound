#include "ctrlmsg.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ---- low-level emitters ---- */

void ph_create_feed(int fd, const char *feed) {
    char js[256];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"create_feed\",\"feed\":\"%s\"}", feed);
    if (n > 0) send_frame_json(fd, js, (size_t)n);
}

void ph_subscribe(int fd, const char *feed) {
    char js[256];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"subscribe\",\"feed\":\"%s\"}", feed);
    if (n > 0) send_frame_json(fd, js, (size_t)n);
}

void ph_unsubscribe(int fd, const char *feed) {
    char js[256];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"unsubscribe\",\"feed\":\"%s\"}", feed);
    if (n > 0) send_frame_json(fd, js, (size_t)n);
}

void ph_publish(int fd, const char *feed, const char *data_json) {
    char js[1024];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":%s}", feed, data_json);
    if (n > 0) send_frame_json(fd, js, (size_t)n);
}

void ph_command(int fd, const char *feed, const char *cmd) {
    char js[768];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"command\",\"feed\":\"%s\",\"data\":\"%s\"}", feed, cmd);
    if (n > 0) send_frame_json(fd, js, (size_t)n);
}

/* ---- control context ---- */

void ph_ctrl_init(ph_ctrl_t *c, int fd, const char *addon_name) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    snprintf(c->name, sizeof c->name, "%s", addon_name);
    snprintf(c->feed_in,  sizeof c->feed_in,  "%s.config.in",  addon_name);
    snprintf(c->feed_out, sizeof c->feed_out, "%s.config.out", addon_name);
}

void ph_ctrl_advertise(ph_ctrl_t *c) {
    ph_create_feed(c->fd, c->feed_in);
    ph_create_feed(c->fd, c->feed_out);
    ph_subscribe(c->fd,   c->feed_in);
}

/* ---- replies ---- */

void ph_reply(ph_ctrl_t *c, const char *json_obj) {
    /* json_obj must start with { and end with } */
    ph_publish(c->fd, c->feed_out, json_obj);
}

void ph_reply_ok(ph_ctrl_t *c, const char *msg) {
    char js[512];
    snprintf(js, sizeof js, "{\"ok\":true,\"msg\":\"%s\"}", msg ? msg : "ok");
    ph_publish(c->fd, c->feed_out, js);
}

void ph_reply_err(ph_ctrl_t *c, const char *msg) {
    char js[512];
    snprintf(js, sizeof js, "{\"ok\":false,\"err\":\"%s\"}", msg ? msg : "err");
    ph_publish(c->fd, c->feed_out, js);
}

static void vreply_fmt(ph_ctrl_t *c, bool ok, const char *fmt, va_list ap) {
    char payload[768];
    vsnprintf(payload, sizeof payload, fmt ? fmt : "", ap);
    char js[900];
    snprintf(js, sizeof js, "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true":"false", payload);
    ph_publish(c->fd, c->feed_out, js);
}

void ph_reply_okf(ph_ctrl_t *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vreply_fmt(c, true,  fmt, ap); va_end(ap);
}

void ph_reply_errf(ph_ctrl_t *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vreply_fmt(c, false, fmt, ap); va_end(ap);
}

/* ---- dispatch ----
   Expects frames shaped like:
   {"type":"command","feed":"<name>.config.in","data":"<cmdline>"}
*/
static const char *json_get(const char *js, const char *key, char *out, size_t outsz) {
    /* super-lightweight extractor to avoid pulling a full JSON lib here */
    const char *p = strstr(js, key);
    if (!p) return NULL;
    p = strchr(p, ':'); if (!p) return NULL; p++;
    while (*p==' '||*p=='\"') { if (*p=='\"') { p++; break; } p++; }
    size_t i=0;
    while (*p && *p!='\"' && *p!='\n' && *p!='\r' && i+1<outsz) out[i++]=*p++;
    out[i]='\0';
    return out;
}

bool ph_ctrl_dispatch(ph_ctrl_t *c, const char *json, size_t n,
                      void (*on_cmd)(ph_ctrl_t *c, const char *cmdline, void *user),
                      void *user)
{
    (void)n;
    char type[32]={0}, feed[128]={0}, data[1024]={0};

    if (!json_get(json, "\"type\"", type, sizeof type)) return false;
    if (!json_get(json, "\"feed\"", feed, sizeof feed)) return false;
    if (strcmp(feed, c->feed_in)!=0) return false;

    /* Accept either:
       - {"type":"command","feed":"...config.in","data":"<cmdline>"}
       - {"type":"publish","feed":"...config.in","data":"<cmdline>"}  (from: ph-cli pub ...)
    */
    if (strcmp(type,"command")==0 || strcmp(type,"publish")==0) {
        if (!json_get(json, "\"data\"", data, sizeof data)) data[0]='\0';
        if (on_cmd) on_cmd(c, data, user);
        return true;
    }
    return false;
}