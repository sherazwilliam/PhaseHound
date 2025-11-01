#ifndef PLUGIN_H
#define PLUGIN_H
#include <stdbool.h>
#include <stdint.h>

#define PLUGIN_ABI 1

typedef struct plugin_ctx {
    uint32_t abi;
    const char *sock_path;
    const char *name;
} plugin_ctx_t;

typedef struct plugin_caps {
    const char *name;
    const char *version;
    const char *const *consumes;
    const char *const *produces;
} plugin_caps_t;

typedef const char* (*plugin_name_fn)(void);
typedef bool (*plugin_init_fn)(const plugin_ctx_t*, plugin_caps_t* out_caps);
typedef bool (*plugin_start_fn)(void);
typedef void (*plugin_stop_fn)(void);

#endif
