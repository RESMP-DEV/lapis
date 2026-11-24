## Approach

Lapis uses two distinct logging mechanisms:

1. **Qt's `QDebug` family** (`qDebug`, `qInfo`, `qWarning`, `qCritical`) is the default output channel for all C++ components (desktop app, session service, supervisor, adapters). There is no central logger facade, no log-level configuration file, and no external sink wiring — each call writes directly through Qt's debug message handler.
2. **A dedicated interaction log subsystem** in `apps/desktop/src/interaction_log.{hpp,cpp}` that writes structured JSON Lines to a per-user file under `<data>/runtime/interaction.jsonl` on a background thread with rotation.

No third-party logging framework (spdlog, glog, Boost.Log) is used by application code; grep across `services/` and `adapters/` returns zero matches for `spdlog`, `LOG_`, or any structured-log macro.

## Key files

- `apps/desktop/src/main.cpp` — entry point; validates CLI options via `qCritical`; starts the desktop UI and emits startup/shutdown messages via `qInfo`.
- `apps/desktop/src/interaction_log.hpp` / `interaction_log.cpp` — the only structured log implementation: settings model, secret-redaction helpers, pointer sampler, and `InteractionWriter` (background writer with queue, rotation, and drop accounting).
- `services/session/src/session_service.cpp` — uses `qWarning`/`qInfo` for attention-journal and history errors.
- `services/supervisor/src/session_service_launcher.cpp` — logs supervisor handshake failures and child process issues via `qWarning`.
- `services/session/src/platform/posix/pty_process.cpp` — logs PTY drain warnings.

## Architecture and conventions

### Qt debug output
- Every module includes `<QDebug>` and calls `qDebug()`, `qInfo()`, `qWarning()`, or `qCritical()` directly at the call site.
- Messages are unstructured strings built with Qt's stream operator (`<<`). Structured fields are not encoded as key-value pairs.
- Long diagnostic lines commonly use `.noquote()` to avoid extra quoting of already-formatted strings.
- No global log level is configured in the codebase; behavior is governed by Qt's runtime debug-message hook, which is not set up here.

### Interaction log (structured, persisted)
The interaction log is the only place where records have a schema and are persisted to disk:

- Location: `<data>/runtime/interaction.jsonl` (one JSON object per line).
- Enabled by an `InteractionLogSettings.enabled` flag read from config; defaults to off.
- `InteractionWriter::append` enqueues `QJsonObject` records plus a wall-clock millisecond timestamp on a dedicated `std::thread`. The writer serializes them and flushes batches to disk.
- Rotation policy: when the current file exceeds `maxFileBytes`, it rotates to `path.1 … path.(files-1)`; `kQueueLimit = 16384` bounds the in-memory queue, and overflow produces a `dropped` record with a count.
- Failure mode: append failures do not propagate to the caller and never reach input handling; a `dropped` record is emitted once the queue has room again.
- Permissions: the data directory is created with `S_IRWXU` and verified to be owner-only; wider directories are refused rather than relaxed.
- Secret redaction: `likely_secret_prompt` detects password/passphrase/PIN/OTP/verification-code prompts; `key_class` categorizes keys without recording their text.
- Pointer movement is sampled at a configurable interval (`pointerSampleMs`, default 50 ms, max 10 s) and folded into one trailing sample carrying the latest position and fold count.

### Conventions observed
- Warning vs. critical distinction: recoverable I/O/configuration problems go through `qWarning()`; mutually exclusive CLI flags and invalid command-line combinations go through `qCritical()`.
- Startup/operational status messages go through `qInfo().noquote()`.
- No log levels are exposed to users; there is no `--verbose`/`--debug` flag in `main.cpp` that adjusts Qt's debug output.
- Structured field names in the interaction log are documented inline in `interaction_log.hpp` comments (e.g. `{"interactionLog": {"enabled": true, "pointerSampleMs": 50, "maxFileMiB": 64, "maxFiles": 8}}`).

## Rules enforced by the code

- Unqualified workspace launches are rejected at startup unless one of `--codex`, `--claude`, `--development-shell`, `--smoke-input`, an explicit program, or `--socket` is provided (`valid_agent_launch` in `main.cpp`).
- `--codex` and `--claude` are mutually exclusive; both cannot be passed together.
- `--new-session` and `--discover` are mutually exclusive.
- `--ui-preview` cannot be combined with `--codex`, `--claude`, `--socket`, `--cwd`, or an explicit program.
- An explicit launch or working directory requires `--socket`.
- The interaction log data directory must be owned by the current user and must not grant group/other permissions; otherwise the directory is refused.
- Out-of-range interaction-log settings values are clamped to the documented min/max ranges defined in `InteractionLogSettings` constants.
- The interaction writer drops records silently when its 16,384-item queue is full and emits a `dropped` record summarizing the gap once space frees up.