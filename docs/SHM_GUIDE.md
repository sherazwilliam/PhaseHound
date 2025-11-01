# Shared Memory Guide (for SDR workloads)

High‑rate SDR pipelines demand bandwidth and low latency that exceed UDS comfort zones. PhaseHound uses **UDS for control** and **SHM for bulk data**.

## Creating SHM

Use `memfd_create` (Linux) with fallback to `shm_open`. See `src/addons/dummy/src/dummy.c`:

- Create fd
- `ftruncate` to desired size
- Optionally write a small header with atomics for coordination
- `mmap` in producer and consumers
- Pass the fd once via `send_frame_json_with_fds` alongside a small JSON descriptor

### Example (producer side)

```c
int sfd = memfd_create("ph-buffer", MFD_CLOEXEC);
ftruncate(sfd, 1<<20); // 1 MiB
// mmap, write header/data...
int fds[1] = { sfd };
char js[256];
int len = snprintf(js, sizeof js,
  "{\"type\":\"publish\",\"feed\":\"iq.rx0.buffers\",\"subtype\":\"shm_map\",\"bytes\":%u}",
  1<<20);
send_frame_json_with_fds(ctrl_fd, js, (size_t)len, fds, 1);
```

### Coordination

A simple header works well:

```c
struct shm_hdr {
  uint32_t magic, version;
  _Atomic uint64_t seq;
  uint32_t used, capacity;
  uint8_t data[];
};
```

- **seq**: monotonic counter incremented on each buffer update
- **used/capacity**: region size information
- Optionally extend with ring indices (`head/tail`), timestamps, sample format, etc.

Consumers subscribe to an event feed (e.g., `iq.rx0.events`) for lightweight notifications like `{"subtype":"shm_ready","seq":42,"bytes":1048576}`.

## When to prefer SHM

- Continuous streams above a few MB/s
- Frames larger than ~64 KiB
- Multiple consumers reading the same data

## Do not copy more than needed

- Avoid re‑encoding or base64 for bulk paths
- Keep JSON envelopes small and infrequent (control only)
