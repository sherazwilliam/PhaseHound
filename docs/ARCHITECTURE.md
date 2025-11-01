# Architecture

PhaseHound consists of:

1. **Core broker (`ph-core`)** — accepts UDS clients, routes messages by *feed*.
2. **CLI (`ph-cli`)** — a convenience client for publishing commands and inspecting state.
3. **Add‑ons** — shared libraries (`.so`) loaded by the core (autoload or on command) that connect back to the broker to publish/subscribe and exchange metadata.

```
+-----------+         UDS JSON frames            +-----------+
|  Add-on A |  <------------------------------>  |  ph-core  |
+-----------+                                     +-----------+
      |                                                 ^
      v                                                 |
+-----------+                                           |
|  Add-on B |  <----------------------------------------+
+-----------+

Large buffers: SHM fd passed once via UDS (SCM_RIGHTS) + small JSON metadata.
```

## Control Plane

- **Transport:** Unix domain socket at `/tmp/.PhaseHound-broker.sock` (compile-time macro `PH_SOCK_PATH`).
- **Framing:** `[u32 length BE][JSON payload]`.
- **Routing:** Each JSON event names a `feed`; subscribers to that feed receive the event unchanged.
- **Core loop:** `select()` over the listen socket and all client fds; on disconnect, subscriptions from that fd are removed.

## Data Plane (for SDR payloads)

- UDS is kept small and predictable (control messages).
- For high-rate IQ or wideband payloads, allocate a **shared-memory** region and **pass the fd once** using `SCM_RIGHTS`.
- The sample `dummy` add‑on demonstrates `memfd_create`/`shm_open` fallback and publishes a small JSON descriptor alongside the passed fd.

## Plugins (Add‑ons)

- Shared libraries with four exported functions:
  - `const char* plugin_name(void);`
  - `bool plugin_init(const plugin_ctx_t*, plugin_caps_t* out_caps);`
  - `bool plugin_start(void);`
  - `void plugin_stop(void);`
- The core loads them (autoload on startup or via CLI `load addon <name>`), calls `init` with `PLUGIN_ABI=1` and the broker socket path, then `start` (typically spawns a thread), and later `stop`.
- See **ADDON_DEVELOPMENT.md** for the full guide.

