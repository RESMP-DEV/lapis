# lapis

Repository: [RESMP-DEV/lapis](https://github.com/RESMP-DEV/lapis).

lapis is a desktop workspace for live CLI agents. Categories contain ordered
agents; one terminal stage shows the selected agent above a strip of live
previews that is the category's navigation. Normal launch has no shell sessions
or sample cards. Managed Codex is the first adapter; Claude Code reports through
a service-side hook adapter.
macOS is the live-agent qualification target; Linux tests run on anvil using
an isolated software-rendered display.

The priority is **responsiveness, then ergonomics, then visuals**. Design for
high-end M-series hardware and high-refresh displays; use generous, bounded RAM
caches to keep sessions ready for immediate switching. Use minimal, mostly opaque
surfaces and short, interruptible animations, drawing on Apple and Material design.

Agents start with [AGENTS.md](AGENTS.md), which defines implementation boundaries,
parallel work areas, milestones, and verification requirements. `CLAUDE.md` links
to that same file so Codex and Claude Code share one set of project instructions.

## Current status

The category workspace supports creating Codex agents, moving and renaming tabs,
remembering each category's selection, restoring the same service-owned processes,
and displaying source-derived activity and pending requests. Window placement is
saved locally and narrow windows use a category selector. The Command theme uses
dark opaque surfaces, restrained teal accents and short selection feedback.
On macOS the native title bar follows the selected theme while retaining the
standard window controls and drag region. There is no tab row: the strip under
the stage shows each agent's latest lines (the rows ending at its cursor) at
most four times a second, in tab order. Moving through it keeps part of the
neighboring card in view, like Neovim's `sidescrolloff`; the last card is
**+** for a new agent. A card whose agent finished a turn or started needing a
response while another was selected pulses until you select it, and its
category shows a pulsing dot. The Dock badge counts agents in any category that
finished unseen or wait on a request, and a new request bounces the Dock icon
once while lapis is in the background. Codex reports working/finished through its
observer; Claude agents run under the session service's Claude Code hook
adapter (`--claude`), which reports turns, permission prompts and input
requests; other harnesses show an output estimate labelled
**Output active** / **Quiet**. Command-W closes the focused agent, not the
window. Agents always start as top-level sessions: when lapis itself was opened
from inside another agent's terminal, that agent's session markers (Claude
Code's child-session and transcript flags, Grok, OpenCode, OMP and Codex
sandbox markers) are removed before any agent starts.
Categories read as group labels and tabs as sessions; keyboard focus, activity,
pending requests and lost connections have separate colors and shapes. One
configurable fixed-width font serves the terminal and machine readouts.

The underlying terminal and managed-attention milestones retain their earlier
qualification receipts. The two-session Milestone 3 workspace (flat manifest,
shared attention queue and guarded carousel) was
[qualified on macOS](evidence/milestone-three-workspace.json) and then replaced
by this category workspace; its service, Codex and Claude Code work carries over. The category workspace's current checks and remaining
acceptance are recorded in [the evidence](evidence/agent-workspace.json) and
[architecture](docs/architecture.md#daily-use-agent-workspace-direction-september-21-review).

| Component | Exercised | Remaining |
| --- | --- | --- |
| POSIX resources and terminal adapter | Descriptor ownership and 14 Ghostty adapter cases on macOS and Linux ARM64 | Broader terminal compatibility |
| PTY and separate session service | Explicit executable/argv/cwd, shell default, resize/paste/exit, failed launch, detached output and same-child reattachment on macOS | Recovery after service loss and later Linux qualification |
| Local transport | Version 6 identity/epoch/generation attachment, correlated history paging and service attention messages, restored-screen input gating, bounded queues and explicit reconnect | Recovery after service failure/reboot |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, default/compact captures and cell-grid/font/decoration regression on M4 Max via MoltenVK; mouse selection, copy, wheel history paging and link opening (Qt tests on anvil) | Cross-cell contextual shaping, rectangular/multi-click selection, link hover feedback and accessibility; native Mac selection not yet exercised; Linux GUI port is deferred |
| History and input lifecycle | Disk quotas, older/newer paging, live-screen retention, same-PID reattach, real disk-full/corruption recovery; Qt and native macOS composition/paste/focus ownership tests | Archived pages retain their original geometry |
| UI iteration and attention | Isolated source-QML reload, captures, configurable navigation and appearance; live request badges, explicit approval/answer dialog, stale-state gating and draft preservation | Automatic carousel and larger session-count qualification |
| Attention core | C++20 single-source reducer; typed IDs, exact retirement, bounded state, explicit decisions, recovery guards and deterministic ordering | Larger-workload profiling |
| Claude Code hooks | Claude Code 2.1.280 permission and structured-input hooks, terminal-only notices, same-child reconnect, `/clear` continuation and actual GUI capture | No GUI responses or authoritative hook-history reconciliation |
| Codex integration | Managed ordinary TUI, service-owned observer, live desktop approval/input responses, same-child reattachment, source close/restore reconciliation, cancellation and simultaneous live approvals; [installed binary qualification](evidence/codex-binary-update.json) | Broader binary and request-kind qualification |

[Desktop evidence](evidence/desktop-preview.json),
[UI refinement evidence](evidence/ui-preview.json),
[reconciled UI and test evidence](evidence/reconciliation.json) and
[adapter evidence](evidence/terminal-adapter.json) delimit these observations.
Dependency packaging remains unfinished; this is a local developer build.
The old multi-layout preview is no longer the product surface. The explicit
`--ui-preview` developer fixture exercises the same category UI with synthetic
data; it is never added to a normal workspace. See the
[Milestone 2 plan](docs/architecture.md#milestone-2-attention-and-codex-plan)
for the underlying attention route and its acceptance evidence.
The first checkpoint implemented the standalone attention core and exercised real
Codex request round trips. [Milestone 2 evidence](evidence/milestone-two.json) now
records assembled service/desktop acceptance on macOS. The
[PR #7 review receipt](evidence/pr7-review.json) covers subsequent lifecycle,
startup, queue-boundary and multi-question fixes. Its
[follow-up receipt](evidence/pr7-review-followup.json) records request-ID and
reconciliation fixes, order-independent question checks and refreshed validation.
The [thread-repair receipt](evidence/pr7-thread-repairs.json) records initial-thread
classification, config separator validation, preserved cleanup diagnostics and
the disposition of the remaining review threads.
The [latest PR #7 receipt](evidence/pr7-classification-repairs.json) records
post-binding thread isolation, transport and fixture cleanup, backend exit
diagnostics, and the evidence-based disposition of the new review batch.
Following the quality cleanup in PR #6, Milestone 2
now includes the production Codex adapter, session-service integration and explicit
desktop response controls. Request arrival never moves keyboard focus. See the
[attention test procedure](CONTRIBUTING.md#codex-attention-qualification).
Latency and warm-switch targets remain provisional.

Qualification history remains in [Milestone 1](evidence/milestone-one.json),
[Milestone 2](evidence/milestone-two.json), the
[PR #7 repairs](evidence/pr7-classification-repairs.json), and the dated receipts
under [evidence/](evidence/). These distinguish terminal behavior, native input,
GPU pixel checks, real adapter traffic, and timing measurements. The
[Codex route comparison](adapters/codex/README.md#integration-route-comparison)
separates terminal operation from attention delivery.

Claude Code's permission and input notices appear under **Requests**; answer
them in Claude's terminal. Lapis does not change Claude's approval policy or
install global hooks. The [Claude hook receipt](evidence/claude-code-hooks.json)
records runtime qualification. See the
[hook contract and limitations](docs/architecture.md#claude-code-hooks-an-observation-only-extension).

## Run the workspace

After the [dependency setup](CONTRIBUTING.md#desktop-preview):

```sh
uv run --no-project python scripts/lapis.py doctor
uv run --no-project python scripts/lapis.py bootstrap
uv run --no-project python scripts/lapis.py build
uv run --no-project python scripts/lapis.py run
```

Use **New agent** (Command-N), choose a harness with arrows and Return, then
enter a project folder and press Return. Codex, Claude, OMP, Grok, Kimi, OpenCode,
Gemini and Antigravity appear in the picker; missing executables are marked
unavailable. Escape returns from the folder step to harness selection. The field
starts at your platform home directory; arrows select folder suggestions and
Tab or Return completes the selected folder. The browse button opens the native
folder picker. There is no name or model field. Each harness uses its existing
login, default model and execution policy; model changes stay inside its own TUI.
To add your own flags to every new agent of a harness (lapis adds none itself),
set `harnessArguments` in `lapis.json`, for example
`{"harnessArguments": {"claude": ["--dangerously-skip-permissions"]}}`; shell
aliases do not apply, because lapis starts the executable directly.
New tabs show the harness mark and a home-relative project path such as `~/dev/lapis`.
Codex and Claude Code have verified activity integration. Other harnesses run
their native CLI and show an output estimate instead of guessing turns.
Each agent gets a
separate service, identity and endpoint. No approval settings or global hooks are
changed; a Claude agent's hooks are passed to that one process by its service. Unsupported Codex binaries keep explicit status/response limitations;
there is no qualification bypass.

Open **Commands** to search or scroll through actions and their configured
shortcuts. Create categories with **New category** there. The category rail and that category's
agent strip are separate navigation levels. Every category remembers its selected
agent. Commands and the card's context menu rename, reorder or move the selected agent without
restarting it. Attention counts do not reorder categories or steal input.

On macOS, Command-Option-left/right or Command-Shift-up/down changes category;
Command-Shift-[ and ] moves through the category's agents. Command-1 through 4
selects a category. Command-J jumps to the next agent, in any category, with a
pending request, or else one that finished while you were elsewhere.
Command-W closes the focused agent: a running agent is
confirmed, then ended by its session service.
Command-Shift-P opens Commands; Command-B hides or shows the category sidebar
and remembers that choice. Command-V remains paste. Command-N creates an agent
(its tab defaults to the project path), Command-Shift-N creates a category, Command-comma
opens Appearance, and Command-R reloads configuration. Linux uses Control-Shift
bindings. Terminal Control chords and Command-left/right editing stay with the
agent. Bindings remain configurable in `lapis.json`.

Close the window (its close button or Command-Q) to detach. Reopen it to
reconnect the same agents and restore category selections and window
placement. Closing the window does not stop agents; Command-W on an agent does.
If an agent's session service is gone when lapis opens (after a reboot or a
crash), lapis restarts it in its card, like a restored terminal tab: Codex,
Claude, Grok, OpenCode, OMP, Kimi and Antigravity resume their saved
conversation with their own resume option; other CLIs, or a conversation with
no saved transcript yet, start fresh in the same folder. Each service records
the conversation beside its endpoint (`<endpoint>.resume`) from the Codex
observer, the Claude hook adapter, or the `agent_checkpoint` sequence that
iTerm2 restore hooks print. For Codex agents whose service predates these
records, lapis reads the thread from the rollout its app-server holds open.
Command-W is what removes an agent for good. Restore runs when lapis opens;
add lapis to Login Items to have it happen at login.
An agent that has ended or cannot be reached keeps its last screen, with a bar
on the stage giving the reason and the key that closes it. **Restart agent**
in Commands starts it again in the same card, resuming its conversation the
same way.

Upgrading lapis does not disturb running agents: quit the old build and open
the new one, and it reattaches to the same processes. Launch fingerprints,
the service protocol, the workspace registry and resume records are kept
compatible across builds, and a test pins the fingerprints.

**Requests** appears when the selected agent needs a response. Open it, select a
request, then explicitly approve, decline, cancel or send answers. Opening or
selecting never approves. Stale sources disable responses. **Turn finished**
means the agent finished a turn, not that the task or process ended. Claude
Code's idle reminder after a finished turn is not a request. A new Codex agent
reads **No prompt yet** until its first turn. Agents
started in the same folder are numbered in cards and menus.

Command-Left/Right move to the start or end of the line, Command-Backspace
deletes to the line start and Command-Delete to the line end (Control-A, -E, -U
and -K to the agent). Command-click (Control-click on Linux) a web link in agent
output to open it,
including links that wrap across rows. Drag across the terminal to select text,
or double-click a word; Command-C
(Control-Shift-C on Linux) copies it, and typing clears it. The mouse wheel pages
through history (read-only; scrolling past the newest page or typing returns to
the live screen, and the typed key reaches the agent), or sends arrow keys to a
full-screen program on the alternate screen.
History actions are also under **Agent**. Private
workspace metadata and window geometry live under ignored `runtime/`. Builds and
local captures live under ignored `build/`. Runtime state is not project config
and must not be copied between hosts.

For development, `lapis.py quality`, `check`, `ui-check` and `cli-check` remain
available. `build` is the full desktop validation gate. During edits use a focused
CMake target and `ctest -R ... --no-tests=error` on anvil. `linux-gui` wraps supplied
commands in a private Xvfb/Openbox software-rendered display. Native Mac input
checks are a separately scheduled acceptance step.

Standalone shell fixtures require `--development-shell` and are not offered in
the product. Explicit managed probes retain `--codex --socket ... --cwd ...
-- codex` and `--claude ... -- claude`. Existing v6 endpoints are neither automatically adopted nor terminated
by the new category registry.

Omit `--codex` and `--claude` for plain terminal launch, which forwards agent
options literally and installs no attention hooks or approval settings.

- [Architecture](docs/architecture.md): product direction and acceptance.
- [Codex investigation](adapters/codex/README.md): protocol and qualification.
- [Contributing](CONTRIBUTING.md): setup, tests, profiling and review.

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
