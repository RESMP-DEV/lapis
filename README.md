# lapis

Repository: [RESMP-DEV/lapis](https://github.com/RESMP-DEV/lapis).

lapis is an early-stage project for a desktop workspace that supervises live CLI
agents. The direction is persistent sessions, fast switching, GPU rendering and
an opt-in attention carousel. macOS and Codex come first; Linux is the next
required platform. There is no desktop application to run yet.

The priority is **responsiveness, then ergonomics, then visuals**. Design for
high-end M-series hardware and high-refresh displays; use generous, bounded RAM
caches to keep sessions ready for immediate switching.

Agents start with [AGENTS.md](AGENTS.md), which defines implementation boundaries,
parallel work areas, milestones, and verification requirements. `CLAUDE.md` links
to that same file so Codex and Claude Code share one set of project instructions.

## Current status

This checkpoint establishes the engine choice, resource-ownership foundation and
contributor workflow. **Milestone 1, one persistent terminal, is incomplete.**

| Component | Exercised | Remaining |
| --- | --- | --- |
| C++20 platform foundation | Move-only descriptor ownership and real pipe I/O/cleanup tests on macOS | PTY launch, resize, process lifecycle and persistent service |
| Headless engine comparison | Ghostty 8/8; Contour 7/8 on macOS ARM64 and Ubuntu 24.04 ARM64 in a local container | Production adapter and broader terminal compatibility |
| Codex protocol probe | Schema export, initialization and loaded-thread listing | Real attention requests, responses and reconnect handling |
| GPU and profiling tools | Vulkan device discovery through MoltenVK; Instruments capture smoke | Terminal rendering, presentation and responsiveness measurements |
| Desktop workspace | Architecture only | Input/IME, previews, carousel and accessibility |

**Next code change: the production Ghostty adapter.** Keep its pinned C API
isolated, extend the tested snapshot subset and verify bounded terminal history.
The PTY service and minimal GUI follow adapter acceptance. Dependency notices must
be completed before redistribution; latency targets remain provisional.

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
Default CTest covers the toolchain and POSIX descriptor ownership; terminal replay
runs separately below. There is no desktop test coverage yet.

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
