# Architecture and near-term plan

This file owns the decisions, executable order, and contracts that still govern
lapis. It is intentionally not a history log. [Status](status.md) owns what is
implemented or exercised, [evidence](../evidence/) owns dated observations, and
Git history retains superseded narratives. If a claim is not needed to build,
review, qualify, or constrain the next slice, it does not belong here.

## Product scope

Lapis is a local macOS workspace for supervising live CLI agents. Correct
terminal behavior, stable session identity, explicit attention decisions, and
durable process ownership come before additional integrations. The supported
early-access objective is a signed Apple Silicon app that can install, launch
local Codex and Claude Code sessions, retain work through GUI replacement and
update, recover after service failure, diagnose its own limits, and
survive a measured eight-session workload.

Responsiveness is the first product priority, followed by ergonomics and then
visual polish. Input-to-presentation and warm-switch tails are measured claims
only when a reviewed receipt names the workload, source, artifact, display, and
percentiles. Terminal glyphs are never animated. Motion is short,
interruptible, and respects the user's reduced-motion setting.

The current reference machine is an M4 Max with 128 GB unified memory and a
120 Hz display. That is a measurement context, not a minimum requirement or a
capacity claim. Linux remains a deferred platform: portable boundaries are
kept, but no Linux desktop support is claimed until a named distro, toolkit
backend, display server, and workload pass their own qualification.

## Component ownership

| Piece | Owns | Boundary |
| --- | --- | --- |
| Session service | PTYs, terminal state/history, adapter connections, snapshots, and session lifecycle | Runs independently of the desktop and continues consuming output while detached |
| Terminal-engine adapter | Ghostty parser/reflow behavior behind a small lapis interface | Upstream types and its unstable C API stay isolated |
| Agent adapters | Tool-specific observation, response, and reconciliation capabilities | Each tool/version is probed independently; success with one adapter is not adapter independence |
| Attention policy | Source pending requests, ordering, aging, and reconciliation | Desktop combines and presents requests but never changes their response authority |
| Desktop | Layout, keyboard/IME focus, selection, accessibility, rendering, and starting one session service per agent | Targets explicit sessions; never owns PTYs, and quitting leaves services running only where the OS permits background execution |
| Login helper | `scripts/restore_at_login.py`: a LaunchAgent that runs the windowless host at login to restart agents whose services died with the Mac | Leaves running agents alone; it does not watch or restart services after login |
| Tool status | Read-only installed/version observations from `harness_catalog` | Never configures tools, changes launch semantics, or qualifies model access |
| Renderer | Glyph/texture caches, changed-region drawing, and bounded previews | Consumes immutable snapshots; never mutates parser or PTY state |

Messages crossing service IPC are validated by their receiving owner. The GUI
does not retain PTY handles. Input encoding follows the terminal engine's
current modes. Shared code lives under `services/session/`, `adapters/`, and
`apps/desktop/`; a new library needs a concrete consumer and a CMake target
that uses `lapis_project_options`.

## Architecture decisions

| Decision | Current position | Constraint or next gate |
| --- | --- | --- |
| Platform | macOS 14 or later on Apple Silicon | Linux requires a separate named platform qualification |
| Language and UI | C++20 with Qt 6.11 Quick and public scene-graph text nodes | No private Qt renderer APIs or OpenGL-only terminal surface |
| Terminal engine | Pinned Ghostty `libghostty-vt` | Keep the unstable C API and dependency notices isolated; replay cases remain the compatibility gate |
| Rendering | Qt Quick, Vulkan through MoltenVK on macOS | Runtime API verification is required; presentation and resize tails still need candidate measurements |
| Session wire | Internal version 6 framing with epochs, generations, history paging, and attention messages | Incompatible changes need rejection/migration tests and a private endpoint |
| Codex route | Ordinary TUI under the service's PTY; Codex's notify program reports finished turns through the terminal, as for a remote agent | Observation only: requests are answered in Codex's terminal; there is no per-agent app-server, approval ping or interrupted-turn signal. The desktop's short-lived app-server model-list probe is outside this agent route |
| Claude route | Hook/terminal observation with explicit answers in Claude's terminal | No GUI response routing until a supported protocol is version-pinned and qualified |
| Process birth | The window (or the windowless host) starts one detached session service per agent; agents survive window quitting only where the OS permits background execution, and the login helper restarts those whose services died with the Mac | Restart of a crashed session service is not implemented: the agent keeps its last screen until it is restarted or lapis next opens; macOS 27 also requires the Allow in the Background conditions in [install](install.md) |
| Settings | Tracked `lapis.json` defaults plus private runtime workspace/window state | Machine-local state never becomes a portable credential or session transcript |
| Native browser surfaces | CEF 8037 is a provisional engine candidate; no runtime is implemented | Run the isolated W0 probe first; product and service wiring wait for R1/R4 |

