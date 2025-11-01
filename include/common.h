#ifndef COMMON_H
#define COMMON_H

#include "ph_uds_protocol.h"
#include <pthread.h>

// Simple dynamic array for ints
typedef struct {
    int *v;
    size_t n, cap;
} intvec_t;

void intvec_init(intvec_t *iv);
void intvec_push(intvec_t *iv, int x);
void intvec_erase(intvec_t *iv, size_t idx);
void intvec_free(intvec_t *iv);

// Feed model
typedef struct {
    char name[POC_MAX_FEED];
    intvec_t subs; // fds subscribed
} feed_t;

typedef struct {
    feed_t *v;
    size_t n, cap;
    pthread_mutex_t mu;
} feedtab_t;

void feedtab_init(feedtab_t *t);
void feedtab_free(feedtab_t *t);
int  feedtab_find(feedtab_t *t, const char *name);
int  feedtab_ensure(feedtab_t *t, const char *name);
void feedtab_sub(feedtab_t *t, const char *name, int fd);
void feedtab_unsub_all_fd(feedtab_t *t, int fd);
void feedtab_list(feedtab_t *t, int fd);

// JSON tiny helpers
int json_get_string(const char *json, const char *key, char *out, size_t outcap);
int json_get_type(const char *json, char *out, size_t outcap);

#endif
