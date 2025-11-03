# PhaseHound — Dummy Addon (Normalized ABI)

> **Status:** reference/demo addon • Last updated: 2025-11-03

This is a tiny, **self‑contained** addon that demonstrates PhaseHound’s *normalized* addon ABI:
a unified **control plane**, standard **feed naming**, and a predictable **lifecycle**. Use it as
a template when authoring new addons (e.g., demodulators, sinks, sources, analyzers).

## 1) What the dummy addon does

- Registers a minimal set of feeds and advertises them through the core.
- Implements a unified **control feed** for commands (`start/stop`, `subscribe/unsubscribe`, `help`, etc.).
- Emits a couple of demo publications (e.g. `ping` reply, `foo` messages) so you can verify pub/sub flows.
- Runs a background thread when `start`ed and shuts down cleanly on `stop`.

It is intentionally boring: the point is to show **how** to talk to the core, not to perform DSP.

## 2) Feeds (normalized names)

The normalized ABI standardizes feed naming to keep UX consistent across addons:

| Feed                         | Direction | Purpose                                                                 |
|-----------------------------|-----------|-------------------------------------------------------------------------|
| `dummy.config.in`           | **in**    | Control plane. Receives commands as UTF‑8 text or JSON (see §4).       |
| `dummy.config.out`          | **out**   | Control events, logs, and command responses (human‑readable text).     |
| `dummy.foo`                 | **out**   | Demo data feed. Publishes small payloads so you can test `sub`.        |

> Convention: each addon owns `«name».config.in/out` as its control channel pair.
> Data/stream feeds are free‑form but should use `«name».…` as a prefix.

In `plugin_register`, the addon declares its I/O in the descriptor:

```c
out->consumes = (const char*[]) { "dummy.config.in",  NULL };
out->produces = (const char*[]) { "dummy.config.out", "dummy.foo", NULL };
```

The core uses this to list feeds and validate `pub/sub` requests.

## 3) Lifecycle hooks (addon ABI)

Every addon implements the standard entry points:

```c
bool plugin_load(ph_core_api_t *api);       // capture core API, static init
bool plugin_register(ph_plugin_desc_t *out);// describe name, version, feeds
bool plugin_start(void);                     // spawn thread(s), begin work
void plugin_stop(void);                      // stop threads and join
void plugin_unload(void);                    // optional: release resources
```

The dummy addon holds an `atomic_int g_run` and a worker `pthread_t g_thr`.
`plugin_start()` flips `g_run=1` and launches `run(void*)`; `plugin_stop()` sets
`g_run=0` then `pthread_join()`s the worker for a clean shutdown.

## 4) Control plane (commands & message format)

### 4.1 Text commands (simple mode)

`dummy.config.in` accepts simple **space‑separated** commands (sufficient for CLIs):

```
help
ping [text]
start
stop
subscribe <feed>
unsubscribe <feed>
foo [payload]        # publish one demo message to dummy.foo
stats                # emit some internal counters to config.out
```

### 4.2 JSON control (advanced mode)

You may also send structured JSON. The addon uses the shared `ctrlmsg.h` helpers to
normalize I/O and logging. A minimal JSON command looks like:

```json
{ "cmd": "subscribe", "feed": "dummy.foo" }
```

The **core’s normalized envelope** for publications is:

```json
{ "type":"publish", "feed":"<name>", "data":"<payload>", "encoding":"utf8" }
```

You’ll see messages like these when interacting with the addon:

```json
{"type":"publish","feed":"dummy.config.out","data":"ok subscribe","encoding":"utf8"}
{"type":"publish","feed":"dummy.foo","data":"pong: hello","encoding":"utf8"}
```

> The same helpers are used throughout PhaseHound (see `ctrlmsg.h`), so your addon’s
> output will look identical to `soapy`, `wfmd`, `audiosink`, etc.

## 5) Try it quickly (with `ph-cli`)

> Assumes the **core** is already running and autoloading is configured, or you’ll load
> the addon manually.

