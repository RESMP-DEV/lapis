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
update, recover after daemon/service failure, diagnose its own limits, and
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
| Desktop | Layout, keyboard/IME focus, selection, accessibility, and rendering | Targets explicit sessions; never owns PTYs or service lifecycle in the production route |
| Supervisor | One-session birth admission, desired state, convergence, and private ownership records | Not a generic control plane; launchd owns registration, not session semantics |
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
| Codex route | Managed ordinary TUI with a service-owned observer and app-server backend | Observation, response, and replay capabilities are declared separately |
| Claude route | Hook/terminal observation with explicit answers in Claude's terminal | No GUI response routing until a supported protocol is version-pinned and qualified |
| Process birth | Experimental same-UID one-session supervisor daemon exists | The first production client is headless `lapis supervisor`; launchd and GUI routes follow it |
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
| 1 | Complete supervisor lifecycle behind `LAPIS_BUILD_SUPERVISOR_RUNTIME` and expose headless `lapis supervisor status/stop/reset` | Real daemon, service, and terminal PIDs are distinct; status is read-only, failed birth is never replayed, explicit stop/reset cleans the private slot, and malformed/foreign-UID requests fail closed |
| 2 | Add `DirectBirthRoute`, `SupervisorBirthRoute`, and `BirthTicket` without changing preview or direct tests | A GUI-observed supervised session keeps its child and v6 identity across GUI close/crash/replacement; no fallback silently rebirths it |
| 3 | Make explicit per-user launchd registration observable | Enabled, disabled, unregistered, and unknown states are reconciled without spawning a second daemon or terminating children implicitly |
| 4 | Package the daemon and pinned service, then exercise a live signed update transition | Manifest, appcast, app, DMG, and six candidate-gate receipts bind one immutable source/artifact pair; rollback preserves or explicitly disposes of live work |
| 5 | Close candidate account and input gates in parallel | Missing selected credentials refuse before replacing a healthy session; large paste, bracketed-mode changes, cancellation, reconnect, and focus/IME transitions preserve exact destination and PTY bytes |
| 6 | Run the declared eight-session support workload | Bursty output, history paging, reconnect, diagnostics, warm/idle transitions, resource bounds, Codex/Claude continuity, and input/switch/frame tails have one candidate-scoped receipt |
| 7 | Publish only the qualified local slice, then schedule controlled 32-session scale | Early-access reports identify exact source/artifact; wider capacity claims wait for the separate 32-session result |

Only step 1 is the current implementation batch. Step 5 may proceed as parallel
focused repairs, but those repairs cannot claim candidate readiness until they
run against the assembled package from steps 1-4. A merged PR, source test, or
worker self-report is not a release qualification receipt.

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
| W1: headless vertical slice | After R1 and R4 close, add `services/web/` with one supervisor-admitted engine tree and isolated per-view request contexts. Register the deterministic bootstrap host-side for new documents, with a heartbeat and explicit degraded state. Expose only `status`, `open`, `navigate`, `snapshot`, `click`, `wait`, and `close` over a private versioned socket; the CLI emits one bounded JSON envelope and meaningful exit codes. Snapshots carry content-derived refs that the service re-resolves at action time; a changed page returns a stale-ref diff. | Real local fixtures, including full navigation and SPA-style route change, pass through the CLI without a GUI. A deliberately broken bootstrap makes injection health explicit; malformed/stale actions never execute. |
| W2: GUI compositing | Add a `WebSurface` beside terminal tiles. Upload bounded dirty BGRA regions through public Qt scene-graph interfaces, use the same focus/IME ownership policy, and keep previews throttled, non-interactive, and never view-resizing. | Background Qt and native macOS input evidence cover typing, IME, selection, and refusal while the user owns input; terminal latency gates do not regress. |
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

## Codex native hooks

Codex upstream ships a Claude Code-style hooks engine, enabled and stable. The
surface below is source-verified at inspected checkout `1f69eb089d`, whose
upstream merge basis is `17f29f3ff8`; upstream stable at the survey time was
0.163.0. It is recorded in
[codex-hooks-surface evidence](../evidence/codex-hooks-surface.json). It is
design evidence plus the later H0 runtime boundary below: lapis installs no
production hooks and claims no hook response capability. A separately resumed
observer receives hook-run notifications when attached before the hook-bearing
turn; Codex does not replay hook runs through thread history, so an observer
that attaches after a hook has finished cannot recover that run. Hook work runs
only on a binary that passed the Codex requalification procedure.

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

