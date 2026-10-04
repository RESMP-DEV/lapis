# Build

## From source

After the [dependency setup](../CONTRIBUTING.md#desktop-preview):

```sh
uv run --no-project python scripts/lapis.py doctor      # what is ready
uv run --no-project python scripts/lapis.py diagnose --json
uv run --no-project python scripts/lapis.py bootstrap   # the pinned terminal engine, once
uv run --no-project python scripts/lapis.py build       # build and the full desktop checks
uv run --no-project python scripts/lapis.py run         # open the workspace
```

A build from the repository is a developer build; the
[downloadable app](install.md) carries its own Qt, MoltenVK and dependency
notices. How releases are built, signed and notarized is in
[CONTRIBUTING](../CONTRIBUTING.md#build-the-mac-app).

## Diagnosing an installed runtime

`diagnose` is a read-only, user-facing check that needs neither a desktop build
nor a GUI. On macOS it defaults to `/Applications/lapis.app` and
`$LAPIS_HOME/runtime` (otherwise `~/.lapis/runtime`); `--app PATH` and
`--runtime PATH` select explicit fixtures or another installation. It checks
package shape and version metadata, private runtime-directory rules, bounded
registry inventory counts, and the shape of session identity hints. Endpoint
reachability is only a bounded Unix-socket connect that is closed before any
protocol bytes; this proves neither protocol compatibility nor health. Missing
or malformed selected inputs return status 1. It never restores sessions, reads
terminal output, launches a process, or changes runtime state.

`--samples N` repeats that same connect-only probe for each bounded endpoint.
N must be 1 through 8 and defaults to 1. Every duration is measured with a
monotonic clock around the connect only. The full report has aggregate and
per-classification p50/p95/p99/max milliseconds plus sample, success, and
timeout counts. Eight samples raise the worst-case connect wait to about 10
seconds. They are local connect diagnostics, not service health, protocol
compatibility, workload-capacity, or resource-acceptance evidence.

`diagnose --export` is the separate support-export mode. It writes a bounded
JSON summary to standard output for the user to submit deliberately. It reports
states, stable issue codes and counts, version-like package fields, socket
connect counters, and aggregate connect-timing distributions. It omits
per-endpoint timing, selected paths, session identities, launch data, account
identifiers, terminal content, and any health claim.

## Checking a change

For routine UI review run `python3 scripts/lapis.py ui-review` (or `just
ui-review`). It builds only the fixtures it needs and checks workspace UI,
shortcuts and terminal input in Qt's offscreen software mode, without taking OS
focus or moving the pointer, and saves PNGs beside its logs for you to look at;
`--json` gives the result and paths. `build` stays the full desktop gate;
`ui-check` and the native input checks keep their own display, GPU and macOS
scope. Use focused CMake targets and CTest expressions for other edits. See the
[background and native test procedure](../CONTRIBUTING.md#history-and-input-qualification).
macOS is the active target; `linux-gui` exists only for an explicitly chosen
Linux port check.

```sh
python3 scripts/lapis.py check         # compile, lint, format-check and CTest
python3 scripts/lapis.py asan          # memory errors and undefined behavior
python3 scripts/lapis.py tsan          # data races
python3 scripts/lapis.py verify-tools  # prove the tools catch faulty fixtures
```

Default CTest covers the toolchain, POSIX descriptor ownership, the terminal
adapter and the attention reducer; run `bootstrap` once first. `build` adds the
PTY, local transport and UI reload and attention behavior. Isolated captures and
the live input probe are in the contribution guide.

## The terminal engine experiment

`python3 scripts/probe_terminal.py` verifies pinned source and toolchain
archives, builds both candidate engines and keeps each result. The comparison
still exits nonzero for Contour's preserved failure; `--engine ghostty` runs the
passing candidate. See the [receipt](../evidence/terminal-engine-probe.json).

## Probing the installed Codex

Needs Python 3.11+ and `codex` on PATH, and no packages:

```sh
python3 scripts/probe_codex.py --output build/reports/codex-probe.json
```

It exports the installed binary's schema to a temporary folder, starts a
private app-server, initializes a client and lists its loaded threads. It never
starts a turn or sends a prompt. It uses your Codex profile, so Codex may do its
normal startup discovery and write logs. The receipt keeps method names and
counts, not thread content or credentials. Local receipts may include machine
paths; sanitize a dated copy before adding it to `evidence/`.

## Checking Claude Code release notes

The changelog CLI reads Claude Code's public RSS feed without launching Claude
or using an account. Initialize a local baseline once, then check for new entries:

```sh
uv run --no-project python scripts/check_claude_changelog.py --record
uv run --no-project python scripts/check_claude_changelog.py --json
```

Run `--record` again after reviewing the reported releases. Ordinary checks leave
the baseline unchanged; a missing baseline is reported explicitly and older
releases are not all marked new. `--state` selects an isolated baseline,
`--timeout` bounds each network wait, and response bytes are capped at 2 MiB.
Read/write or feed errors exit 1, invalid arguments exit 2, and successful checks
exit 0. Recording replaces the baseline atomically, refuses a feed that parsed to
no entries, replaces an unreadable baseline instead of failing, and follows
redirects only within the feed's own host. The CLI creates no scheduled
job. Release notes suggest what to probe; they do not qualify the installed
adapter's runtime behavior.

## Development fixtures

`--ui-preview` runs the same category UI with synthetic data; it is never added
to a real workspace. Standalone shells need `--development-shell` and are not
offered in the product. The managed probes keep `--codex --socket ... --cwd ...
-- codex` and `--claude ... -- claude`; leaving out `--codex` and `--claude` is a
plain terminal launch that passes agent options through as given and installs
no hooks or approval settings. Existing v6 endpoints are neither adopted nor
ended by the category registry. No hooks are ever installed into a CLI's own
configuration.
