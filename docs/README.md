# PhaseHound — Developer Docs

PhaseHound is a small, embeddable broker for wiring add‑ons together via a local **Unix domain socket (UDS)** control plane and an optional **shared‑memory (SHM)** data plane. It uses a tiny framed JSON protocol and a plugin ABI so add‑ons can be built as `.so` libraries.

This documentation set explains:
- how the core works,
- the wire protocol,
- shared memory patterns (why and how),
- how to author add‑ons correctly,
- how to build and debug, and
- recommended conventions and best practices for SDR‑style workloads.

> **Context:** PhaseHound targets high‑throughput SDR pipelines. Use **UDS for control**, **SHM for bulk IQ data** or other large payloads. FD passing (SCM_RIGHTS) is supported out of the box.

## Quick Start

```bash
# Build core, CLI and bundled add‑ons
make && make addons

# Start the core in one terminal
./ph-core

# In another terminal, interact via CLI (examples)
./ph-cli help
./ph-cli list feeds
./ph-cli available-addons
```

See **CLI.md** for a full tour and **ADDON_DEVELOPMENT.md** to write your first add‑on.

---

- **Default socket:** `/tmp/.PhaseHound-broker.sock`
- **Plugin ABI:** `1`
- **Max feed name:** 64 bytes
- **Max JSON frame:** 65536 bytes

## Repo Layout

```
/
  include/          # headers: protocol, plugin ABI, helpers
  src/              # core + common helpers
    addons/         # sample add‑ons (abracadabra, barbosa, dummy)
  tools/            # ph-cli
  Makefile          # top-level build (core + cli + addons)
```

See **ARCHITECTURE.md** and **PROTOCOL.md** for internals.
