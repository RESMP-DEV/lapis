## Approach

Lapis uses **C++ exceptions** (`std::runtime_error`, `std::invalid_argument`, `std::length_error`, `std::overflow_error`, `std::out_of_range`) as the primary error propagation mechanism across its C++20 core. Python tooling raises custom exception classes (e.g., `SetupError` in `scripts/lapis.py`). The Qt GUI layer surfaces errors via `qCritical()`/`qWarning()` logging rather than exceptions, and Android/iOS apps use typed Kotlin/Swift result types at their gateway boundaries.

There is no centralized error-code enum or `Result<T>` monad; instead, each subsystem documents which specific standard exceptions it throws for which failure mode.

## Key Files and Patterns

### Core C++ contract: `services/session/include/lapis/session/terminal.hpp`
The `Terminal` class documents its exception contract explicitly in a comment:

> "Invalid geometry/configuration throws invalid_argument; input/snapshot limits throw length_error. Reply overflow throws overflow_error and permanently faults the instance (subsequent operations fail); recreate and resynchronize it. Upstream errors throw runtime_error. No exceptions cross a C callback."

This is the most explicit error-convention statement in the repository — it ties three distinct categories of failure to three distinct standard exceptions and defines recovery semantics (recreate + resync after overflow).

### Terminal implementation: `services/session/src/terminal/terminal.cpp`
Throws the documented exceptions per category:
- `std::invalid_argument` for parameter validation (limits, key enums, history offsets)
- `std::length_error` for byte/grapheme/cell count violations
- `std::overflow_error` for reply queue overflow
- `std::out_of_range` for index validation
- `std::runtime_error` for upstream/Ghostty protocol failures

A catch-all `catch (...)` block at line ~495 wraps viewport extraction to guarantee state restoration before rethrowing.

### Supervisor runtime: `services/supervisor/src/runtime.cpp`, `control_server.cpp`, `session_service_launcher.cpp`
Each file has a local `failed(const char*)` helper that wraps system calls with `std::generic_category().message(errno)` appended to a `std::runtime_error`. System call wrappers are consistently used around `open()`, `bind()`, `connect()`, `socket()`, etc.

Convergence and lifecycle methods wrap sub-operations in `try { ... } catch (const std::exception&)` blocks that swallow the exception and continue convergence, treating individual process launch failures as non-fatal to the supervisor loop.

### CLI entry point: `apps/desktop/src/main.cpp`
Argument parsing validates mutually exclusive flags and required options via `qCritical()` messages followed by early return. The top-level `main()` wraps Qt application startup in a `try { ... } catch (const std::exception& error) { qCritical() << error.what(); }` block so unhandled C++ exceptions become fatal diagnostic output rather than crashing silently.

### PTY process lifecycle: `services/session/src/platform/posix/pty_process.hpp`
No exceptions are thrown from `PtyProcess`; process termination and I/O errors are propagated through Qt signals (`finished(int exit_code, QProcess::ExitStatus)`, `failure(const QString& message)`). This keeps asynchronous OS event handling exception-free.

### Codex observer: `adapters/codex/src/observer.cpp`
Validation-heavy code path that throws `std::runtime_error` for every malformed hook payload, question set, inventory entry, timing field, and identity mismatch. Errors here reject the request before State consumption.

### Claude observer: `adapters/claude/src/claude_observer.hpp`
Observer API is signal-based with no throwing contract exposed publicly; internal diagnostics surface through `diagnostic()` returning a `QString`.

### Python tooling: `scripts/lapis.py`
Uses a custom `SetupError` exception class raised throughout argument/process validation paths, caught by a top-level `try: ... except Exception:` wrapper that returns an error exit code. Other scripts (`check_supervisor_runtime.py`, `check_restore.py`, etc.) follow the same pattern of raising domain-specific exceptions under `try/except` blocks.

## Architecture and Conventions

1. **Layered boundary**: Pure C++ libraries (`lapis::session`, `lapis::supervisor`) propagate errors via exceptions. Qt-owned layers (`PtyProcess`, observers) translate them into signals or log lines. Python tooling uses structured exception hierarchies.

2. **Exception taxonomy is semantic, not numeric**: Failure type is encoded in the exception class chosen (`invalid_argument` = caller bug / bad params, `length_error` = resource limit exceeded, `overflow_error` = invariant violated, `runtime_error` = upstream/system failure). There are no sentinel error codes comparable to `errno` within the app.

3. **System-call wrapping**: Every POSIX syscall in the supervisor is wrapped by a helper that converts `errno` into a `std::runtime_error` with the platform error message attached. Call sites never read `errno` directly.

4. **Graceful degradation in the supervisor loop**: Process launch failures are caught locally and logged, allowing the supervisor to continue converging other sessions. Only unrecoverable bootstrap failures bubble up.

5. **No exceptions across FFI boundaries**: The terminal's Ghostty C API is wrapped so that exceptions never escape into the C callback zone.

6. **GUI errors are logged, not propagated**: Desktop UI code uses `qCritical()` for user-facing configuration errors and `qWarning()` for recoverable conditions (e.g., missing next-prompt log directory, link open failure). Exceptions caught at the top level are also logged via `qCritical`.

7. **Asynchronous I/O avoids exceptions entirely**: PTY reads/writes, process lifecycle, and socket notifications go through Qt signals; callers handle outcomes reactively rather than catching exceptions.

## Conventions and Constraints

- The `Terminal` class contract in `services/session/include/lapis/session/terminal.hpp` is the authoritative source defining which standard exception maps to which failure class; implementers must preserve this mapping.
- No exceptions may cross a C callback boundary (documented in the same header comment).
- System call failures must be wrapped with `std::generic_category().message(errno)` appended to a `std::runtime_error` via the local `failed()` helpers in supervisor modules.
- Malformed external protocol payloads (Codex hooks, JSON state, control replies) raise `std::runtime_error` with a descriptive message indicating the rejected field.
- Unrecoverable `overflow_error` on the terminal leaves the instance in a permanently faulted state; callers must recreate and resynchronize it.
- Qt event-loop code should not throw exceptions into the event loop; instead, emit signals or log via `qCritical`/`qWarning`.
- Python scripts define and raise domain-specific exception subclasses (not bare `Exception`) so callers can distinguish setup/validation failures from runtime failures.