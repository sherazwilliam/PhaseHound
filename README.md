# PhaseHound  
Lightweight modular SDR runtime in pure C


## What is PhaseHound?

PhaseHound is a modular SDR core designed for people who actually care about RF, latency, and control — not just pretty GUIs.

- **Core daemon (`ph-core`)**
  A broker that manages local "feeds", routes messages, and glues modules together. Think lightweight signal bus.

- **Runtime-loadable add-ons (.so plugins)**
  Each add-on can publish IQ streams, demodulate, decode, sink audio, log metadata, etc. Add-ons register feeds and talk to the core at runtime. You can load/unload them without restarting.

- **CLI (`ph-cli`)**
  A simple control tool that speaks the core protocol over a Unix domain socket. You can:
  - list addons / feeds
  - subscribe to live data
  - send commands to addons
  - load/unload addons

The goal: a clean SDR processing pipeline that is:
- headless
- scriptable
- zero bloat
- fast enough for real RF work



## Why does this exist?

Most SDR stacks fall into 2 buckets:
1. Interactive lab tools (massive GUI, Python graphs, Qt widgets, etc.)
2. Hardcoded one-shot receivers/demods that aren't composable.

PhaseHound sits in the middle:
- It's not a GUI app.
- It's not a single demod.
- It's a signal routing core that lets you wire sources → DSP → sink, on the fly, using plugins.

Example:
- RF source from hardware via SoapySDR
- WFM demodulator
- Audio sink
- ...all as independent add-ons talking over a shared memory bus, coordinated by `ph-core`.

You can extend it by dropping in a new `.so`. No need to recompile the core.



## High-level architecture

```text
                -+
                |      ph-cli          |
                | (control / monitor)  |
                -+
                           |
                           |  Unix Domain Socket
                           v
                  +
                  |     ph-core      |
                  |  (the broker)    |
                  +
                            |
            --+
            |                                   |
    --+                    +
    |   soapy.so   |                    |   wfmd.so     |
    | RF source    |                    | WFM demod     |
    | (IQ out)     |                    | (Stereo PCM)  |
    --+                    +
            |                                   |
            |      shared memory buffers        |
            | file descriptors passed via UDS   |
            |>|
            |                                   |
            v                                   v
        +           --+
        | iq-feed          |           | audio-feed      |
        +           --+
                                                |
                                                v
                                      +
                                      | audiosink.so     |
                                      | output / playback|
                                      +

```


There are 3 important concepts:

### 1. Feeds

A "feed" is a named channel in the broker. Add-ons publish to feeds (IQ samples, audio frames, metadata, telemetry, etc.). Other add-ons subscribe.

### 2. Messages

Messages are framed JSON sent over the Unix domain socket. Some messages also carry shared-memory file descriptors so high-rate data doesn't go through JSON.

### 3. Add-ons

An add-on is just a shared library (`.so`) implementing a tiny interface:

* register your feeds,
* subscribe to what you need,
* react to commands,
* push data or announce new buffers.


## Features (current status)

* Pure C11 / `pthread` / `dlopen`
* Brokered pub/sub model over a Unix domain socket
* Runtime discovery and hot-loading of plugins
* Shared memory ring buffers for IQ/audio (file descriptors passed with `SCM_RIGHTS`)
* SoapySDR source add-on for live RF capture
* Wide FM demodulator add-on
* Audio sink / playback add-on
* Dummy add-on for devs to learn the plugin API
* Minimal CLI to control and observe the graph

Status: core works, IQ → WFM → audio works, API is still evolving but already usable.


## Quick start (FM radio demo)

This is the "it actually does RF" demo.

### 0. Build

```bash
cd PhaseHound/
make
```

### 1. Run the core

```bash
./ph-core
```

You should see logs like:

```text
[INF] core listening on /tmp/phasehound-broker.sock
[INF] autoload loaded plugin soapy
[INF] autoload loaded plugin wfmd
[INF] autoload loaded plugin audiosink
...
```

