#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <time.h>

static const char *lvl_name(int lvl){
    switch(lvl){
        case LOG_DEBUG: return "DBG";
        case LOG_INFO:  return "INF";
        case LOG_WARN:  return "WRN";
        default:        return "ERR";
    }
}

void log_msg(log_level_t lvl, const char *fmt, ...){
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[64];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ld] %s: ", tbuf, ts.tv_nsec/1000000, lvl_name(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int set_nonblock(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int uds_listen_create(const char *path){
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0){ log_msg(LOG_ERROR, "socket: %s", strerror(errno)); return -1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    unlink(path);
    if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        log_msg(LOG_ERROR, "bind: %s", strerror(errno));
        close(fd); return -1;
    }
    if(listen(fd, 128) < 0){
        log_msg(LOG_ERROR, "listen: %s", strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

int uds_connect(const char *path){
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        close(fd); return -1;
    }
    return fd;
}

// ---- framing ----
static int read_full(int fd, void *buf, size_t len, int timeout_ms){
    size_t off = 0;
    while(off < len){
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 };
        int rc = select(fd+1, &r, NULL, NULL, timeout_ms>=0? &tv: NULL);
        if(rc <= 0){ if(errno==EINTR) continue; return -1; }
        ssize_t got = read(fd, (char*)buf+off, len-off);
        if(got <= 0) return -1;
        off += (size_t)got;
    }
    return 0;
}

