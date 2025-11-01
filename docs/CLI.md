# CLI (`ph-cli`) Usage

`ph-cli` is a small client for sending commands and subscribing to feeds.

## Examples

- **Help**

```
./ph-cli help
```

- **Publish text**

```
./ph-cli pub A-in "hello"
```

- **Subscribe to one or more feeds** (Ctrl+C to exit)

```
./ph-cli sub A-out dummy.config.out
```

- **List things**

```
./ph-cli list feeds
./ph-cli list addons
./ph-cli available-addons
```

- **Load / unload add‑ons**

```
./ph-cli load addon abracadabra
./ph-cli unload addon abracadabra
```

Internally, the CLI sends compact JSON on the broker socket `/tmp/.PhaseHound-broker.sock` with the framing described in **PROTOCOL.md**.
