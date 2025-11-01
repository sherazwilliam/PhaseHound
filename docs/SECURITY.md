# Security Notes

- **Socket permissions:** The broker listens on `/tmp/.PhaseHound-broker.sock`. Consider directory permissions and umask to restrict access to trusted users.
- **Trusted plugins:** Add‑ons are native code with full process privileges. Only load signed or trusted `.so` files.
- **Input validation:** The core routes JSON as‑is. Add‑ons should defensively parse only expected keys and cap lengths.
- **FD limits:** Guard `nfds` when receiving (`recv_frame_json_with_fds`) and close unused fds.
- **Resource cleanup:** On client disconnect, the core unsubscribes the fd. Add‑ons must also unmap and close SHM on `plugin_stop`.