The old terminal-engine comparison and pinned source receipts remain in
[evidence](../evidence/). They justify the selected engine; they are not a
standing instruction to rebuild the engine or add a second renderer.

## Production delivery order

The next release is a vertical durability slice, not a feature expansion. Work
is valuable only when its exact source and artifact can pass the named gate.

| Order | Executable slice | Observable exit |
| --- | --- | --- |
| 1 | Package the pinned service, then exercise a live signed update transition | Manifest, appcast, app, DMG, and six candidate-gate receipts bind one immutable source/artifact pair; rollback preserves or explicitly disposes of live work |
| 2 | Close candidate input gates in parallel | Large paste, bracketed-mode changes, cancellation, reconnect, and focus/IME transitions preserve exact destination and PTY bytes |
| 3 | Run the declared eight-session support workload | Bursty output, history paging, reconnect, diagnostics, warm/idle transitions, resource bounds, Codex/Claude continuity, and input/switch/frame tails have one candidate-scoped receipt |
| 4 | Publish only the qualified local slice, then schedule controlled 32-session scale | Early-access reports identify exact source/artifact; wider capacity claims wait for the separate 32-session result |

Process lifetime stays as it runs today: the window starts each session
service, services outlive the window where the OS permits background
execution, and the login helper restores agents after login. Restart of a
crashed session service is not implemented, and no
step above adds it. Step 2 may proceed as parallel focused repairs, but those
repairs cannot claim candidate readiness until they run against the assembled
package from step 1. A merged PR, source test, or worker self-report is not a
release qualification receipt.

## Native browser utilization

The next web objective is a native, service-owned browser surface: agents use
bounded `lapis web` CLI operations against headless views and never require an
external browser, an MCP transport, a visible GUI, or a caller-named browser
profile. The research basis is
[web-surface research](../evidence/web-surface-research.json). It is design
evidence only: no CEF archive was fetched or executed for that receipt.

The ladder below is ordered and non-collapsing. Passing one checkpoint does not
promote the next, and no checkpoint makes Lapis a general-purpose browser.

| Checkpoint | Bounded slice | Exit evidence |
| --- | --- | --- |
| W0: engine probe | Resolve the current exact macOS arm64 stable artifact in CEF branch 8037, then fetch and digest it; inspect SDK, codesign, and redistribution inventory; run one isolated offscreen CEF host on fixture content. Exercise BGRA `OnPaint`, trusted input synthesis, and in-process DevTools with remote-debugging TCP disabled. Measure installed size, launch/frame cost, and one-view memory. A BGRA-to-Qt upload is the first qualified display path; IOSurface/Vulkan interop is optional follow-up evidence, not a prerequisite. | `evidence/web-engine-probe.json` records the resolved version, artifact digest, toolchain, real frames/input/replies, observed limits, and cleanup. A failed result selects the recorded fallback decision; it does not silently swap engines. |
| W1: headless vertical slice | After R1 and R4 close, add `services/web/` with one service-owned engine tree and isolated per-view request contexts. Register the deterministic bootstrap host-side for new documents, with a heartbeat and explicit degraded state. Expose only `status`, `open`, `navigate`, `snapshot`, `click`, `wait`, and `close` over a private versioned socket; the CLI emits one bounded JSON envelope and meaningful exit codes. Snapshots carry content-derived refs that the service re-resolves at action time; a changed page returns a stale-ref diff. | Real local fixtures, including full navigation and SPA-style route change, pass through the CLI without a GUI. A deliberately broken bootstrap makes injection health explicit; malformed/stale actions never execute. |
| W2: GUI compositing | Add a `WebSurface` beside the terminal surface. Upload bounded dirty BGRA regions through public Qt scene-graph interfaces, use the same focus/IME ownership policy, and keep previews throttled, non-interactive, and never view-resizing. | Background Qt and native macOS input evidence cover typing, IME, selection, and refusal while the user owns input; terminal latency gates do not regress. |
| W3: attention and approvals | Register `adapter_id` `web`; classify login/captcha, form submission, download, upload, clipboard, payment, and navigation-risk actions. Decision tokens bind the current view, navigation epoch, target, and arguments. | Risky fixture actions block for an explicit user decision and retire only that request. Page text can request attention but never act or redirect focus. |
| W4: credential import and isolation | Permit explicit user-initiated import only. Start with a `storageState` interchange file; Chromium-family disk import requires a later reviewed probe under Lapis' final code signing. Cookies and storage enter one in-memory isolated context before first navigation, are never exposed by a verb/event/receipt, and are never copied as a profile directory. | User-consented fixture login passes; fixture credential bytes appear in no CLI output, event, log, or receipt; cross-view isolation and revocation/destruction tests pass. |
| W5: workload and publication | Exercise the assembled package with eight concurrent web views under controlled navigation/output load, then record memory, frame/action latency, and idle cost. Complete dependency notices, inventory/SBOM, and the pin/update policy. | One candidate-scoped receipt reports p50/p95/p99 and recovery on service/GUI restart. A 32-view workload is a later separate scale claim. |

