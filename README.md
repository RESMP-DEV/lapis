# lapis

Repository: [RESMP-DEV/lapis](https://github.com/RESMP-DEV/lapis).

lapis is an early-stage project for a desktop workspace that supervises live CLI
agents. The direction is persistent sessions, fast switching, GPU rendering and
an opt-in attention carousel. macOS and Codex are the active target; a Linux
desktop port is deferred. The macOS workspace supports two retained sessions.

The priority is **responsiveness, then ergonomics, then visuals**. Design for
high-end M-series hardware and high-refresh displays; use generous, bounded RAM
caches to keep sessions ready for immediate switching. Use minimal, mostly opaque
surfaces and short, interruptible animations, drawing on Apple and Material design.

Agents start with [AGENTS.md](AGENTS.md), which defines implementation boundaries,
parallel work areas, milestones, and verification requirements. `CLAUDE.md` links
to that same file so Codex and Claude Code share one set of project instructions.

## Current status

The macOS workspace retains live terminal, Codex and Claude Code sessions in one window. It
supports manual switching, identity-checked reconnection and detached services.
**Milestone 1 is qualified on macOS.** Milestone 2 is qualified for one managed
Codex session. **Milestone 3 is qualified for two sessions on macOS:** retained
terminals, a shared attention queue, source-bound decisions, and an opt-in guarded
carousel. The assembled qualification and measurement limits are recorded in the
[workspace receipt](evidence/milestone-three-workspace.json), including the
two-session integration checks repeated after the Claude hook addition.

| Component | Exercised | Remaining |
| --- | --- | --- |
| POSIX resources and terminal adapter | Descriptor ownership and 14 Ghostty adapter cases on macOS and Linux ARM64 | Broader terminal compatibility |
| PTY and separate session service | Explicit executable/argv/cwd, shell default, resize/paste/exit, failed launch, detached output and same-child reattachment on macOS | Recovery after service loss and later Linux qualification |
| Local transport | Version 6 identity/epoch/generation attachment, correlated history paging and service attention messages, restored-screen input gating, bounded queues, explicit reconnect and retained workspace entries | Automatic recovery policy after service loss |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, guarded manual focus switching, 10 Hz background previews, default/compact captures and cell-grid/font/decoration regression on M4 Max via MoltenVK | Cross-cell contextual shaping, selection and accessibility; Linux GUI port is deferred |
| History and input lifecycle | Disk quotas, older/newer paging, live-screen retention, same-PID reattach, real disk-full/corruption recovery; Qt and native macOS composition/paste/focus ownership tests | Archived pages retain their original geometry |
| Retained workspace | Two real shells, GUI creation, four layouts, independent input/history/geometry, shared archive quotas/failure, same-child GUI reopen, removal/adoption and native IME switching; aggregate attention and guarded carousel | Larger workloads and automatic service recovery |
| UI iteration and attention | Isolated source-QML reload, captures, configurable navigation and appearance; live request badges, explicit approval/answer dialog, stale-state gating, per-request drafts, shared queue, pin/pause/snooze controls | Broader request-kind qualification |
| Attention core | C++20 single-source reducer; typed IDs, exact retirement, bounded state, explicit decisions, recovery guards, deterministic workspace ordering and quiet-session fairness | Larger-workload profiling |
| Claude Code hooks | Claude Code 2.1.280 permission and structured-input hooks, terminal-only notices, same-child reconnect and actual GUI capture | No GUI responses or authoritative hook-history reconciliation |
| Codex integration | Managed ordinary TUI, service-owned observer, live desktop approval/input responses, same-child reattachment, source close/restore reconciliation, cancellation and simultaneous live approvals | Broader binary and request-kind qualification |

The [architecture](docs/architecture.md#milestone-3-supervising-two-live-sessions-on-macos)
owns milestone scope and acceptance. Milestone 3 exercises two real shells and two
managed Codex sources with simultaneous approval and structured-input requests.
It retains the UI layouts and the corrections merged in PRs #7, #8 and #9.
Automatic navigation starts off, including after reopening, and request arrival
alone never moves keyboard focus or approves a request.

Qualification history remains in [Milestone 1](evidence/milestone-one.json),
[Milestone 2](evidence/milestone-two.json), the
[PR #7 repairs](evidence/pr7-classification-repairs.json), and the dated receipts
under [evidence/](evidence/). These distinguish terminal behavior, native input,
GPU pixel checks, real adapter traffic, and timing measurements. The
[Codex route comparison](adapters/codex/README.md#integration-route-comparison)
separates terminal operation from attention delivery.

Latency targets remain provisional. Two sessions do not qualify 32-session
capacity, Linux UI, or full response/reconciliation support for another adapter.
Selection, accessibility, cross-cell shaping and automatic recovery after service death remain later work.
Dependency packaging and redistribution notices remain unfinished; this is a
local developer build.

Claude Code can be selected when adding a session. Its permission and input
notices appear in the shared queue; answer them in Claude's terminal. Lapis does
not change Claude's approval policy or install global hooks. The
[Claude hook receipt](evidence/claude-code-hooks.json) records runtime qualification.
See the [hook contract and limitations](docs/architecture.md#claude-code-hooks-an-observation-only-extension).

## Run the window on macOS

After the [dependency setup](CONTRIBUTING.md#desktop-preview), set up and run
everything through one entry point. No environment variables need exporting:

```sh
python3 scripts/lapis.py doctor    # report which dependencies are ready
python3 scripts/lapis.py bootstrap # build the pinned Ghostty terminal library (once)
python3 scripts/lapis.py quality   # repository/Python quality checks (no GUI)
python3 scripts/lapis.py check     # compile, lint, format-check and run CTest
python3 scripts/lapis.py build     # build the desktop app and run its checks
python3 scripts/lapis.py run       # open runtime/workspace-v1.json
python3 scripts/lapis.py ui        # isolated fixture, source-QML reload and attention replay
python3 scripts/lapis.py ui-debug  # launch the isolated fixture in LLDB
python3 scripts/lapis.py ui-check  # bounded preview captures and failure cases
python3 scripts/lapis.py cli-check # CLI/service/GUI acceptance fixtures
```

`just` recipes with the same names wrap the same launcher (`just run`,
`just ui-check`, and so on), and `python3 scripts/lapis.py` alone lists every
command. The launcher locates the bootstrapped terminal dependency and opens
windows on the laptop panel. `just run` uses `runtime/workspace-v1.json`;
pass `--workspace /absolute/path.json` explicitly for another registry. Pass
`--socket`, `--cwd` or an explicit `-- program` only for the legacy single-session
mode. `just native-input` runs the automated macOS keyboard, clipboard and
Japanese IME checks.

An empty workspace offers **Session → Create or adopt…** for a shell, Codex or Claude Code.
Closing the window detaches every live child; removing a workspace entry detaches
that entry without terminating it. Reopening reconnects only: it verifies the
recorded identity and restores the screen, never relaunching a dead service or
replaying input. Input stays disabled until a screen is restored and is guarded
during paste and IME composition while focus changes manually. The pane starts
your login shell: `$SHELL` when set, otherwise the account's shell from the user
database. Type `exit` to end a shell. One GUI may attach per endpoint.

Entries become durable after their first verified connection; wait for Live terminal
before closing a newly created session. The registry is private, owner-locked, atomically written and bounded to 64 KiB
and eight entries. Its reconnect-only JSON stores endpoint, identity,
fingerprint, title, directory and agent—never commands, transcripts or
credentials. The single-session menu keeps explicit reconnect/discover/new-session
actions; the workspace menu separates create/adopt and remove/detach. Builds stay
under `build/`; private sockets, logs and bounded workspace/identity files stay
under `runtime/`.

Use the **Workspace** button to inspect attention from every retained session and
open its source-bound response form. Drafts stay with their exact request while
you move between forms, with the 64 most recent drafts retained in memory. Reviewing a request does not switch the terminal or
approve it. The same panel provides **Automatic**, **Paused**, **Pinned**,
and per-request **Snooze** controls. Automatic switching waits for an active window
and idle input; typing, held keys, paste, composition, dialogs and dragging guard
the current owner. Manual navigation takes precedence. Carousel settings and
snoozes are not persisted.

Use **Older**, **Newer**, and **Live** above the terminal to browse archived
output. History is read only: keys, paste and terminal resize resume only after
returning to Live. Output continues to update the retained live screen while you
browse. Archives use private files under `runtime/history`, with 64 MiB per
session and 256 MiB shared-root page budgets by default. See the
[history and input procedure](CONTRIBUTING.md#history-and-input-qualification) for
limits, recovery and automated native-input acceptance.

To launch Codex with managed attention in its own persistent terminal, use the launcher, which
resolves the build environment and opens on the laptop panel:

```sh
python3 scripts/lapis.py run --codex \
  --socket "$PWD/runtime/codex-v6.sock" --new-session --cwd "$PWD" -- codex
```

Use a dedicated socket for a Codex session. The default socket keeps the shell
that `just run` starts, and an occupied endpoint rejects a different program
before replacing the window, so reusing it for Codex reports a launch mismatch.

Repeat the same command without `--new-session` to reconnect to the same Codex
process. Use `--discover` only to explicitly adopt an existing matching session
when no usable saved identity exists. The `--codex` mode owns a dedicated backend
and observes the ordinary TUI's persistent thread. Open **Requests** to review a
pending command or question. Select a request, then explicitly approve, decline,
cancel or send its answers. Sending does not clear it; source resolution does.
Unsupported request kinds must be answered in the terminal. Lost or unqualified
sources disable the controls; reattachment reconciles before enabling them.

Omit `--codex` and `--claude` for plain terminal launch, which forwards agent options literally.
That mode retains the upstream CLI's backend ownership. Other executables and literal arguments work after
`--`. Explicit programs or `--cwd` require a socket, which the launcher supplies
by default; a launch mismatch is rejected before replacing the existing window.
Plain terminal mode installs no attention hooks or approval settings. The explicit
socket path opens one live session; the default workspace retains multiple live
sessions, while `ui` uses fixture cards.

The default socket is `runtime/desktop-v6.sock`. Older v1/v2/v3/v4/v5 sessions are not
migrated or terminated by this build. See the
[qualification procedure](CONTRIBUTING.md#cli-integration-qualification) for the
optional no-prompt Codex check and current limits.

- [Architecture and near-term plan](docs/architecture.md): component ownership,
  open decisions and acceptance criteria. This is the single implementation plan.
- [Codex investigation](adapters/codex/README.md): protocol routes and evidence.
- [Contributing](CONTRIBUTING.md): shared code standards, setup, checks, profiling and the PR procedure.
- [Quality audit receipt](evidence/code-quality.json): scoped fixes, regression evidence and macOS checks.
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
POSIX descriptor ownership, the production terminal adapter and the attention
reducer; run `python3 scripts/lapis.py bootstrap` once first.
`python3 scripts/lapis.py build` additionally covers PTY, local transport and UI reload/attention
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