(If your distro doesn't allow autoload from `src/addons/...`, you can `load` them manually — see below.)

### 2. Use the CLI to inspect feeds

In a second terminal:

```bash
./ph-cli list feeds
./ph-cli list addons
```

### 3. Tune the SDR source

Tell the Soapy add-on to open a device, set center frequency and sample rate.
Example: 95.6 MHz FM broadcast, 2.4 MS/s, whatever gain the Soapy driver decides is sane.

```bash
./ph-cli pub soapy.config.in \
'{"cmd":"tune","freq_hz":95600000,"sample_rate":2400000,"gain":"auto"}'
```

### 4. Listen

At this point, data flow is:

`soapy.so` → (IQ feed in shared memory) → `wfmd.so` → (PCM feed) → `audiosink.so` → speakers.

You should get audio out, and the logs in `ph-core` will show pipeline activity, underruns/overruns, etc.


## Quick start (developer/dummy demo)

This one is meant for people writing new add-ons. It shows the command/response pattern without needing RF hardware.

1. Load the `dummy` addon:

```bash
./ph-cli load addon dummy
```

2. Ask it for help:

```bash
./ph-cli pub dummy.config.in '{"cmd":"help"}'
```

3. Ping it:

```bash
./ph-cli pub dummy.config.in '{"cmd":"ping"}'
```

4. Watch responses:

```bash
./ph-cli sub dummy.config.out
```

5. `dummy.foo` exists purely to demonstrate arbitrary new feed types:

```bash
./ph-cli sub dummy.foo
```

From a plugin author point of view:

* `dummy.config.in` = commands in
* `dummy.config.out` = replies / status
* `dummy.foo` = "some feed I invented"
  This is exactly how you're supposed to add new capabilities to PhaseHound. You don't patch the core; you ship a new `.so` with its own feeds.


## ph-cli cheatsheet

```bash
./ph-cli list addons
./ph-cli list feeds
./ph-cli load addon <name|/path/to/libsomething.so>
./ph-cli unload addon <name>

# publish a JSON command/message to a feed
./ph-cli pub <feed> '{"cmd":"...","arg":123}'

# subscribe to a feed and print everything it emits
./ph-cli sub <feedA> [<feedB> ...]
```

Subscriptions stay open and stream live. You can sub multiple feeds at once (useful for debugging data flow).


## How add-ons talk to each other

* Control / metadata:

  * sent as framed JSON over the core’s Unix domain socket.
  * The frame format is intentionally dead simple to parse/debug.

* High-bandwidth sample data (IQ buffers, audio frames):

  * passed as file descriptors using `SCM_RIGHTS` on that same socket,
  * then memory-mapped shared ring buffers are used,
  * atomic cursors coordinate producer/consumer without copies.

Translation: IQ at a couple MS/s and raw float audio are not shoved through JSON. You get proper throughput.

This is almost always where other "simple" SDR toys fall apart. PhaseHound already has the right architecture for real signal rates.


## Dependencies

**Required to build core + CLI:**

* POSIX environment (Linux is tested)
* `gcc`/`clang`, `make`
* `pthread`
* `dlopen` / `dlsym`
* nothing exotic

**Optional (but strongly recommended if you want hardware RF input):**

* `libsoapysdr-dev`

SoapySDR is used by the `soapy` addon to talk to real radios (RTL-SDR, HackRF, Lime, etc.).
If you don't install Soapy, everything else still builds — you just won't get live RF until we add file/network sources.


## Project status / roadmap

* ✅ Core broker, message framing, feed registry
* ✅ CLI
* ✅ SoapySDR source addon
* ✅ WFM demod addon
* ✅ Audio sink addon
* ✅ Dummy addon for devs
* 🔄 Shared memory ring buffers for IQ/audio
* 🔄 Stable plugin API docs
* 🔲 Digital voice / trunking / TETRA / DMR decoders
* 🔲 Network source / file source addon
* 🔲 Remote control over TCP instead of only local UDS
* 🔲 CI with sanitizer builds / fuzzer for the framing layer

This repo is early, but already functionally interesting.

The long-term vision is:

* make PhaseHound the "radio daemon" you leave running on a box,
* configure/observe it with simple CLI or a thin web UI,
* drop new demods as plugins instead of rebuilding the world.


## Contributing

You are absolutely welcome to:

* write a new addon (`src/addons/<yours>`),
* improve demodulators,
* add protocol decoders,
* add sinks (recording, streaming, etc.),
* improve security (peer credential check, socket perms),
* improve performance (epoll backend, lock-free rings).

We try to keep the code clean C11 and readable, not "clever".

Please open an issue / PR if:

* the docs drift,
* the protocol needs clarity,
* you built a cool addon.

