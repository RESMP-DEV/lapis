# lapis

Repository: [RESMP-DEV/lapis](https://github.com/RESMP-DEV/lapis).

lapis is a provisional desktop workspace for operating many CLI agents with fast
session switching, an attention-driven carousel, and GPU-accelerated rendering.
Codex is the first integration target; each additional tool gets its own adapter.

Agents start with [AGENTS.md](AGENTS.md), which defines implementation boundaries,
parallel work areas, milestones, and verification requirements. `CLAUDE.md` links
to that same file so Codex and Claude Code share one set of project instructions.

## Starting point

This folder contains the initial design, adapter boundaries, a reproducible
Codex protocol probe, and a runnable C++ verification baseline. It does not yet
contain a terminal emulator, renderer, session daemon, or working attention adapter.

- [Architecture](docs/architecture.md): persistent sessions and a portable UI.
- [Attention contract](docs/attention-contract.md): tool events and focus policy.
- [Adapter model](adapters/README.md): integration levels for individual CLIs.
- [Codex integration](adapters/codex/README.md): source findings and runtime evidence.
- [Desktop boundary](apps/desktop/README.md).
- [Session service boundary](services/session/README.md).
- [C++ verification](docs/verification.md): linting, sanitizers, and profiling workflow.

## Check the C++ baseline

```sh
just check         # Compile, lint, format-check, and run CTest
just asan          # Memory errors and undefined behavior
just tsan          # Data races
just verify-tools  # Prove the tools detect deliberately faulty fixtures
```

See the verification guide for installation and Python commands without `just`.
The current C++ program verifies the toolchain; it is not the desktop app.

The initial desktop candidate is C++20 with Qt 6 Quick and a custom terminal
surface. Codex's Rust implementation is a reason to investigate its existing
structured interfaces; it does not require lapis to share its language or link
its internal crates. The service language and terminal engine remain decisions
for a measured implementation spike.

## Probe the installed Codex

Requires Python 3.11+ and `codex` on PATH; no Python packages are needed.

```sh
python3 scripts/probe_codex.py --output evidence/codex-probe.json
```

The probe exports the installed binary's schema to a temporary directory, starts
a private stdio app-server, initializes a client, and lists its loaded threads.
It does not start a turn or send a prompt. It uses the existing Codex profile;
Codex itself may perform normal startup discovery and write runtime logs.
The receipt retains method names and counts, not thread content or credentials.

## First implementation milestone

1. Implement one persistent terminal session and a replayable attention queue.
2. Connect Codex through a verified adapter; demonstrate input and approval events.
3. Demonstrate safe keyboard ownership during carousel transitions.
4. Add multiple sessions and GPU rendering; benchmark 32 sessions under output load.
5. Qualify a second CLI adapter to prove the shared contract is tool-independent.

Performance figures are acceptance targets until measured. No hooks have been
installed into existing CLI configurations as part of this bootstrap.