```bash
# (Optional) Load the addon if not autoloaded
./ph-cli load addon dummy

# Get help
./ph-cli pub dummy.config.in "help"

# Subscribe to the demo feed before generating data
./ph-cli sub dummy.foo &

# Ping round‑trip (reply appears on dummy.config.out and/or dummy.foo)
./ph-cli pub dummy.config.in "ping hello"

# Start the background worker (if the addon has one)
./ph-cli pub dummy.config.in "start"

# Emit a demo message on dummy.foo
./ph-cli pub dummy.config.in "foo test‑payload"

# Show control‑plane chatter
./ph-cli sub dummy.config.out

# Stop the addon
./ph-cli pub dummy.config.in "stop"
```

Typical outputs:

```text
[dummy.config.out] ok subscribe
[dummy.foo] pong: hello
[dummy.config.out] started
[dummy.config.out] ok foo (len=12)
[dummy.config.out] stopped
```

> Tip: `./ph-cli sub A B C` supports **multiple feeds** in one process; each line is tagged
> with its source feed so you can tail several at once when debugging pipelines.

## 6) Building the dummy addon

From the repository root (or inside `src/addons/dummy/` if provided):

```bash
make -C src/addons/dummy
# or, from the top-level:
make addons
```

Artifacts:
- Shared object: `libdummy.so`
- Discovered via the core’s autoload path, or load manually: `./ph-cli load addon ./src/addons/dummy/libdummy.so`

## 7) Error handling & invariants (copy these to real addons)

- **Idempotent start/stop**: `start` when already running should be a no‑op with a polite notice.
- **Subscription sanity**: refuse `subscribe` to unknown feeds; emit `err unknown feed` on `config.out`.
- **Never block the control plane**: command handling must stay quick; heavier work belongs in the worker.
- **Consistent logging**: always report `ok/err <verb>` on `config.out` for human‑readable diagnostics.
- **Declare feeds truthfully** in `plugin_register`: the core and tools depend on this for UX.

## 8) Porting checklist (turn dummy → real addon)

- [ ] Rename feeds: `yourname.config.in/out`, plus your data feeds.
- [ ] Replace the `foo/ping` stubs with your real processing.
- [ ] Wire inputs via pub/sub or shared‑memory channels as needed (keep control on `*.config.*`).
- [ ] Expose runtime parameters as commands (e.g., `set sr=2400000`).
- [ ] Emit **structured** status on `config.out` (`json` preferred for machines, concise text for humans).
- [ ] Add unit-ish tests by scripting `ph-cli pub/sub` in a `tests/` folder.

## 9) Notes on the *normalized ABI* delta

You mentioned updating the normalization ABI. The dummy addon already reflects the
current model:

- **Unified control plane**: `*.config.in/out` is mandatory for all addons.
- **Standard lifecycle**: `plugin_load/register/start/stop` with a background worker pattern.
- **Feed discovery** via `plugin_register(out)`: the addon populates `out->consumes` and `out->produces`.
- **Subscribe/unsubscribe semantics** are handled within the addon using shared `ctrlmsg` helpers so
  every addon behaves the same from the CLI’s perspective.
- **Envelope format** for publications is stable (`type/feed/data/encoding`).

If you tweak the ABI further (fields in the descriptor, new encodings, error codes), mirror those in
`ctrlmsg.h` and update this README so downstream addons stay aligned.

## 10) Troubleshooting

- **I subbed but see nothing** → Send `foo` or `ping` or `start` (some addons only publish when running).
- **Unknown command** → Run `help` on `config.in`; the addon prints its supported verbs.
- **No such feed** → Check `./ph-cli list feeds` and verify `plugin_register` correctly advertises them.
- **Race on stop** → Ensure `plugin_stop` flips the run flag, then `join()`s the worker before unloading.

## 11) File layout (in this demo zip)

```
dummy/
├── Makefile
└── src/
    └── dummy.c     # Implements the normalized ABI and control plane
```

For a production addon, prefer:

```
src/addons/<name>/
├── include/<name>/<public>.h
├── src/<name>.c
├── Makefile (or is integrated in the top-level build)
└── README.md  ← this file
```
