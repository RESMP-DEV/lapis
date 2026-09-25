# lapis

Repository: [RESMP-DEV/lapis](https://github.com/RESMP-DEV/lapis).

lapis is a desktop workspace for live CLI agents. Categories contain ordered
agents; one terminal stage shows the selected agent above a strip of live
previews that is the category's navigation. Normal launch has no shell sessions
or sample cards. Managed Codex is the first adapter; Claude Code reports through
a service-side hook adapter.
macOS is the active live-agent qualification target. Linux desktop work remains
deferred; an explicitly selected Linux host can run isolated software-rendered checks.

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
| PTY and separate session service | Explicit executable/argv/cwd, shell default, resize/paste/exit, failed launch, detached output and same-child reattachment on macOS; after simulated power loss, the login helper resumes observer-verified Codex/Claude conversations and restarts advisory-only agents fresh ([check](scripts/check_restore.py)) | An actual reboot through the login helper, and later Linux qualification |
| Local transport | Version 6 identity/epoch/generation attachment, correlated history paging and service attention messages, restored-screen input gating, bounded queues and explicit reconnect; stale sockets left by a simulated power loss are replaced | Qualification across an actual reboot |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, default/compact captures and cell-grid/font/decoration regression on M4 Max via MoltenVK; mouse selection, copy, wheel history paging and link opening (Qt tests on the Linux test host) | Cross-cell contextual shaping, rectangular/multi-click selection, link hover feedback and accessibility; native Mac selection not yet exercised; Linux GUI port is deferred |
| History and input lifecycle | Disk quotas, older/newer paging, live-screen retention, same-PID reattach, real disk-full/corruption recovery; Qt and native macOS composition/paste/focus ownership tests | Archived pages retain their original geometry |
| UI iteration and attention | Isolated source-QML reload, captures, configurable navigation and appearance; tiles on the stage (drag from the strip, dividers, keys, zoom), dragging cards to reorder and between categories with multi-select, find in the terminal and text size (Qt tests on the Linux test host); live request badges, explicit approval/answer dialog, stale-state gating and draft preservation; live config reload, alert chimes and their repeat rules, Command-K agent search, the usage meter and per-machine dashboard (Qt tests on the Linux test host); usage answers from the installed Codex 0.156.1, Claude Code 2.1.282, Grok 1.0.41, Kimi Code 0.39.1 and OMP 18.2.9, here and on a Linux host over ssh | Automatic carousel and larger session-count qualification; the chimes and usage view have not been seen and heard on a Mac by a test |
| Attention core | C++20 single-source reducer; typed IDs, exact retirement, bounded state, explicit decisions, recovery guards and deterministic ordering | Larger-workload profiling |
| Claude Code hooks | Claude Code 2.1.280 permission and structured-input hooks, terminal-only notices, same-child reconnect, `/clear` continuation and actual GUI capture | No GUI responses or authoritative hook-history reconciliation |
| iPhone app (prototype) | Gateway on the Mac over Tailscale, admitting only the owner's iOS devices; SwiftUI app listing categories and agents, drawing the Mac's cell grid and sending text, paste and keys; the phone joins beside the desktop so both stay in sync (services started by this build); starting an agent in a category from the phone through the Mac's lapis; UI tests in the iOS 26.5 Simulator against real services and the real windowless lapis host with a Mac-side client attached, fake agents, and real Codex and Claude Code on a fake model; installed and used on an iPhone 17 Pro | Agents started before sync are taken over instead; no structured requests or push notifications; starting agents needs a lapis window or the login helper running |
| Mac app package | Qt 6.11.2 built with Vulkan (arm64, macOS 14 or later) with MoltenVK loaded directly; signed with the hardened runtime and notarized; Sparkle 2.10.0 updates signed with an EdDSA key and fed from the latest release; a login item for keeping agents running; release checks for architecture, minimum macOS, links outside the bundle, identifying strings, the update key, the bundled MoltenVK on an M4 Max, the windowless host and a launchd start taking the login shell's PATH; 0.1.0 opened and used with a live agent by a person | An update installed through Sparkle (the first comes with the release after 0.2.0); notifications, the login item and the Finder and editor actions not yet exercised by a check on a Mac; macOS 14 and 15 untested |
| Codex integration | Managed ordinary TUI, service-owned observer, live desktop approval/input responses, same-child reattachment, source close/restore reconciliation, cancellation and simultaneous live approvals; [installed binary qualification](evidence/codex-binary-update.json) | Broader binary and request-kind qualification |

[Desktop evidence](evidence/desktop-preview.json),
[UI refinement evidence](evidence/ui-preview.json),
[reconciled UI and test evidence](evidence/reconciliation.json) and
[adapter evidence](evidence/terminal-adapter.json) delimit these observations.
The [downloadable Mac app](#download-for-macos) carries its own Qt, MoltenVK
and dependency notices; a build from the repository is a developer build.
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

## Download for macOS

The Mac app is on the [releases page](https://github.com/RESMP-DEV/lapis/releases/latest):
open `lapis-macos-arm64.dmg` and drag lapis to Applications. It needs an Apple
silicon Mac with macOS 14 or later; it is signed with a Developer ID and built
and checked on macOS 26.5. The app keeps `lapis.json` and its runtime state in
`~/.lapis` (or `$LAPIS_HOME`), and uses the agent CLIs you already have
installed. Opened from Finder or the Dock, it starts agents with your login
shell's environment, so they find the same tools and keys as in your terminal.
It checks the latest release for updates once a day and installs them after
asking (Sparkle). Settings can keep agents running at login. The phone gateway
is set up from this repository; the app does not include it yet.
[Contributing](CONTRIBUTING.md#build-the-mac-app) describes how the app is built,
signed and notarized.

## Run the workspace

To build it yourself, after the [dependency setup](CONTRIBUTING.md#desktop-preview):

```sh
uv run --no-project python scripts/lapis.py doctor
uv run --no-project python scripts/lapis.py bootstrap
uv run --no-project python scripts/lapis.py build
uv run --no-project python scripts/lapis.py run
```

Use **New agent** (Command-T), choose a harness with arrows and Return, then
enter a project folder and press Return. Claude, Codex, OpenCode, Grok, OMP,
Antigravity and Kimi appear in the picker, in that order; missing executables
are marked unavailable. Escape returns from the folder step to harness selection. The field
starts at your platform home directory; arrows select folder suggestions and
Tab or Return completes the selected folder. The browse button opens the native
folder picker. Chips under the folder choose a model, from the models the CLI
itself lists for your account (its default first; Kimi, OpenCode and OMP from
their config, recent models and roles), and one of three approval modes:
Accept edits, Auto or Full access (the first time, Full access, or
`newAgent.mode`), each passed as that CLI's own flag. The mode
stays when the CLI changes; a CLI without it (OMP, OpenCode and Antigravity
have no Auto, Kimi and OpenCode no Accept edits) uses its nearest, less access
first. The next agent starts with the same CLI, mode and model. Everything else
stays in each CLI's own config.
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

Drag a card from the strip onto an edge of the stage to tile that agent beside
the one shown, as iTerm2 splits a tab (onto a tile's middle to swap it in).
Dividers drag to share the space, and agents are resized once, when the drag
ends. Command-D starts an agent like the selected one (folder, CLI, model and
mode) tiled to the right, Command-Shift-D below it; Command-Control-arrows move
between tiles, Command-Shift-Return fills the stage with one, and a tile's ×
or a drag back to the strip takes it off the stage while it keeps running. A
strip agent that is not tiled takes the selected tile when you click it. Each
category keeps its tiles. Cards also drag along the strip to reorder, onto a
category in the rail to move there, and onto the rail's **+** to start a new
category; Command-click and Shift-click pick several to drag together, and
categories drag up and down the rail.

Command-F finds text in the selected terminal: the page shown first, then older
history pages (Return goes older, Shift-Return newer). Command-plus, minus and
zero change the text size. Files dropped on a terminal paste their quoted paths.
The card menu and Commands show an agent's folder in Finder, open it in your
editor (`editor` in `lapis.json`, else the first of Cursor, VS Code, Zed,
Windsurf and Sublime Text installed) or copy its path. Command-Shift-T reopens
the last agent you closed, resuming its conversation where its CLI can.

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
and remembers that choice. Command-V remains paste. As in a browser, Command-T
creates an agent (its tab defaults to the project path) and Command-N creates a
category; the **+** under the last category does the same. Command-comma
opens Appearance, and Command-R reloads configuration. Command-K finds an agent
as you type: by the letters of its name, folder, category, CLI or machine, or by
text on its screen, and Return shows it. Linux uses Control-Shift
bindings. Terminal Control chords and Command-left/right editing stay with the
agent. Bindings remain configurable in `lapis.json`.

`lapis.json` applies as soon as it changes, whether Appearance, you or an agent
edits it. Besides bindings and appearance it holds the defaults for new agents,
per machine where they differ:

```json
{
  "newAgent": {
    "harness": "codex",
    "folder": "~/dev",
    "mode": "edits",
    "machines": {"devbox": {"folder": "~/work"}},
    "models": {"codex": ["gpt-6-astra", "gpt-6-sol"]}
  },
  "alerts": {"sound": true, "finished": true, "repeat": 3, "notify": true},
  "editor": "Cursor",
  "keepAwake": true,
  "usage": {"show": true, "meter": ["codex", "claude", "grok"], "machines": ["devbox"]}
}
```

An agent that needs you chimes (two taps, rising), and again every few seconds
while the request waits and you are looking elsewhere, up to `repeat` times; a
Codex or Claude turn that ends out of view chimes once, quietly. While lapis is
in the background the same moments post a notification (`notify`); clicking it
shows the agent. Appearance has the switches and a Play button for each. In the
downloaded app it also keeps agents running at login and checks for updates,
which install from the latest release. `keepAwake` keeps the Mac from
sleeping while it is plugged in, so the phone can reach it.

`usage.show` puts plan usage under the categories: every plan a CLI here is
signed in to (Codex, Claude, Grok and Kimi, plus every account OMP's logins
hold), each showing what is left of its tightest window: a green bar with
plenty left, orange under 30%, red under 10%. `usage.meter` picks which appear and in
what order; one that is not signed in is left out. Clicking it opens a
dashboard per machine, this Mac and each ssh host in `usage.machines`: every
account's windows with what is left, reset times, and when the current pace
runs out or how much it leaves at the reset, and the
Codex and Claude tokens used on that machine today, this month and per day
for 30 days, by model. Each CLI is asked through its own interface every five
minutes, without a prompt, a hook or a saved session; another machine's CLIs
are asked over ssh, and its transcripts are counted there by its own python3.
There are no prices.

Close the window (its close button or Command-Q) to detach. Reopen it to
reconnect the same agents and restore category selections and window
placement. Closing the window does not stop agents; Command-W on an agent does.
If an agent's session service is gone when lapis opens (after a reboot or a
crash), lapis restarts it in its card, like a restored terminal tab: Codex,
Claude, Grok, OpenCode, OMP, Kimi and Antigravity resume their saved
conversation with their own resume option; other CLIs, or a conversation with
no saved transcript yet, start fresh in the same folder. Explicit resume arguments
remain authoritative; lapis adds a resume selection only when none is already
present. Codex transcript lookup stays within the `sessions/YYYY/MM/DD` layout
and does not follow directory symlinks. Each service records
the conversation beside its endpoint (`<endpoint>.resume`) from the Codex
observer, the Claude hook adapter, or the `agent_checkpoint` sequence that
iTerm2 restore hooks print. For Codex builds lapis has not qualified, and
Codex agents whose service predates these records, lapis reads the thread from
the rollouts its app-server holds open, following `/new` and `/resume`.
Command-W is what removes an agent for good. Restore runs when lapis opens.
To have agents come back at login without opening a window, install the login
helper once with `uv run --no-project python scripts/restore_at_login.py
install`. It runs `lapis_desktop --restore-agents --serve`, restarts the agents
whose services died with the Mac (restart, crash, power cut), and then keeps the
workspace without a window so the phone can start agents; opening lapis takes
the workspace from it and it exits. It leaves agents that are still running
alone, and it needs a logged-in user session.
The development login helper preserves custom `CODEX_HOME`, `CLAUDE_CONFIG_DIR`
and `LAPIS_HISTORY_ROOT` values from the installing shell. Reinstall that helper
after changing those locations.
An agent that has ended or cannot be reached keeps its last screen, with a bar
on the stage giving the reason and the key that closes it. **Restart agent**
in Commands starts it again in the same card, resuming its conversation the
same way.

A new agent's CLI updates itself first (`claude update`, `omp update`, `grok
update`, `kimi upgrade`, `opencode upgrade`, `agy update`), at most every 30
minutes per CLI; the card reads **Updating Claude…** until the agent starts on
the new version, and results go to `runtime/harness-updates.log`. Codex is the
exception: lapis observes only Codex builds it has qualified, so it keeps the
qualified build and starts Codex with its update prompt turned off
(`check_for_update_on_startup=false`).
Explicit supported-CLI creation uses the same update queue; reconnect and
discovery do not. Pass `--no-harness-updates` to keep a chosen CLI installation
unchanged. Queued agents wait until the updater and its installer children have
stopped; restarting a queued agent cannot bypass that wait.

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
workspace metadata and window geometry live under ignored `runtime/` (in
`~/.lapis/runtime/` for the downloaded app). Builds and
local captures live under ignored `build/`. Runtime state is not project config
and must not be copied between hosts.

For development, `lapis.py quality`, `check`, `ui-check` and `cli-check` remain
available. `build` is the full desktop validation gate. During edits use a focused
CMake target and `ctest -R ... --no-tests=error` on the Linux test host. `linux-gui` wraps supplied
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

## Use it from your iPhone

The iPhone app (`apps/ios`) talks to a small gateway on the Mac
(`apps/remote/lapis_remote.py`) over Tailscale. There is nothing to sign in to:
the gateway listens only on the Mac's Tailscale address and serves a request
only when `tailscale whois` names the Mac owner's login on an iOS device. The
phone must be signed in to Tailscale with the same account as the Mac.
Keep Tailscale active on both devices when using HTTP; its WireGuard connection
provides transport encryption. The configured gateway address is trusted input,
and arbitrary LAN hosts are outside this transport contract. An explicit HTTPS
URL keeps HTTPS, and unsupported URL schemes are rejected.

Keep the gateway running at login with
`uv run --no-project python apps/remote/launch_agent.py install` (`status`,
`uninstall`; log in `~/Library/Logs/lapis-remote.log`). With the phone unlocked
on the same Wi-Fi or a cable,
`uv run --no-project python scripts/install_ios_app.py` builds, signs and
installs the app with your development profile, set to reach this Mac by its
Tailscale name. To work on it in Xcode, run `xcodegen` in `apps/ios`
(`Config/Local.xcconfig`, not committed, holds your team and default host).

The app lists each category's agents as cards with the harness mark and the
folder in path form (`~/dev/infinity`; an agent reached over ssh shows its host
first, `devbox:~/lapis`). Opening an agent shows its screen at phone width, with
a key bar (esc, ^C, arrows, enter, backspace, tab, ^U, ^D) and a message field
that pastes and presses Enter; dictation works there. Scrolling up loads the
agent's earlier output from the service's archived history. **Send screen to
Mac** in the agent's menu saves a screenshot and the exact screen data under
`runtime/phone-captures/` for debugging what the phone drew. The phone
reports capture failures in the same alert used for delivery results. Rotation
updates the terminal grid while composing; keyboard appearance alone keeps its
row count. The phone
joins the agent beside the desktop: both show the same screen, either can type,
and the terminal takes the size of the device in use. Opening the agent on the
phone gives it the phone's size; closing it or locking the phone, activating the
lapis window, moving the pointer over the agent, or typing on the Mac gives it
back. Agents started
before this build run services that cannot be joined; opening one on the phone
takes it from the desktop (its card offers **Reconnect agent**), and the phone
says so. Restarting such an agent gives it a service that can. The Mac must be
awake.

The **+** at the top offers a new agent or a new category (added on the Mac
without moving its window). Swiping an agent left offers **Close**, and a full
swipe closes it, as Command-W does on the Mac. A new agent from the phone: pick the
machine (this Mac, or an ssh host from your ssh config and shell history,
reachable and most used first), the CLI, the category, a folder (starting at
that machine's `newAgent` folder), a model and an approval mode, and it opens
as a new tab in that category in lapis on the Mac, then on the phone once it
runs. Another machine keeps the chosen CLI when it has it, and the mode and
each CLI's model are remembered. The folder starts at `newAgent.folder` (the
machine's own in `machines`, else the one for every machine) when that machine
has it, and opens its own screen: that preset, pinned and marked, then the ten
folders where you have started the most Codex, Claude Code and lapis agents,
most first, then the machine's folders to browse (visible ones
alphabetically, hidden ones last), or a search by a few of their letters;
both run on the phone. The list, the CLIs, the
machines, the folder indexes and each running agent's screen are fetched in
the background, so opening the sheet or an agent does not wait. A shown agent on the Mac keeps the stage. This needs lapis running on
the Mac: an open window, or the login helper (`scripts/restore_at_login.py`),
which after a restart keeps the workspace without a window until lapis opens.
`uv run --no-project python scripts/check_ios_remote.py [--codex] [--claude]`
runs the app's UI tests in a headless simulator against disposable services.

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
licenses. The Mac app carries Qt under the LGPL-3.0, MoltenVK under Apache-2.0
and Ghostty's VT library under MIT; their notices are in
[third_party/](third_party) and in the app under `Contents/Resources/Notices`,
and each release attaches the Qt source archives the app was built from.