int send_frame_json(int fd, const char *json_str, size_t len){
    uint32_t be = htonl((uint32_t)len);
    if(write(fd, &be, 4) != 4) return -1;
    size_t off = 0;
    while(off < len){
        ssize_t w = write(fd, json_str+off, len-off);
        if(w <= 0){ if(errno==EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

int recv_frame_json(int fd, char *buf, size_t bufcap, int timeout_ms){
    uint32_t be;
    if(read_full(fd, &be, 4, timeout_ms) < 0) return -1;
    uint32_t len = ntohl(be);
    if(len >= bufcap) return -1;
    if(read_full(fd, buf, len, timeout_ms) < 0) return -1;
    buf[len] = '\0';
    return (int)len;
}

// ---- tiny dynamic array ----

void intvec_init(intvec_t *iv){ iv->v=NULL; iv->n=iv->cap=0; }
void intvec_push(intvec_t *iv, int x){
    if(iv->n == iv->cap){
        size_t nc = iv->cap? iv->cap*2: 8;
        iv->v = (int*)realloc(iv->v, nc*sizeof(int));
        iv->cap = nc;
    }
    iv->v[iv->n++] = x;
}
void intvec_erase(intvec_t *iv, size_t idx){
    if(idx >= iv->n) return;
    for(size_t i=idx+1;i<iv->n;i++) iv->v[i-1]=iv->v[i];
    iv->n--;
}
void intvec_free(intvec_t *iv){
    free(iv->v); iv->v=NULL; iv->n=iv->cap=0;
}

// ---- feeds ----
#include <pthread.h>
static void feed_init(feed_t *f){ f->name[0]='\0'; intvec_init(&f->subs); }

void feedtab_init(feedtab_t *t){
    t->v=NULL; t->n=t->cap=0;
    pthread_mutex_init(&t->mu, NULL);
}
void feedtab_free(feedtab_t *t){
    for(size_t i=0;i<t->n;i++){ intvec_free(&t->v[i].subs); }
    free(t->v);
    pthread_mutex_destroy(&t->mu);
}
static int feedtab_find_nolock(feedtab_t *t, const char *name){
    for(size_t i=0;i<t->n;i++) if(strcmp(t->v[i].name, name)==0) return (int)i;
    return -1;
}
int feedtab_find(feedtab_t *t, const char *name){
    pthread_mutex_lock(&t->mu);
    int idx = feedtab_find_nolock(t, name);
    pthread_mutex_unlock(&t->mu);
    return idx;
}
int feedtab_ensure(feedtab_t *t, const char *name){
    pthread_mutex_lock(&t->mu);
    int idx = feedtab_find_nolock(t, name);
    if(idx>=0){ pthread_mutex_unlock(&t->mu); return idx; }
    if(t->n == t->cap){
        size_t nc = t->cap? t->cap*2: 8;
        t->v = (feed_t*)realloc(t->v, nc*sizeof(feed_t));
        t->cap = nc;
    }
    feed_init(&t->v[t->n]);
    strncpy(t->v[t->n].name, name, sizeof(t->v[t->n].name)-1);
    idx = (int)t->n;
    t->n++;
    pthread_mutex_unlock(&t->mu);
    log_msg(LOG_INFO, "feed created: %s", name);
    return idx;
}
void feedtab_sub(feedtab_t *t, const char *name, int fd){
    int idx = feedtab_ensure(t, name);
    pthread_mutex_lock(&t->mu);
    // prevent duplicates
    for(size_t i=0;i<t->v[idx].subs.n;i++) if(t->v[idx].subs.v[i]==fd){ pthread_mutex_unlock(&t->mu); return; }
    intvec_push(&t->v[idx].subs, fd);
    pthread_mutex_unlock(&t->mu);
    log_msg(LOG_INFO, "fd=%d subscribed to %s", fd, name);
}
void feedtab_unsub_all_fd(feedtab_t *t, int fd){
    pthread_mutex_lock(&t->mu);
    for(size_t i=0;i<t->n;i++){
        for(size_t j=0;j<t->v[i].subs.n;){
            if(t->v[i].subs.v[j]==fd) intvec_erase(&t->v[i].subs, j);
            else j++;
        }
    }
    pthread_mutex_unlock(&t->mu);
}
void feedtab_list(feedtab_t *t, int fd){
    char buf[POC_MAX_JSON];
    pthread_mutex_lock(&t->mu);
    for(size_t i=0;i<t->n;i++){
        int len = snprintf(buf, sizeof buf, "{\"type\":\"info\",\"feed\":\"%s\",\"subs\":%zu}", t->v[i].name, t->v[i].subs.n);
        send_frame_json(fd, buf, (size_t)len);
    }
    pthread_mutex_unlock(&t->mu);
}

// ---- minimal JSON readers (very naive; suitable for PoC) ----
static int find_key(const char *json, const char *key, const char **val, size_t *vlen){
    const char *p = strstr(json, key);
    if(!p) return -1;
    p = strchr(p, ':');
    if(!p) return -1;
    p++;
    // skip spaces and optional quotes
    while(*p==' '||*p=='\t') p++;
    if(*p=='\"'){
        p++;
        const char *q = strchr(p, '\"');
        if(!q) return -1;
        *val = p; *vlen = (size_t)(q-p);
        return 0;
    } else {
        const char *q = p;
        while(*q && *q!=',' && *q!='}' && *q!='\n') q++;
        *val = p; *vlen = (size_t)(q-p);
        return 0;
    }
}

int json_get_string(const char *json, const char *key, char *out, size_t outcap){
    const char *v; size_t n;
    if(find_key(json, key, &v, &n)<0) return -1;
    if(n+1>outcap) n = outcap-1;
    memcpy(out, v, n); out[n]='\0';
    return 0;
}

int json_get_type(const char *json, char *out, size_t outcap){
    return json_get_string(json, "type", out, outcap);
}

// ---- base64 ----
static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_rev(char c){
    if('A'<=c && c<='Z') return c-'A';
    if('a'<=c && c<='z') return c-'a'+26;
    if('0'<=c && c<='9') return c-'0'+52;
    if(c=='+') return 62;
    if(c=='/') return 63;
    if(c=='=') return -2; // padding
    return -1;
}
size_t b64_encoded_len(size_t bin_len){ return ((bin_len + 2) / 3) * 4; }
size_t b64_decoded_maxlen(size_t b64_len){ return (b64_len/4)*3; }

int b64_encode(const uint8_t *in, size_t inlen, char *out, size_t *outlen){
    size_t olen = b64_encoded_len(inlen);
    *outlen = olen;
    size_t j=0;
    for(size_t i=0;i<inlen;i+=3){
        uint32_t v = in[i] << 16;
        if(i+1<inlen) v |= in[i+1] << 8;
        if(i+2<inlen) v |= in[i+2];
        out[j++] = B64[(v>>18)&63];
        out[j++] = B64[(v>>12)&63];
        out[j++] = (i+1<inlen)? B64[(v>>6)&63] : '=';
        out[j++] = (i+2<inlen)? B64[v&63] : '=';
    }
    return 0;
}

int b64_decode(const char *in, size_t inlen, uint8_t *out, size_t *outlen){
    size_t j=0;
    int quad[4]; int qi=0;
    for(size_t i=0;i<inlen;i++){
        int r = b64_rev(in[i]);
        if(r==-1) continue; // skip whitespace/others
        quad[qi++] = r;
        if(qi==4){
            uint32_t v = 0;
            int pad = 0;
            for(int k=0;k<4;k++){
                if(quad[k]==-2){ quad[k]=0; pad++; }
                v = (v<<6) | (uint32_t)quad[k];
            }
            out[j++] = (v>>16)&0xFF;
            if(pad<2) out[j++] = (v>>8)&0xFF;
            if(pad<1) out[j++] = v&0xFF;
            qi=0;
        }
    }
    *outlen = j;
    return 0;
}

#include <sys/uio.h>

int send_frame_json_with_fds(int fd, const char *json_str, size_t len, const int *fds, size_t nfds){
    if(!fds || nfds==0){
        return send_frame_json(fd, json_str, len);
    }
    if(nfds > 16) nfds = 16;

    /* 1) Send 4-byte big-endian length first (separate write loop). */
    uint32_t be = htonl((uint32_t)len);
    size_t off = 0; const uint8_t *pb = (const uint8_t*)&be;
    while(off < 4){
        ssize_t w = write(fd, pb + off, 4 - off);
        if(w <= 0){ if(errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }

    /* 2) Send JSON bytes + SCM_RIGHTS via one sendmsg(). */
    struct iovec iov = { .iov_base = (void*)json_str, .iov_len = len };
    char cbuf[CMSG_SPACE(sizeof(int)*16)];
    struct msghdr msg; memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = CMSG_SPACE(sizeof(int)*nfds);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int)*nfds);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int)*nfds);

    ssize_t s = sendmsg(fd, &msg, 0);
    return (s < 0) ? -1 : 0;

}

static int recv_all_timeout(int fd, void *buf, size_t len, int timeout_ms){
    uint8_t *p=(uint8_t*)buf; size_t off=0;
    while(off<len){
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd,&rfds);
        struct timeval tv={ .tv_sec=timeout_ms/1000, .tv_usec=(timeout_ms%1000)*1000 };
        int r = select(fd+1, &rfds, NULL, NULL, timeout_ms<0?NULL:&tv);
        if(r==0) return -1;
        if(r<0){ if(errno==EINTR) continue; return -1; }
        ssize_t g = recv(fd, p+off, len-off, 0);
        if(g<=0) return -1;
        off += (size_t)g;
    }
    return 0;
}

int recv_frame_json_with_fds(int fd, char *json_out, size_t outcap, int *fds_out, size_t *nfds_inout, int timeout_ms){
    uint32_t be = 0;
    if(recv_all_timeout(fd, &be, 4, timeout_ms)<0) return -1;
    uint32_t len = ntohl(be);
    if(len >= outcap) return -1;
    struct iovec iov = { .iov_base = json_out, .iov_len = len };
    char cbuf[CMSG_SPACE(sizeof(int)*16)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
    ssize_t g = recvmsg(fd, &msg, MSG_WAITALL);
    if(g != (ssize_t)len) return -1;
    json_out[len] = 0;
    size_t capfds = nfds_inout? *nfds_inout : 0;
    size_t gotfds = 0;
    if(fds_out && capfds){
        for(struct cmsghdr *cmsg=CMSG_FIRSTHDR(&msg); cmsg; cmsg=CMSG_NXTHDR(&msg,cmsg)){
            if(cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SCM_RIGHTS){
                size_t cnt = (cmsg->cmsg_len - CMSG_LEN(0))/sizeof(int);
                int *arr = (int*)CMSG_DATA(cmsg);
                for(size_t i=0;i<cnt && gotfds<capfds;i++){
                    fds_out[gotfds++] = arr[i];
                }
            }
        }
    }
    if(nfds_inout) *nfds_inout = gotfds;
    return (int)len;
}
