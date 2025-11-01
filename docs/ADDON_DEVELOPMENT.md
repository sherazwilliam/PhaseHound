# Add‑on Development Guide

This guide demonstrates how to build a new add‑on (`.so`) that connects to the broker, declares its feeds, and communicates efficiently.

## ABI

Header: `include/plugin.h`

- `PLUGIN_ABI = 1`
- Functions you must export:
  - `const char* plugin_name(void);`
  - `bool plugin_init(const plugin_ctx_t* ctx, plugin_caps_t* out_caps);`
  - `bool plugin_start(void);` (spawn worker thread(s) that connect to `/tmp/.PhaseHound-broker.sock` and run your logic)
  - `void plugin_stop(void);` (signal threads and join)
- The core uses `dlopen`/`dlsym` and will refuse plugins not matching the ABI.

## Capabilities

`plugin_caps_t` is advisory, used for introspection:

```c
typedef struct plugin_caps {
  const char *name;       // plugin_name()
  const char *version;    // semantic version string
  const char *const *consumes; // feeds you subscribe to (NULL-terminated)
  const char *const *produces; // feeds you publish (NULL-terminated)
} plugin_caps_t;
```

Populate these in `plugin_init`. They are logged by the core on load.

## Minimal Skeleton

```c
#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>

static const char *g_sock = NULL;
static pthread_t g_thr;
static volatile int g_run = 0;

static void* run(void* _){
    int fd = uds_connect(g_sock);
    if(fd < 0) return NULL;

    // declare feeds and subscriptions
    send_frame_json(fd, "{\"type\":\"create_feed\",\"feed\":\"my.addon.out\"}", 49);
    send_frame_json(fd, "{\"type\":\"subscribe\",\"feed\":\"my.addon.in\"}", 49);

    char js[POC_MAX_JSON];
    g_run = 1;
    while(g_run){
        int got = recv_frame_json(fd, js, sizeof js, 200);
        if(got <= 0) continue;
        // handle messages, publish results
    }

    close(fd);
    return NULL;
}

const char* plugin_name(void){ return "myaddon"; }

bool plugin_init(const plugin_ctx_t* ctx, plugin_caps_t* out){
    if(!ctx || ctx->abi != PLUGIN_ABI) return false;
    g_sock = ctx->sock_path;
    if(out){
        static const char *cons[] = {"my.addon.in", NULL};
        static const char *prod[] = {"my.addon.out", NULL};
        out->name = plugin_name();
        out->version = "0.1.0";
        out->consumes = cons;
        out->produces = prod;
    }
    return true;
}

bool plugin_start(void){ return pthread_create(&g_thr, NULL, run, NULL) == 0; }
void plugin_stop(void){ g_run = 0; pthread_join(g_thr, NULL); }
```

See `src/addons/abracadabra` and `src/addons/barbosa` for compact working examples, and `src/addons/dummy` for SHM + config channel patterns.

## Build Template (Makefile)

```make
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -fPIC -pthread
LDFLAGS ?= -pthread
INCS = -I../../../include

NAME := myaddon
SO := lib$(NAME).so
SRCS_SO := src/$(NAME).c ../../../src/common.c

all: $(SO)
$(SO): $(SRCS_SO)
	$(CC) $(CFLAGS) $(INCS) -shared -o $@ $^ $(LDFLAGS) -ldl

clean:
	rm -f $(SO) $(SRCS_SO:.c=.o)
```

## Best Practices

- **Control vs data:** Use UDS for events/commands; use **SHM + fd passing** for buffers over ~64 KiB or sustained > few MB/s (typical SDR IQ).
- **Feeds:** Prefer dotted names: `rx0.iq`, `rx0.config.in`, `rx0.config.out`.
- **Config channel:** Mirror the `dummy` add‑on’s `*.config.in/out` interactive control model.
- **Escaping:** If sending UTF‑8 text, escape quotes/backslashes. For binary, use SHM or base64.
- **Threading:** Create a dedicated worker thread in `plugin_start` and join in `plugin_stop`.
- **Non‑blocking I/O:** Use timeouts in `recv_frame_json`, avoid busy loops, add small sleeps if idle.
- **Backpressure:** With SHM, implement a small header (`seq`, `used`, `capacity`) or ring indices, emit light notifications on a feed.
- **Cleanup:** Unsubscribe on disconnect handled by core; still close fds, unmap SHM, and stop threads gracefully.
- **Versioning:** Bump `PLUGIN_ABI` on breaking changes; add runtime checks in `plugin_init`.

