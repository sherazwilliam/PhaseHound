# Coding Style & Conventions

- C11, `-Wall -Wextra`, prefer small, single‑purpose helpers (`common.c`).
- Keep JSON minimal; no external JSON library is used — parsing helpers exist for simple `key:value` extraction.
- Feeds: lower‑case with dots, e.g. `rx0.iq`, `rx0.config.in`.
- Log sparingly with levels; avoid noisiness in hot paths.
- Avoid global mutable state; protect with `pthread_mutex_t` where shared (see feed tables and plugin registry).
