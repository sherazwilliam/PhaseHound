# Building

## Prerequisites

- POSIX toolchain (C11 compiler, make)
- Linux recommended (for `memfd_create`); works with POSIX SHM fallback
- `dlopen`/`pthread` available

## Build all

```bash
make            # builds ph-core and ph-cli
make addons     # builds all add-ons under src/addons/*
```

Artifacts:
- `ph-core`
- `ph-cli`
- `src/addons/*/lib*.so`

## Run

```bash
./ph-core
```

Use a second terminal for `./ph-cli ...` commands.
