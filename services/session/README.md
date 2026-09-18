# Session component (planned)

Owns local processes, PTY/ConPTY I/O, terminal state, bounded history, structured
agent clients, and adapter events independently of the desktop process. Exposes
session snapshots and routes user input/decisions by stable identity.

The implementation language is open: C++ can share the desktop toolchain; Rust
can be evaluated for process and protocol orchestration. Codex's Rust internals
are not a stable embedding API by assumption. Prefer a measured external protocol
boundary until a concrete need justifies linking internal crates.
