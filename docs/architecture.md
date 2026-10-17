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
| Web surfaces | Deferred design only | Do not build until process ownership, package, and engine gates close |

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
| Web surfaces | CEF 8037 is a provisional engine candidate only | Build W1 only after R1/R4 close; then prove service-owned lifecycle, wire v1, injection determinism, storage isolation, and import consent |
| Performance thresholds | Existing 20/30/35 ms input budget is the only reviewed budget | Promote switch, frame, sustained-output, or memory numbers only from named candidate workloads and tails |

## Deferred work

The following are not release dependencies and must not displace the delivery
order: new adapters, a provider-routing framework, a second terminal engine,
web views, multiple windows, automatic carousel, widgets, triggers, Keychain
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