| Checkpoint | Bounded slice | Exit evidence |
| --- | --- | --- |
| H0: inventory and delivery boundary | On a requalified binary, the existing app-server observer enumerates bounded `hooks/list` metadata and parses `hook/started`/`hook/completed`. The qualification creates one harmless hook, attaches an observer before each hook-bearing turn, and requires exact thread/turn start/completion identity on both the turn-owner and pre-attached observer connections | A disposable-session receipt records inventory, command execution, owner and pre-attached observer delivery, the no-history-replay boundary, and cleanup; no production `[hooks]` configuration is written by lapis |
| H1: per-session decision hook | A lapis-owned PermissionRequest command hook configured only inside the session's disposable Codex home can explore direct decision-policy integration distinct from passive observation | Live allow/deny round trips on a disposable session record any-deny folding and no-decision passthrough; no global or shared hook installation exists |
| H2: subagent visibility | Consume pre-attached SubagentStart/SubagentStop agent identity and type as advisory signals within one Codex session | A receipt maps subagent lifecycles without promoting them to lapis sessions or claiming recovery for events missed before attachment |

An approval-answering hook changes the agent's execution policy, so H1 is
strictly per-session opt-in and never a global installation. The H0 receipt for
binary `da306dac…` is retained in
[Codex hooks H0 evidence](../evidence/codex-hooks-h0-20261018.json). Upstream
has marked legacy `notify` for removal in favor of this engine. These
checkpoints do not displace the delivery order above.

## Release gates

**R1, process lifetime.** The GUI is not the production process owner. A
per-user supervisor owns birth and convergence; clients attach by validated
identity. Qualify GUI close/crash/replacement, daemon SIGKILL, service death,
login/logout, reboot, restart-storm bounds, and explicit operator disposition.
Same-child survival and replacement with a new epoch are different outcomes and
must be reported separately.

**R2, input integrity.** Paste admission, mode-aware encoding, attachment
identity, cancellation, and reconnect reporting stay service-owned. Candidate
acceptance compares actual PTY bytes and bracket markers under a full queue,
slow reader, focus/IME transition, and disconnect. Background Qt evidence does
not replace native macOS presentation.

**R3, account identity.** A missing, unreadable, or malformed selected
credential must refuse before changing a healthy session or acting as another
account. Uncertain reset and approval operations never replay automatically.
Real provider/account qualification is separate from fixture refusal tests.

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
| Supervisor endpoint authority | Workspace/CLI selects the validated private endpoint; daemon owns the admitted process | Move endpoint selection only with a multi-session identity and reconnect migration receipt |
| Supervisor registration | Explicit launchd registration follows the headless client and GUI route | Qualify enabled, disabled, missing plist, live-socket disagreement, logout/login, and reboot before automatic registration |
| Credential environments | First birth requires an empty service environment | A reviewed contract must distinguish user consent, tool identity, same-UID exposure, and revoke/recovery behavior before admission |
| Codex thread switch | Explicit proposal only | Select an exact persistent thread, rotate epoch, reconcile by exact ID, invalidate old tokens, and qualify foreign/archive recovery before UI navigation |
| Codex external-agent import | Session-only importer and qualification probe exist | Consent, dedicated server ownership, duplicate reconciliation, launch/resume, and failure recovery must qualify before onboarding UI |
| Codex native hooks | H0 inventory and delivery boundary are qualified; H1 per-session relay is the next candidate | A reviewed live receipt must qualify relay loss, trust and decision semantics; decision hooks are per-session opt-in and lapis never installs global hooks |
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

Optional prediction and saved-reset features remain off the critical path.
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

Every session has a stable lapis ID. Terminal sessions own PTYs and terminal
engines; structured Codex sessions own their app-server process/connection and
conversation state. A structured backend does not reproduce the TUI, and a
separate app-server does not observe independently launched CLIs by default.
GUI detachment does not invalidate a healthy service-owned source connection.

Supervisor state owns the one-session slot, launchd owns registration, and the
client reconciles both without treating one as a mirror of the other. Unknown
registration or birth state refuses mutation. An unqualified one-shot birth is
never retried automatically because a lost reply could become a second child.

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
request never approves it. Decision-capable responses route through the
originating adapter; hook-only tools may require the user to answer in the
original terminal.

### Terminal rendering and pacing

The service screen is authoritative; desktop snapshots are replaceable caches.
Preserve Unicode, shaping, wide cells, escape-sequence behavior, resize, IME,
selection, clipboard, and accessibility. Focused terminal glyphs render 1:1 on
a whole-device-pixel cell grid. Only bounded previews may scale snapshots down,
and scaled previews never resize PTYs.

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