CEF is the only current W0 candidate because the research found its public
offscreen paint, trusted input, and in-process DevTools surfaces in one
out-of-process engine tree. The September survey artifact remains published, but
the live index has a newer branch-8037 stable; W0 resolves that current exact
version instead of inheriting the survey timestamp. WKWebView remains a display
candidate without a qualified headless control path, and a separately supervised
Qt WebEngine host is the recorded alternative if CEF fails its empirical gate.
Neither is a
silent substitution. External-browser CDP attachment remains a different
capability and does not solve profile ownership or process cleanup.

## Codex route

A local Codex agent is Codex's own TUI under the session service's PTY. The
service starts it with `-c notify=[...]` before every other argument (unless
the arguments already set `notify`), makes a 32-hex-digit hook nonce, binds it
to the agent's `TerminalHookChannel` itself, and puts the nonce, the Python
relay (`services/session/src/terminal_hook_relay.py`) and the PTY device in
the agent's environment. After each finished turn Codex runs that program: the
relay writes the event's bounded metadata (`type`, `thread-id`, `turn-id`) to
the device as an OSC 7717 frame, which the service removes from the output,
and then runs the person's own effective `notify` from
`$CODEX_HOME/config.toml` layered with the selected
`$CODEX_HOME/<profile>.config.toml` (read without `tomllib` on python3 before
3.11). `NotifyTurns` marks the turn
finished; submitted input returns the status to the output estimate. The
event's `thread-id` is the conversation to resume, recorded as an observer
identity because the nonce is never printed to the terminal. The nonce is not a
secret from Codex or its configured notify command: both inherit it. Codex's
default shell environment also inherits it, although
`shell_environment_policy` can filter it from tool commands. The nonce rejects
terminal text that does not carry the channel value; it does not identify the
emitting process or isolate the notify program from Codex. A thread whose
rollout's first line names a parent is a subagent's and is skipped. Remote
Codex agents use the
same relay through their login shell's preamble (`withRemoteHooks`).

Saved arguments and the `lapis-codex-v1` launch fingerprint are unchanged, so
a service started by an older lapis (a `codex app-server` beside a `--remote`
TUI) still attaches; its requests show read-only until the agent is reloaded.
The notify route reports no turn start, approval request or interrupted turn;
between finished turns the status is estimated from output, and every request
is answered in Codex's terminal. Upstream has marked `notify` legacy in favor
of the hooks engine below; if a release removes it, Codex status falls back to
the output estimate.

### Codex native hooks (research)

Codex upstream ships a Claude Code-style hooks engine, enabled and stable. The
surface below is source-verified at inspected checkout `1f69eb089d`, whose
upstream merge basis is `17f29f3ff8`; upstream stable at the survey time was
0.163.0. It is recorded in
[codex-hooks-surface evidence](../evidence/codex-hooks-surface.json). It is
design evidence only: lapis installs no hooks and claims no hook response
capability. Codex does not replay hook runs through thread history.

Verified surface: twelve events (PreToolUse, PermissionRequest, PostToolUse,
Pre/PostCompact, SessionStart/End, UserPromptSubmit, SubagentStart/Stop,
Stop, Interrupt) with command, MCP-tool, prompt, and agent handlers, sync or
async execution, thread or turn scope, per-event JSON schemas, and
Claude-compatible decision JSON. PreToolUse can block a call and rewrite its
input. PermissionRequest handlers return Allow or Deny, folded
any-deny-wins, decided in core before Guardian or user approval UI. Stop
carries a continuation guard, a changed-files list, and an internal
memory-consolidation target, so a Stop hook never proves task completion;
handler-added context spills to disk past a 2,500-token default. Since the
surveyed 0.157.1, the app-server gained `hooks/list` (per-cwd hook inventory)
beside the existing `hook/started`/`hook/completed` run notifications, and
hooks additionally distribute through the plugin marketplace with
content-hash trust.

