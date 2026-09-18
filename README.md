# lapis

Repository: [RESMP-DEV/lapis](https://github.com/RESMP-DEV/lapis).

lapis is an early-stage project for a desktop workspace that supervises live CLI
agents. The direction is persistent sessions, fast switching, GPU rendering and
an opt-in attention carousel. macOS and Codex come first; Linux is the next
required platform. A macOS terminal window is now available for a visual checkpoint.

The priority is **responsiveness, then ergonomics, then visuals**. Design for
high-end M-series hardware and high-refresh displays; use generous, bounded RAM
caches to keep sessions ready for immediate switching. Use minimal, mostly opaque
surfaces and short, interruptible animations, drawing on Apple and Material design.

Agents start with [AGENTS.md](AGENTS.md), which defines implementation boundaries,
parallel work areas, milestones, and verification requirements. `CLAUDE.md` links
to that same file so Codex and Claude Code share one set of project instructions.

## Current status

The macOS preview has a **live terminal in the enlarged pane** (shell by default) and a horizontal
strip of five placeholder sessions plus the live preview below it. The cards show
the carousel composition; they do not switch sessions yet. **Milestone 1 is still
incomplete.**

| Component | Exercised | Remaining |
| --- | --- | --- |
| POSIX resources and terminal adapter | Descriptor ownership and 14 Ghostty adapter cases on macOS and Linux ARM64 | Broader terminal compatibility |
| PTY and separate session service | Explicit executable/argv/cwd, shell default, resize/paste/exit, failed launch, detached output and same-child reattachment on macOS | Linux service qualification, recovery identities, disk-backed history |
| Local transport | Version 2 launch-matching attachment, bounded frames/snapshots and malformed-message rejection without displacing the active client | Attachment generations, hostile/slow-client and failure recovery cases |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, default/compact window captures on M4 Max via MoltenVK | Full shaping, IME, selection, accessibility, Linux GUI and latency/frame qualification |
| UI iteration and attention fixture | Isolated source-QML reload, PNG captures, LLDB launch/attach, compact header and finite red cue replay | Maintainer visual review, rebindable navigation and real attention integration |
| Codex integration | Direct TUI launch, no-prompt editing/navigation/paste/resize, normal/compact GPU captures and same-child reattachment; separate schema/init/list probe | Real model-turn attention requests, responses and source reconnect handling |

[Desktop evidence](evidence/desktop-preview.json),
[UI refinement evidence](evidence/ui-preview.json) and
[adapter evidence](evidence/terminal-adapter.json) delimit these observations.
Dependency packaging remains unfinished; this is a local developer build.
The [two UI refinements](docs/architecture.md#ui-refinement-checkpoint) are
implemented for visual review: an isolated preview/debugging workflow and a compact
header with replayable red attention cues. Rebindable navigation follows; real
agent attention and automatic carousel behavior remain later work. Finish terminal
acceptance and qualify the minimal Linux view before expanding the live workspace.
Latency and warm-switch targets remain provisional.

Explicit CLI launch is now implemented through the existing service-owned PTY.
The [launch receipt](evidence/cli-launch.json) records the exercised macOS scope and
dated sanitizer limitations. The [PR #2 repair](evidence/pr2-review.json) resolves
the renderer TSan reports and records subsequent review fixes. Session identity/recovery, terminal fidelity, disk history
and Linux qualification remain in the
[ordered plan](docs/architecture.md#next-complete-persistent-terminal-acceptance).
The [Codex route comparison](adapters/codex/README.md#integration-route-comparison)
separates terminal operation from attention delivery.

## Run the window on macOS

After the [dependency setup](CONTRIBUTING.md#desktop-preview):

```sh
just desktop      # Build, behavioral cases and static checks
just run          # Open the live shell window
just ui           # Isolated fixture, source-QML reload and attention replay
just ui-debug     # Launch the isolated fixture in LLDB
just ui-check     # Bounded preview captures and failure cases
just cli-check    # Dedicated CLI/service/GUI acceptance fixtures
```

The shell starts in this checkout. Closing the window detaches it; reopening
reattaches to the same service-owned shell. Type `exit` to end the shell. The
current preview allows one attached window per socket endpoint. Builds stay under
`build/`; its owner-only local socket and service log stay under `runtime/`.
An additional window replaces the previous attachment. A service failure is not
GUI detachment: the current client reports disconnection and does not automatically
recover the session. Reopening after the service has exited starts a new shell.

To launch Codex directly in its own persistent terminal:

```sh
build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop \
  --socket "$PWD/runtime/codex.sock" --cwd "$PWD" -- codex --no-daemon
```

Repeat the same command to reattach. `--no-daemon` selects a Codex backend owned
by that TUI; it is an explicit choice for this example, not a lapis default.
Other executables and literal arguments work after `--`. Explicit programs or
`--cwd` require `--socket`; a launch mismatch is rejected before replacing the
existing window. No hooks or approval settings are changed. There is still one
live pane per window; its other cards remain fixtures.

The new default socket is `runtime/desktop-v2.sock`. Older v1 sessions are not
migrated or terminated by this build. See the
[qualification procedure](CONTRIBUTING.md#cli-integration-qualification) for the
optional no-prompt Codex check and current limits.

- [Architecture and near-term plan](docs/architecture.md): component ownership,
  open decisions and acceptance criteria. This is the single implementation plan.
- [Codex investigation](adapters/codex/README.md): protocol routes and evidence.
- [Contributing](CONTRIBUTING.md): setup, checks, profiling and the PR procedure.

## Check the C++ baseline

```sh
just check         # Compile, lint, format-check, and run CTest
just asan          # Memory errors and undefined behavior
just tsan          # Data races
just verify-tools  # Prove the tools detect deliberately faulty fixtures
```

See the contribution guide for installation and Python commands without `just`.
Default CTest covers the toolchain, POSIX descriptor ownership and the production
terminal adapter. Bootstrap its pinned dependency as described in the contribution
guide before the first check. `just desktop` additionally covers PTY, local transport and UI reload/attention
behavior; isolated captures and the live input probe are described in the
contribution guide. The engine comparison runs separately below.

Run the separate engine experiment with `python3 scripts/probe_terminal.py`.
It verifies pinned source/toolchain archives, builds both consumers and keeps each
result. The comparison currently exits nonzero for Contour's preserved failure.
Use `--engine ghostty` for the passing candidate. See the
[experiment receipt](evidence/terminal-engine-probe.json) and contribution guide.

## Probe the installed Codex

Requires Python 3.11+ and `codex` on PATH; no Python packages are needed.

```sh
python3 scripts/probe_codex.py --output build/reports/codex-probe.json
```

The probe exports the installed binary's schema to a temporary directory, starts
a private stdio app-server, initializes a client, and lists its loaded threads.
It does not start a turn or send a prompt. It uses the existing Codex profile;
Codex itself may perform normal startup discovery and write runtime logs.
The receipt retains method names and counts, not thread content or credentials.
Local receipts may include machine paths; review and sanitize a dated copy before
adding it to `evidence/`.

No hooks have been installed into existing CLI configurations. Later milestones
and their acceptance criteria live in the architecture document.

## License

lapis is licensed under [MIT](LICENSE). Third-party components retain their own
licenses; the experiment receipt records upstream licenses and outstanding
attribution work required before redistribution.
