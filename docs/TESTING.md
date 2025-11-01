# Testing & Examples

## Demo pipeline (A → B)

1. Start core: `./ph-core`
2. Build add‑ons: `make addons`
3. Load `abracadabra` and `barbosa`:
   - `./ph-cli load addon abracadabra`
   - `./ph-cli load addon barbosa`
4. Publish to `A-in`: `./ph-cli pub A-in "hello world"`
5. Observe:
   - `abracadabra` base64‑encodes and publishes on `A-out`
   - `barbosa` decodes `A-out` and appends to a timestamped `./<epoch>.log`

## SHM demo (dummy)

```
./ph-cli sub dummy.config.out
./ph-cli pub dummy.config.in "shm-demo"
```

The add‑on will pass a 1 MiB SHM fd, then publish small JSON heartbeats with `subtype:"shm_ready"`.
