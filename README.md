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
the renderer TSan reports and records subsequent review fixes. The
[merge preparation receipt](evidence/pr2-merge.json) covers cursor presentation,
descendant cleanup and contributor/test procedures. Session identity/recovery,
terminal fidelity, disk history
and Linux qualification remain in the
[ordered plan](docs/architecture.md#next-complete-persistent-terminal-acceptance).
The [Codex route comparison](adapters/codex/README.md#integration-route-comparison)
separates terminal operation from attention delivery.

## Run the window on macOS

After the [dependency setup](CONTRIBUTING.md#desktop-preview), set up and run
everything through one entry point. No environment variables need exporting:

```sh
python3 scripts/lapis.py doctor    # report which dependencies are ready
python3 scripts/lapis.py bootstrap # build the pinned Ghostty terminal library (once)
python3 scripts/lapis.py check     # compile, lint, format-check and run CTest
python3 scripts/lapis.py build     # build the desktop app and run its checks
python3 scripts/lapis.py run       # open the live shell window
python3 scripts/lapis.py ui        # isolated fixture, source-QML reload and attention replay
python3 scripts/lapis.py ui-debug  # launch the isolated fixture in LLDB
python3 scripts/lapis.py ui-check  # bounded preview captures and failure cases
python3 scripts/lapis.py cli-check # CLI/service/GUI acceptance fixtures
```

`just` recipes with the same names wrap the same launcher (`just run`,
`just ui-check`, and so on), and `python3 scripts/lapis.py` alone lists every
command. The launcher locates the bootstrapped terminal dependency, supplies the
socket path, and opens windows on the laptop panel.

The shell starts in this checkout. Closing the window detaches it; reopening
reattaches to the same service-owned shell. Type `exit` to end the shell. The
current preview allows one attached window per socket endpoint. Builds stay under
`build/`; its owner-only local socket and service log stay under `runtime/`.
An additional window replaces the previous attachment. A service failure is not
GUI detachment: the current client reports disconnection and does not automatically
recover the session. Reopening after the service has exited starts a new shell.

To launch Codex directly in its own persistent terminal, use the launcher, which
resolves the build environment and opens on the laptop panel:

```sh
python3 scripts/lapis.py run --socket "$PWD/runtime/codex.sock" --cwd "$PWD" -- codex
```

Use a dedicated socket for a Codex session. The default socket keeps the shell
that `just run` starts, and an occupied endpoint rejects a different program
before replacing the window, so reusing it for Codex reports a launch mismatch.

Repeat the same command to reattach to the same Codex process. Codex 0.154.0
removed the older `--no-daemon` flag; the plain TUI is the owned backend, and no
lapis-specific Codex options are required. Other executables and literal
arguments work after `--`. Explicit programs or `--cwd` require a socket, which
the launcher supplies by default; a launch mismatch is rejected before replacing
the existing window. No hooks or approval settings are changed. There is still
one live pane per window; its other cards remain fixtures.

The new default socket is `runtime/desktop-v2.sock`. Older v1 sessions are not
migrated or terminated by this build. See the
[qualification procedure](CONTRIBUTING.md#cli-integration-qualification) for the
optional no-prompt Codex check and current limits.

- [Architecture and near-term plan](docs/architecture.md): component ownership,
  open decisions and acceptance criteria. This is the single implementation plan.
- [Codex investigation](adapters/codex/README.md): protocol routes and evidence.
- [Contributing](CONTRIBUTING.md): setup, checks, profiling and the PR procedure.
- [First contributor baseline](CONTRIBUTING.md#first-contributor-baseline): isolated
  worktrees, test sequence and coordination for large changes.
- [Test suites](CONTRIBUTING.md#test-suites-and-failure-triage) and
  [desktop sanitizers](CONTRIBUTING.md#desktop-sanitizers): coverage, commands and
  failure evidence.

## Check the C++ baseline

```sh
python3 scripts/lapis.py check         # Compile, lint, format-check and run CTest
python3 scripts/lapis.py asan          # Memory errors and undefined behavior
python3 scripts/lapis.py tsan          # Data races
python3 scripts/lapis.py verify-tools  # Prove the tools detect faulty fixtures
```

The contribution guide covers installation. Default CTest covers the toolchain,
POSIX descriptor ownership and the production terminal adapter; run
`python3 scripts/lapis.py bootstrap` once first. `python3 scripts/lapis.py build`
additionally covers PTY, local transport and UI reload/attention
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
