# FAQ

**Why UDS for control and not TCP?**
Local pipelines keep deployment simple and fast; UDS are efficient and support fd passing.

**When should I pick SHM?**
Any sustained high‑bandwidth payloads (SDR IQ, raw frames, image bursts). Keep JSON envelopes tiny.

**Do I need base64?**
Prefer SHM. Base64 is fine for small diagnostic messages or interop with simple tools.
