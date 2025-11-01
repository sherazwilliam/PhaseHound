# Protocol

## Framing

Every message on the UDS is framed as:

```
[u32 length big-endian][UTF-8 JSON bytes]
```

Helpers: `send_frame_json`, `recv_frame_json` (see `include/ph_uds_protocol.h`).

## Core Types

Each JSON object must include a `type` field. The core understands these types:

- `"create_feed"`: `{"type":"create_feed","feed":"<name>"}`
- `"subscribe"`:   `{"type":"subscribe","feed":"<name>"}`
- `"unsubscribe"`: `{"type":"unsubscribe","feed":"<name>"}`
- `"publish"`:     `{"type":"publish","feed":"<name>","data":"<bytes>","encoding":"utf8|base64"}`
- `"command"`:     free-form command string (used by CLI), routed to `cli-control` feed.
- `"ping"` / `"pong"`

**Payload encoding**
- Small text → `encoding="utf8"` and put bytes in `data` (escape JSON quotes and backslashes).
- Binary / large payloads → prefer SHM (see below). If you must inline, use `encoding="base64"`.

**Feed limits**
- Max feed name length: **64**.
- Max JSON size per frame: **65536** (defensive cap).

## File-Descriptor Passing (SCM_RIGHTS)

For SHM, send a JSON envelope plus one or more fds in the same datagram:

```
send_frame_json_with_fds(int fd, const char* json, size_t len,
                         const int* fds, size_t nfds);
recv_frame_json_with_fds(...)
```

### Suggested SHM Descriptor

When passing a SHM fd, publish metadata alongside it on a feed that interested consumers subscribe to, e.g.:

```json
{
  "type": "publish",
  "feed": "iq.rx0.buffers",
  "subtype": "shm_map",
  "bytes": 1048576,
  "desc": "rx0 1 MiB ring",
  "mode": "rw",
  "layout": {
    "format": "ci16",        // complex int16 IQ
    "channels": 1,
    "stride": 0,             // 0 = tightly packed
    "offset": 0
  }
}
```

> The fd itself is attached using `SCM_RIGHTS`. Consumers `mmap()` it and coordinate via a small header or side-channel events (see **SHM_GUIDE.md**).

## Commands (via CLI)

The core exposes convenience commands over the `cli-control` feed. The CLI sends them for you:

```
    fprintf(stderr, "ph-cli usage:\n");
    fprintf(stderr, "  ph-cli help\n");
<text>\
<data>\
    fprintf(stderr, "  ph-cli sub <feed> [feed2 ...]\n");
    fprintf(stderr, "  ph-cli list feeds | list addons | available-addons\n");
    fprintf(stderr, "  ph-cli load addon <name|/path/to/lib.so>\n");
    fprintf(stderr, "  ph-cli unload addon <name>\n");
```

Responses are ordinary JSON messages (same framing), often echoed to stdout by `ph-cli`.

