# Configuration System

## Approach

Lapis uses a **plain-file JSON configuration** model centered on `lapis.json`, with environment-variable overrides for locating the file and data directory. There is no external config framework: the desktop app reads/writes Qt's `QJsonObject` directly from disk, reloads it live when the file changes, and exposes a `reloadConfig` command. The supervisor runtime persists its own desired state in a separate private JSON file via a custom codec.

## Key files

- `docs/config.md` — authoritative user-facing documentation of every top-level key in `lapis.json` (`newAgent`, `harnessArguments`, `harnessUpdates`, `alerts`, `interactionLog`, `tabAway`, …).
- `apps/desktop/src/keymap.cpp` — parses `lapis.json`, resolves its path, watches it for filesystem changes, applies defaults (keybindings, layout, density, alert thresholds), and exposes `reloadConfig`.
- `apps/desktop/src/app_paths.hpp` — defines the data-directory resolution strategy used by the whole desktop app.
- `lapis.json` — root-level example/seed config (the actual per-user copy lives under `~/.lapis/lapis.json`).
- `services/supervisor/src/json_state_codec.cpp`, `posix_state_storage.cpp`, `state.cpp`, `client.cpp` — persistence layer for the supervisor's *desired state* (separate from user config).
- `services/supervisor/lapis_supervisor.qt.conf` — minimal Qt plugin-path hint.

## Location resolution and precedence

1. **User-data directory** (`data_directory()`): `~/.lapis` for a downloaded app, the checkout tree for a developer build. This is where `lapis.json` and `runtime/` live by default.
2. **`LAPIS_HOME`** — environment variable that overrides the data directory entirely.
3. **`LAPIS_CONFIG`** — environment variable that points to an alternate `lapis.json` file; used by tests to keep fixture configs apart from the person's real config.
4. Path expansion: paths in `lapis.json` may start with `~/` (home) or be relative to the directory containing `lapis.json`.

The order is enforced in `KeyMap::default_source_path()` (env var first, then `data_directory()/lapis.json`) and in `app_paths.hpp`'s comment describing `data_directory()`.

## File format and schema

`lapis.json` is a plain JSON object. Top-level keys observed across the codebase and docs:

- `keybindings` — maps action names to shortcut strings; parsed in `KeyMap::apply_defaults()` and merged with built-in defaults.
- `version` — schema version field (present in the repo seed).
- `newAgent` — defaults for the new-agent form: `harness`, `folder`, `mode`, `machines`, `models`.
- `harnessArguments` — extra flags appended to each new agent of a CLI.
- `harnessUpdates` — map of CLIs whose automatic updates are disabled.
- `alerts` — `sound`, `finished`, `repeat`, `notify`, `awayAfter`, `remindAfter`, `judge`, `finishedFile`, `finishedSound`.
- `interactionLog` — `enabled`, `pointerSampleMs`, `maxFileMiB`, `maxFiles`.
- `tabAway` — `rank`, `work`, `overdueHours`.

There is no formal schema validation library: values are read through `QJsonObject::value(...)` with defaults and ranges clamped at the call site (e.g. `remindAfter` clamped to `[0, 1440]`). Unknown fields are ignored.

## Live reloading

The desktop app watches the config file for filesystem changes and calls `KeyMap::reload()` automatically when the on-disk contents differ from the cached bytes. A `reloadConfig` command (default `Meta+R` on macOS, `Ctrl+Shift+R` elsewhere) triggers the same reload manually. Changes propagate to newly created agents immediately; existing sessions do not re-read their launch parameters.

## Supervisor state (separate from user config)

The supervisor's runtime state (desired session, enabled flag, spawn tokens, fingerprints) is persisted independently in a private JSON file under the supervisor's data directory. It uses:

- `JsonStateCodec` for serialization/deserialization.
- `PosixStateStorage` for atomic write/read with owner-only permissions (`0600`).
- Size bounds checked in both codec and storage (`max_state_bytes`, `max_state_file_size`).
- A poisoned-state mechanism: any corruption sets `poisoned_`, and further transitions fail with messages like `"Supervisor persistence failed; reload before transitioning"`.
- `Client::valid_state_file()` validates that the state file is a regular file, owned by the caller, within the allowed path bound, and has strict ownership.

This is distinct from `lapis.json`: the former is machine-readable runtime state, the latter is user-editable application settings.

## Constraints and conventions

- Config files are plain JSON; no YAML, INI, TOML, or `.env` files are used by the application itself.
- Secrets are not loaded from a dedicated secrets manager; sensitive values (e.g. `CLAUDE_CONFIG_DIR` passed to child processes, SSH config path) are resolved at runtime from environment or filesystem.
- User-facing config paths support `~/` home expansion and relative-to-config expansion, as documented in `docs/config.md` and implemented in the chime-sound loading path.
- Files referenced by config are bounded: sound files ≤ 4 MiB, interaction-log files capped by `maxFileMiB × maxFiles`, supervisor state size-checked against a hard limit.
- Permissions are enforced: the `runtime/` directory must be `0700` (owner-only) or lapis refuses to write the interaction log; supervisor control sockets are `0600`.
- Defaults are applied in code rather than supplied by a separate defaults file — keybinding defaults, alert thresholds, layout/density enums are all compiled into `KeyMap::apply_defaults()`.
- Test isolation: use `LAPIS_CONFIG` to point at a test fixture instead of editing `~/.lapis/lapis.json`.

## What this does NOT include

- Android/iOS apps have their own platform-specific preferences (Android `Settings.kt`, iOS `Info.plist` / Swift `UserDefaults`), but these are separate from the cross-platform `lapis.json` system described here.
- No feature-flag service or remote configuration endpoint is present in the observed code.