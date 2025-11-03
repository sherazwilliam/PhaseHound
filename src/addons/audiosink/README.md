# PhaseHound — audiosink Addon (Normalized ABI)

> **Status:** reference sink addon • Last updated: 2025-11-03

`audiosink` is a normalized ABI addon that handles audio output from demodulators such as `wfmd`.
It connects via the pub/sub system, consumes PCM frames, and outputs them to an audio device.

## 1. Purpose

The audiosink plugin consumes decoded audio frames from other PhaseHound modules and plays them via ALSA or other supported devices.
It uses the normalized ABI: unified control plane (`*.config.in/out`), predictable lifecycle, and explicit feed declarations.

## 2. Feeds

| Feed | Direction | Description |
|------|------------|-------------|
| `audiosink.config.in` | **in** | Control commands and configuration. |
| `audiosink.config.out` | **out** | Human-readable responses and status updates. |
| `audiosink.audio-info` | **out** | JSON-formatted playback statistics. |

In `plugin_register()`:

```c
out->consumes = (const char*[]) { "audiosink.config.in", NULL };
out->produces = (const char*[]) { "audiosink.config.out", "audiosink.audio-info", NULL };
```

## 3. Lifecycle Hooks

```c
bool plugin_load(ph_core_api_t *api);
bool plugin_register(ph_plugin_desc_t *out);
bool plugin_start(void);
void plugin_stop(void);
void plugin_unload(void);
```

The addon opens and controls an ALSA device during `plugin_start()` and stops cleanly on `plugin_stop()`.

## 4. Control Commands

Commands on `audiosink.config.in` (text or JSON):

```
help
device <alsa_device>             # Example: hw:0,0
set sr=<hz> ch=<1|2> fmt=<f32|s16>
latency <ms>
volume <0-100>
mute <on|off>
start
stop
subscribe <feed>                 # Example: wfmd.audio
unsubscribe <feed>
stats
```

Example usage:

```bash
./ph-cli pub audiosink.config.in "device hw:0,0"
./ph-cli pub audiosink.config.in "subscribe wfmd.audio"
./ph-cli pub audiosink.config.in start
./ph-cli sub audiosink.config.out
```

## 5. Expected Audio Format

- **Sample rate**: 48 kHz
- **Format**: `f32` or `s16`
- **Channels**: 1 (mono) or 2 (stereo, interleaved)

The sink performs no resampling or conversion — the upstream module must match the device format.

## 6. JSON Examples

Send structured control commands:

```json
{ "cmd": "device", "name": "hw:0,0" }
```

Periodic info example (`audiosink.audio-info`):

```json
{ "sr": 48000, "ch": 2, "fmt": "f32", "underrun": 0, "device": "hw:0,0" }
```

## 7. Build

```bash
make -C src/addons/audiosink
# or
make addons
```

Produces `libaudiosink.so`.

## 8. Typical Pipeline

```
[Soapy IQ] → wfmd (demod) → wfmd.audio → audiosink
```

```bash
./ph-cli pub audiosink.config.in "device hw:0,0"
./ph-cli pub audiosink.config.in "subscribe wfmd.audio"
./ph-cli pub audiosink.config.in start
```

## 9. Troubleshooting

| Problem | Solution |
|----------|-----------|
| Silence | Verify `wfmd.audio` publishes data and subscription is correct. |
| Distortion | Ensure sample rate, format, and channels match. |
| No device | Run `aplay -L` to list valid ALSA outputs. |

## 10. File Layout

```
src/addons/audiosink/
├── Makefile
├── src/audiosink.c
└── README.md
```
