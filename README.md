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

The macOS preview has a **live local shell in the enlarged pane** and a horizontal
strip of five placeholder sessions plus the live preview below it. The cards show
the carousel composition; they do not switch sessions yet. **Milestone 1 is still
incomplete.**

| Component | Exercised | Remaining |
| --- | --- | --- |
| POSIX resources and terminal adapter | Descriptor ownership and 14 Ghostty adapter cases on macOS and Linux ARM64 | Broader terminal compatibility |
| PTY and separate session service | macOS shell I/O, resize, exit, failed exec, output burst, GUI close/reopen with the same child | Linux service qualification, recovery identities, disk-backed history |
| Local transport | Bounded versioned frames, owned snapshots, fragmented/coalesced input and malformed-message rejection | Attachment generations, hostile/slow-client and failure recovery cases |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, default/compact window captures on M4 Max via MoltenVK | Full shaping, IME, selection, accessibility, Linux GUI and latency/frame qualification |
| Codex protocol probe | Schema export, initialization and loaded-thread listing | Real attention requests, responses and reconnect handling |

[Desktop evidence](evidence/desktop-preview.json) and
[adapter evidence](evidence/terminal-adapter.json) delimit these observations.
Dependency packaging remains unfinished; this is a local developer build.
The [next two implementation steps](docs/architecture.md#planned-ui-refinement)
are an isolated UI preview/debugging workflow, then a compact header and replayable
red attention cue. Neither has started. Rebindable navigation follows; real agent
attention and automatic carousel behavior remain later work. Finish terminal
acceptance and qualify the minimal Linux view before expanding the live workspace.
Latency and warm-switch targets remain provisional.

## Run the window on macOS

After the [dependency setup](CONTRIBUTING.md#desktop-preview):

```sh
just desktop      # Build, behavioral cases and static checks
just run          # Open the compiled app
```

The shell starts in this checkout. Closing the window detaches it; reopening
reattaches to the same service-owned shell. Type `exit` to end the shell. The
current preview allows one attached window per checkout. Builds stay under
`build/`; its owner-only local socket and service log stay under `runtime/`.

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
guide before the first check. `just desktop` additionally covers PTY and local
transport behavior; the app capture/input probe is described in the contribution
guide. The engine comparison runs separately below.

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