A hook that reports turn starts and approval requests through the same
terminal relay would recover what the notify route lacks; one that answers
approvals changes the agent's execution policy and stays per-session opt-in.
The earlier app-server H0 receipt is retained in
[Codex hooks H0 evidence](../evidence/codex-hooks-h0-20261018.json).

## Release gates

**R1, process lifetime.** The window starts each agent's session service and
does not own it afterwards: quitting the window leaves the service and its
agent running where the OS permits background execution, and clients attach by
validated identity. On macOS 27 that means the Allow in the Background and
old-app conditions in [install](install.md); otherwise macOS ends the agents.
The login helper restarts agents whose services died with the Mac. Restart of a
crashed session service is not implemented. Qualify GUI close/crash/replacement,
service death, login/logout, and reboot. Same-child survival and replacement
with a new epoch are different outcomes and must be reported separately.

**R2, input integrity.** Paste admission, mode-aware encoding, attachment
identity, cancellation, and reconnect reporting stay service-owned. Candidate
acceptance compares actual PTY bytes and bracket markers under a full queue,
slow reader, focus/IME transition, and disconnect. Background Qt evidence does
not replace native macOS presentation.

**R3, account identity.** lapis neither chooses nor holds CLI credentials:
each CLI starts with the sign-in its machine or ssh host already has, and a
remote command saved with a retired plan preamble loses it at its next start.
Uncertain approval operations never replay automatically.

**R4, package and update provenance.** The manifest binds source, version,
dependency inventory, notices, app, DMG, appcast, and six external candidate
receipts: downloaded-asset verification, notarized staged verification,
fresh-user Finder launch, installed update with live sessions, update-failure
recovery, and registry/history migration rollback. No current receipt satisfies
these gates.

**R5, workload and support.** The first measured workload is eight local
sessions: one active stage plus seven previews, with bursty output, history
paging, and reconnect. Record rates, duration, warm/idle transitions,
input/switch/frame tails, memory, descriptors, disk, and idle CPU/GPU. A
fresh-user environment must diagnose missing CLIs, registration/permission
failures, and service disconnect without developer tools.

## Open choices

| Topic | Current position | Decision gate |
| --- | --- | --- |
| Codex native hooks | Research only; the notify route reports finished turns | A per-session hook relayed through the terminal would add turn starts and approval requests; it needs a live receipt for relay loss and trust, and lapis never installs global hooks |
| Native browser surfaces | W0 is an isolated engine probe; W1 begins only after R1/R4 close | Promote each checkpoint with its named runtime, input, isolation, credential, and workload receipt |
| Performance thresholds | Existing 20/30/35 ms input budget is the only reviewed budget | Promote switch, frame, sustained-output, or memory numbers only from named candidate workloads and tails |

## Deferred work

The following are not release dependencies and must not displace the delivery
order: new adapters, a provider-routing framework, a second terminal engine,
web views, native Codex hook routes, multiple windows, automatic carousel,
widgets, triggers, Keychain
fill or Touch ID, Linux UI, and broader OS/CLI support claims. Existing
prototype code and receipts remain valid dated observations, but no prototype
becomes an advertised capability through this plan.

Optional prediction features remain off the critical path.
Their provider calls, account identity, logs, and input behavior need separate
reviewed contracts before they are enabled by default.

## Verification discipline

1. Start from the relevant [status](status.md) row and the required-check
   matrix in [CONTRIBUTING](../CONTRIBUTING.md#checks), not from an old
   narrative section.
2. Select the smallest check that can fail on the contract being changed.
   Focused source, sanitizer, real adapter, background UI, native input/GPU,
   package, reboot, and soak evidence are distinct and are not interchangeable.
3. Reuse unchanged evidence only with its original source, platform, dirty
   state, and non-claims. Re-run the assembled integration gate once after
   dependent changes combine.
4. A worker's green test is implementation evidence, never parent acceptance.
   The integrating owner reviews the diff and observes real process or UI state.
5. Every non-trivial batch writes a sanitized receipt. Candidate receipts bind
   exact source and artifact hashes, commands, OS/CLI versions, results, known
   limits, and recovery instructions.
6. Update status only when behavior passes the check that owns its claim. Use
   implemented, exercised, measured, and proposed precisely; silence or a live
   process is not automatically completion.

## Contracts to preserve

### Session identity and process boundaries

Every session has a stable lapis ID. Sessions own PTYs and terminal engines;
every agent, Codex included, is its ordinary CLI. Observers (Claude Code's hook
socket, the terminal hook channel) are service-owned, so GUI detachment does
not invalidate a healthy source.

The window starts a session service only for a create-mode attach, passing the
session ID it chose, and then attaches with that ID; later attaches use the
saved descriptor. A failed start leaves the agent disconnected with the reason
shown; the connection does not launch the service again by itself.

### Verified adapter contracts

Adapters declare observation, response, and reconciliation separately. Supported
protocols are preferred; verified hooks are next; launcher exits, bells, and
prompt heuristics are advisory only. Preserve source request, thread, turn,
item, connection-epoch, and typed-ID identity. Deduplicate events, detect gaps,
and retire only the request actually resolved.

Source transport loss makes requests stale and disables replies until
reconciliation. A returning GUI refreshes service state before enabling actions.
Attention attachment identities remain separate from source epochs so delayed
input cannot target a reused request. Codex and Claude qualification is pinned
to the installed binary/version and request kind actually exercised.

### Attention and keyboard ownership

Only desktop focus policy assigns keyboard ownership. Agent output can request
attention but cannot redirect input. Typing, held keys, paste, IME composition,
selection, dragging, and modal work defer automatic focus changes. Lapis never
splits a paste or composition or steals OS focus while another application is
active.

Session positions stay stable. Manual navigation, pinning, snoozing, and an
opt-in carousel must not starve quiet sessions. Viewing or acknowledging a
request never approves it. No current adapter routes a response: every agent
takes its answers in its own terminal, and the service refuses a decision an
older desktop sends.

The window draws status, not attention: an agent's mark shows what it is doing
(working, done, waiting on an answer, lost, unknown, ended) and never whether
the person looked at it (decided Nov 11, 2026, after unseen dots and counts
re-flagged agents the owner had already read). The desktop still keeps one
unseen mark per agent for Tab, the chime and `agent_state.json`, owned by
`Workspace` (the rule is written above `Workspace::looking` in `workspace.cpp`):
a finished turn or new request marks it once unless the person is looking
(stage agent, window active, input within two minutes); looking by any path
clears it for good; transient statuses and reconnects never re-mark a seen turn.

### Terminal rendering and pacing

The service screen is authoritative; desktop snapshots are replaceable caches.
Preserve Unicode, shaping, wide cells, escape-sequence behavior, resize, IME,
selection, clipboard, and accessibility. Focused terminal glyphs render 1:1 on
a whole-device-pixel cell grid. Only bounded previews may scale snapshots down,
and scaled previews never resize PTYs.

An inline agent renderer can rebuild its scrollback on a PTY resize by erasing
every kept row and replaying the transcript (OMP's default
`tui.resizeScrollback: rebuild`, and Kimi). Local OMP agents receive a
lapis-owned `PI_CONFIG_FILES` overlay that sets `tui.resizeScrollback:
preserve`, so a resize repaints only the viewport. A user's existing overlays
are kept, and the lapis overlay is appended after them. Remote OMP agents keep
the remote machine's own configuration; lapis does not copy its local overlay
over ssh.

Typing while scrolled back returns the live screen, or sends a full-screen
program to its bottom on the same ordered connection before the key. That
return debt is tracked for programs that report the mouse (wheel notches) and
for those that scroll with arrow keys, and is reset when the program's screen
changes. `scrollToBottomOnTyping` gates the behavior.

Rate limits measure publish-to-publication time and publish the newest state at
the deadline; they never queue stale snapshots or tax the first event of a
burst with a full interval. Debounce is a separate named feature where waiting
for quiet is intentional. Hidden panels stop drawing while their output remains
bounded and consumable. Static scenes sleep.

### Storage, credentials, and diagnostics

Tracked `lapis.json` contains defaults only. Private workspace, window, and
support state stays under ignored runtime paths with bounded size, owner-only
permissions, atomic replacement, and explicit corruption behavior. Credentials
never enter Git, transcripts, logs, support exports, or architecture receipts.
Diagnostics are read-only until an explicit user action mutates state, and
redaction is tested against representative private paths and identifiers.

## History policy

On October 17, 2026, this document replaced a 5,987-line accumulated work log
with this executable plan. Removed dated narratives remain available from Git
history at `44cbcdd053066504fb95cf565f9062000cda7f83b`; their evidence receipts
remain unchanged under [evidence](../evidence/). Future dated entries are
allowed only when they record a decision or gate that cannot be expressed in
the sections above, and they should be folded into those sections once their
consequences are settled.
