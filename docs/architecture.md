# Architecture and near-term plan

This is the single implementation plan for lapis. See the
[current status](../README.md#current-status) for what has been implemented and exercised.
macOS is the active target. Milestones 1 and 2 are qualified for the recorded
single-session scope: a persistent terminal and managed Codex attention with
explicit desktop responses. The quality baseline from PR #6 remains in force.
[Milestone 3](#milestone-3-supervising-two-live-sessions-on-macos) has
retained entries, workspace attention and guarded opt-in navigation assembled on
macOS. Its two-session acceptance and measurement limits are recorded in the
[workspace receipt](../evidence/milestone-three-workspace.json).
A Linux desktop port is deferred; the headless engine has
already been exercised on Linux, but the session service and desktop have not.

## Product philosophy

**Responsiveness first, ergonomics second, visuals third.** Correct terminal
behavior and keyboard ownership remain requirements. Optimize the time from an
action to visible feedback, including slow outliers during output bursts. Visual
effects must fit within that interaction budget.

Use a minimal blend of Apple's restraint and Material's clear hierarchy. Favor
opaque surfaces, readable typography, generous spacing and subtle borders; avoid
glass, heavy shadows and decorative motion. Snappy, continuous feedback contributes
to perceived responsiveness, but never substitutes for fast input or correct state.
Animate position and color, not terminal glyphs. Begin feedback immediately, keep
transitions interruptible, and never gate keyboard input on animation completion.
The current preview uses 130–140 ms ease-out hover/color transitions and a two-pixel
lift. Future pane transitions should preserve spatial continuity without bounce;
honor reduced-motion preferences before enabling animated navigation. These are
initial design choices, not a new animation framework or measured performance claim.

Design for high-end M-series machines (Pro/Max/Ultra class), with this M4 Max,
128 GB unified memory and 120 Hz display mode as the initial reference. These
are observed reference-machine properties, not minimum specifications or measured
lapis performance. If Linux is ported later, it will need its own named
hardware/workload reference.
Aim at 120 Hz on capable displays and follow higher refresh rates where qualified.

Use RAM generously to buy immediate switching: retain every live terminal's
current screen and recent history, share glyph caches, and prewarm useful previews
and render resources. Switching away does not unload a session. Warm caches in
bounded background work after the first view becomes interactive. Give caches
generous configurable limits; account for CPU/GPU unified memory once, alongside
separate agent-process usage. Under pressure, evict cold previews/history first
and spill older history to disk while protecting the active working set.

The first interactive view must include timing markers. Warm-switch latency and
frame budgets are **provisional hypotheses**, not measured guarantees or PR/release
gates. The [measurement procedure](../CONTRIBUTING.md#measuring-responsiveness)
owns the numbers, endpoints and workloads. Revisit them after the first real
terminal/switching baseline; require a reviewed, evidence-backed decision before
promoting any number to an acceptance threshold.

## Component ownership

| Piece | Owns | Boundary |
| --- | --- | --- |
| Session service | Processes, PTYs, terminal state/history, adapter connections and session lifecycle | Runs independently of the desktop; keeps consuming output while detached |
| Terminal-engine adapter | Existing engine's parsing, reflow and mode-aware input encoding | Inside the service; upstream types stay behind a small lapis interface |
| Agent adapters | Tool-specific activity, attention requests, responses and reconciliation | Service-owned; capabilities verified per tool/version |
| Attention policy | Service-owned pending requests; desktop aggregation, navigation aging/cooldowns and presentation snooze | Source state stays authoritative in each service; only desktop focus policy assigns keyboard ownership |
| Desktop | Layout, navigation, keyboard ownership, IME, selection and accessibility | Receives snapshots; explicitly targets input and decisions to a session |
| Renderer | Glyph/texture caches, terminal drawing and previews | Desktop render thread; consumes snapshots, never mutable parser objects |

```mermaid
flowchart LR
    CLI[CLI processes] <--> PTY[PTY backend]
    subgraph Service[Persistent session service]
        PTY <--> Engine[Terminal engine and history]
        Agent[Agent adapters] --> Attention[Attention state and queue]
        Engine --> State[Session snapshots]
        Attention --> State
    end
    Tool[Tool protocol or hooks] <--> Agent
    State --> Desktop[Desktop and renderer]
    Desktop -->|Targeted input and resize| Engine
    Desktop -->|Explicit decisions| Agent
```

Desktop messages cross service IPC and are validated there; the GUI never owns
PTY handles. Input encoding uses the engine's current terminal modes. Keep code
in `services/session/`, `adapters/` and `apps/desktop/`; introduce libraries or
subdirectories when implementation needs them. Use the
[contributor handoff procedure](../CONTRIBUTING.md#large-changes-and-parallel-contributors)
for ownership, shared contracts and integration checks across large changes.

## Direction and open choices

| Topic | Current position | Decision gate |
| --- | --- | --- |
| Platform | macOS active; Linux desktop deferred | Keep portable platform boundaries; qualify Linux separately if and when the port is scheduled |
| Desktop | C++20 and Qt 6.11.2 Quick with public QSGTextNode terminal drawing | macOS Vulkan visual checkpoint exercised; macOS performance qualification remains |
| Engine | Pinned Ghostty `libghostty-vt` selected for the first adapter | Eight-case macOS/Linux replay passes; isolate unstable C API and resolve dependency-notice gaps |
| Service language | C++20 around Ghostty's C API | C++20 consumer exercised on both target platforms; no Rust linkage required |
| Transport | Version 6 local framing with session/epoch/generation identity, readiness, history paging, attention messages and retained workspace entries | Automatic service recovery remains deferred |
| Codex mode | Managed ordinary TUI with a dedicated service-owned backend and observer; desktop responses qualified in Milestone 2 | Milestone 3 qualifies routing across two independent sessions; other binaries and request kinds need separate evidence |

The [research receipt](../evidence/terminal-research.json) retains pinned upstream
sources. Contour is the closest structural reference; WezTerm supplies service/GUI
separation examples. These are source observations, not local runtime results.
Ghostty's external VT C API is explicitly unstable and needs a Zig toolchain;
Contour currently defaults to C++23 and its renderer uses Qt private APIs.
Qualify engines independently of upstream GUIs. Keep C++20 until an evidenced
decision changes it.

The [engine experiment](../evidence/terminal-engine-probe.json) records pinned
builds and runtime results. The production adapter now reuses that pinned library;
distribution packaging remains unfinished.

## Portable rendering

Terminal emulation and rendering are separate choices. Ghostty VT or Contour's
engine maintains terminal state; the lapis surface draws snapshots using Qt Quick.
Keep common drawing code on public Qt scene-graph interfaces and portable shaders.
The preferred common GPU path to qualify is Vulkan:

| Platform | GPU paths to qualify | Priority |
| --- | --- | --- |
| macOS | Vulkan through MoltenVK, which translates to Metal | First implementation |
| Linux | Vulkan through the GPU driver | Deferred port; not claimed or currently scheduled |

macOS has no native Vulkan driver; [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
supplies a portability implementation over Metal and converts SPIR-V shaders.
This adds a loader/runtime dependency and feature constraints, not a demonstrated
performance failure. Use the common supported feature set and query capabilities.
Qt already abstracts GPU APIs, so choosing Vulkan does not by itself remove
platform-specific input, font or process work.

The [local probe](../evidence/vulkan-probe.json) established device discovery. The
[desktop checkpoint](../evidence/desktop-preview.json) now establishes actual Qt
terminal drawing on the Apple M4 Max, using Vulkan through MoltenVK 1.4.2 and
Qt 6.11.2. The application verifies the selected API at runtime. Its maintained
surface uses public QSGTextNode/QTextLayout interfaces and Qt's glyph cache, not
upstream private rendering code. Static scenes keep their retained nodes; new
snapshots replace only changed row text nodes. Each surface retains the current
owned snapshot and the previous rendered snapshot for comparison. GUI-owned
objects, preedit text and geometry are copied into an immutable render state and
handed to the render thread under a standard mutex. The render callback never
dereferences a GUI-owned session. Allocation and frame-time gains still need
measurement; preview throttling and full shaping remain follow-up work.

Qt's default macOS transaction layer produced five-second display-lock stalls in
this Vulkan window. Setting `QT_MTL_NO_TRANSACTION=1` selected the plain
CAMetalLayer path and removed the warnings in the same threaded-render-loop
capture. This workaround is isolated to macOS startup and tied to Qt 6.11.2;
revalidate it on upgrades. Continuous resize, presentation timing and packaging
still need qualification. Linux rendering, if scheduled later, needs its own evidence. Compare native Metal if later matched measurements warrant it; no
second custom renderer is needed for that comparison.

Avoid OpenGL-only `QQuickFramebufferObject` and upstream private renderer APIs.
Use [Qt's backend selection](https://doc.qt.io/qt-6/qtquick-visualcanvas-adaptations.html)
and verify the selected API at runtime; a silent fallback is not Vulkan evidence.

Portability also covers PTY/process lifecycle, local IPC, font fallback, IME,
clipboard and accessibility. macOS/Linux can share POSIX concepts with OS-specific
implementations. Qualify Linux on a named distro and test Wayland/X11 separately.
Keep native handles and platform APIs out of the engine, session contracts and
attention policy.

## Near-term work

### Starting code structure

Keep components together by ownership. Each compiled component gets its own CMake
target; public headers live under `include/lapis/<component>/`, implementation
under `src/`, and behavioral cases under `tests/`. Add these directories as code
needs them. Every first-party target uses `lapis_project_options`.

| Location | First responsibility | Status |
| --- | --- | --- |
| `services/session/src/platform/posix/` | Native descriptor ownership, then PTY launch/I/O/resize/reaping | `UniqueFd` on macOS/Linux; Qt-owned PTY launch/I/O/resize/reaping exercised on macOS |
| `tools/terminal_probe/` | Shared headless workloads and independent Ghostty/Contour consumers | Implemented; pinned builds and eight-case replay on macOS/Linux |
| `services/session/src/terminal/` | Wrap the selected engine's parsing, mode-aware input and screen extraction | Implemented as `lapis_terminal`; 14 behavioral cases on macOS/Linux ARM64 |
| `services/session/include/lapis/session/` | Owned commands, session identity and snapshots for clients | Owned terminal values implemented; internal version 6 attachment/snapshot/history and attention framing under `src/transport/` |
| `services/session/src/` | Service event loop and session lifecycle, then local IPC | Separate one-terminal service with explicit argv/cwd and bounded local transport on macOS |
| `apps/desktop/` | Minimal Qt view, input routing and terminal surface | Live enlarged shell and static carousel composition; macOS Vulkan capture |
| `adapters/codex/` | Codex protocol mapping and attention delivery | Ordinary TUI plus isolated shared-server request/response/reconnect exercised; service adapter and IPC approval/input round trips exercised |

The first target, `lapis_session_platform`, is an internal C++20 library with no
Qt, GPU or engine dependency. Its `UniqueFd` owns one native descriptor, closes
it on destruction and transfers ownership by move. The owner thread synchronizes
access; a borrowed descriptor is never closed by its caller. Tests use actual
POSIX pipes to exercise transfer, I/O, EOF, release and replacement. This is a
resource primitive, not a terminal or persistent service. No empty service or
desktop executable is advertised as an application.

### Current visual checkpoint

The desktop starts a separate Qt Core service on a private local socket. That
service owns QProcess, a nonblocking POSIX PTY, and the Ghostty terminal. Closing
the GUI only detaches the local socket: the service continues draining output.
The enlarged pane and previews represent live workspace entries; the isolated
preview retains labeled fixtures. One GUI may attach per endpoint. A matching
attachment replaces the previous connection; a mismatched launch is rejected
first. The default workspace registry is `runtime/workspace-v1.json`, and an
explicit `--workspace` path selects another registry. Registry paths are validated
as private trusted filesystem paths, without importing Unix socket length or
file-type constraints. Actual service endpoints still obey platform socket limits.
Exported endpoints are canonical (including macOS `/tmp` aliases), and manifest
identity fields use canonical lowercase hex. Explicit `--socket` or
program flags retain the legacy single-session path. Stable session IDs, service
epochs and attachment generations bind each connection; automatic recovery
remains later work. This wire format is internal and provisional.

Qt event loops own their respective objects. PTY reads yield after 64 KiB and
input dispatch after 64 frames. Writes have a 1 MiB queue; text messages are at
most 64 KiB. Snapshot frames are limited to 8 MiB, 32,768 cells and 65,536 codepoints.
The service coalesces updates on a 16 ms timer with one snapshot in flight; a slow
GUI does not block PTY parsing. The measured input-to-frame baseline exceeds one 120 Hz
interval; it is not a responsiveness-target pass. Recent history uses the adapter's
bounded memory budget. Older primary-screen
rows move to the bounded disk archive described in the completion contract below.

The GUI decodes owned snapshots and routes text, navigation, Control-letter input,
paste and resize. The focused pane chooses the PTY dimensions; scaled previews do
not resize it. Cell-grid/font fallback has native Vulkan regression coverage.
Qt and automated macOS keyboard/paste/Japanese IME ownership are tested;
selection/copy and a terminal accessibility tree remain open. The renderer retains
static scene nodes and lets Qt schedule updates and brief hover transitions.
The controlled latency probe now correlates received input, service processing,
snapshot application and frame submission. Pixel-visible presentation is not
measured. The visual checkpoint alone does not complete milestone 1.

### UI refinement checkpoint

Both scoped UI changes are implemented and exercised on macOS; maintainer visual
review remains before connecting real requests. They reuse Qt Quick, owned terminal
snapshots and the Vulkan surface, with no new runtime dependency. Milestone 1
is qualified on macOS; see the [assembled receipt](../evidence/milestone-one.json).
The [UI receipt](../evidence/ui-preview.json) records checks and
measurement limits; commands live in [Contributing](../CONTRIBUTING.md#ui-tuning-and-debugging).

#### Isolated iteration and debugging

`--ui-preview` constructs synthetic sessions without a live connection or service.
It rejects `--smoke-input`. The existing live window stayed attached to its shell
while the isolated capture suite ran. `just ui` loads source QML; manual reload
creates a candidate view and preserves the previous view on load failure. Accepted
reloads preserve geometry and defer destruction of the previous engine so a QML
caller can finish safely. Normal launches use bundled resources.

The development-only **preview scenario v1** has stable card IDs, owned terminal
snapshots and bounded requests keyed by ID. It is separate from service IPC and
the future attention policy. Arrival, duplicate, two requesting cards, matching
resolution and reset are explicit replay actions. There are at most eight pending
requests per fixture session; duplicate IDs do not change the request or cue serial.

Captures wait for actual frames, have a 15-second deadline and report load/save
failures. LLDB launch, breakpoint, stack inspection, resume and separate attach/
detach were exercised against the isolated app. The GUI and service remain
separate debugging targets.

#### Compact header and attention cue

An 18-pixel strip holds connection status and preview tools; it omits repeated
session names and paths. Carousel cards have no title or repeated preview badge:
terminal content, working directory and attention state identify them. No user
naming is required. Cards keep their positions and dimensions. A new synthetic
request produces two red edge pulses over 1.4 seconds, then a steady rim and
readable pending label. The cue occupies the card's existing background and never
changes the terminal geometry or keyboard owner.
Duplicate requests and ordinary output do not restart it; resolving one request
leaves others pending. Focusing a card never approves or resolves a request.

Reduced motion uses a steady marker. The app reads the macOS accessibility
preference at startup and activation; a preview override can also enable it.
Linux preference integration is still unqualified. Cue motion pauses while the
window is hidden or inactive and stops after the finite sequence. Captured default,
compact, arrival, two-card and reduced-motion states passed focus/geometry checks.

The trace records bounded, GUI-thread observations of `frameSwapped`. Those times
include signal delivery and scheduling; they are not physical presentation or input
latency. The receipt includes interval distributions and a short idle observation,
not a latency guarantee or sustained CPU/GPU benchmark. Targets remain provisional.

#### Next steps and ownership

After visual review, introduce rebindable next-session, previous-session and
next-needing-attention actions with persisted settings and focus/input tests.
Tab or a Tab chord remains a candidate, not a selected default: plain Tab belongs
to shell completion/TUIs unless explicitly rebound. Keep positions stable and never
split paste/IME operations. Selecting a session never sends a response.

Keep real attention adapters, automatic carousel movement and workspace-wide
prompt/approval routing out of this fixture. Multiple live sessions and guarded
manual navigation are qualified for checkpoint 3A in the
[workspace receipt](../evidence/milestone-three-workspace.json).

For parallel changes, commit the shared contract first and assign disjoint files
with one coordinator/build owner. Preview hosting lives in `ui_preview.*`, layout
in `qml/`, and capture/check tooling in `ui_capture.*`, desktop tests and
`scripts/check_ui_preview.py`. Review partial worker output before integration.
Use `just desktop` for integrated C++ changes, targeted ASan for reload lifetimes,
and `just ui-check` for capture behavior. QML-only iteration uses reload and focused
visual checks; reuse pinned dependencies and update these documents in place.

### Terminal adapter v0

The checkpoint repair is merged in PR #1. The first production adapter lives in
`services/session/`: public values under `include/lapis/session/terminal.hpp`,
Ghostty integration under `src/terminal/`, and behavioral cases under
`tests/terminal/`. It is one C++20 library, with no PTY, threads or GUI. The
[adapter receipt](../evidence/terminal-adapter.json) records the exercised scope.

- One owner thread operates each terminal. Clients receive owned snapshots that
  survive later input, resize and terminal destruction. Ghostty types remain
  private. Snapshots use a contiguous grapheme pool and row-major cells, avoiding
  per-cell heap allocation; the adapter reuses extraction scratch storage.
- Preserve graphemes, wide tails and wrap spacers, all exposed style flags and
  underline styles, default/indexed/RGB color identity, the current palette,
  cursor visibility/shape and the initial client's input modes. Effective colors
  apply inverse once; bold alone does not brighten indexed colors.
- Input and snapshot payloads have configurable byte/cell/codepoint limits.
  Invalid geometry and oversized input fail before engine mutation; extraction
  overflow leaves parsing usable. Generated terminal replies use preallocated
  storage. A reply overflow permanently faults the instance so a service cannot
  send a partial response; recreate and resynchronize it.
- History uses Ghostty's page-granular byte budget. Exercised eviction and clearing
  preserve the current viewport; this budget is not a strict allocation or RSS
  ceiling. Snapshot payload limits also do not account for allocator overhead.
  The service adds per-session/shared-root disk quotas and a bounded I/O queue;
  these remain separate from allocator and process RSS accounting.
- The verified input subset is navigation key presses with modifiers and pure text
  paste encoding. Clipboard access, full text/key protocols, mouse, IME, selection,
  hyperlinks and image presentation are not exposed. Image storage and external
  image media are disabled. A terminal reply never writes directly to a PTY.

This is an in-process boundary, not an IPC schema. The normal CMake build always
includes the adapter and requires a successful pinned Ghostty build prefix;
missing dependencies cannot silently omit its tests. The existing archive runner
owns downloads and source verification, while `cmake/Ghostty.cmake` checks build
provenance and imports the library. Reuse the build across C++ check modes.
[Dependency notices](../third_party/ghostty/NOTICES.txt) collect the upstream texts;
remaining source-provenance/SBOM limits are explicit. No binary is packaged yet.

The coordinator owns shared headers, build files and these documents. For future
parallel work, commit the shared contract first, assign disjoint files and one
build owner, and use bounded worker runs. Integrate and test worker output before
calling it complete. Preserve partial work after timeouts and avoid repeated
optional checks once the affected behavior passes.

### Persistent terminal acceptance

The explicit launch slice is implemented. Current exercise status is in the
[README](../README.md#current-status), with a
[sanitized receipt](../evidence/cli-launch.json). The macOS acceptance below is
complete; the [assembled receipt](../evidence/milestone-one.json) records its scope.
Visual review of the earlier UI refinements remains pending.

#### First wiring slice: explicit CLI launch

Internal **launch contract v1**, in `services/session/src/launch_spec.hpp`, carries
an executable, literal argument vector, working directory and initial terminal
size. `validate_launch` resolves PATH/relative executable names, preserves
executable symlinks and argv[0] semantics, canonicalizes the working directory,
and rejects invalid geometry, missing executables/directories, embedded NULs,
more than 256 arguments or more than 64 KiB of supplied launch text. The default
shell still receives `-i`; other programs receive only their requested arguments.
Both entry points preserve argv before initializing Qt and let only lapis parse
it: Qt otherwise consumes child options such as `-platform` even after `--`.

`WorkspaceOptions` supplies the launch and endpoint to the desktop. Explicit
programs or cwd overrides require `--socket`; all child arguments follow `--`.
Fixture mode rejects launch/endpoint options and does not inspect the user's
shell. Shell smoke injection rejects explicit launch/cwd overrides.

Service IPC is **version 6**. The launch fingerprint remains SHA-256 of a
Qt_6_0 big-endian stream of executable path, arguments and canonical cwd; terminal
size is excluded. Managed Codex mode prefixes this byte stream with
`lapis-codex-v1` plus a NUL byte, keeping it distinct from plain terminal launch.
It checks launch matching, while the session ID, service epoch
and attachment generation establish continuity. Neither is a secret token;
owner-only socket directory permissions provide the local access boundary.
Mismatched candidates never replace the active client. At most eight initial
handshakes wait three seconds, and each accepted client must acknowledge its
first full snapshot within three seconds of attachment, including a blocked hello
(15 seconds for dedicated Codex backend startup). The desktop bounds synchronization to
five seconds. Only an explicit new-session action starts a service.

Socket parents must be private and owned by the current user. Ordinary files,
symlinks and live foreign listeners are rejected; existing directories are not
chmodded. The default `runtime/desktop-v6.sock` leaves old v1/v2/v3/v4/v5 sessions alone.
No state migration, multi-session manager or automatic service recovery is implied.
Launch profiles, hooks and approval policies remain owned by the selected CLI.

Ownership stays separated: the PTY owns the child; the service owns parsing and
attachment; the desktop routes input after applying and acknowledging the initial screen. `scripts/check_cli_launch.py`
exercises literal argv/cwd, resize, paste, exit, detached output, mismatches and
malformed handshakes against real service processes. Its optional GUI and Codex
modes add GPU captures and no-prompt TUI interaction; that fixture does not exercise
Codex attention or model-turn continuation. Those are qualified separately by the
[Milestone 2 checks](#milestone-2-attention-and-codex-plan). Commands live in
[Contributing](../CONTRIBUTING.md#cli-integration-qualification).

The PR #2 repair adds explicit render-state synchronization, retained row nodes,
control/Meta text-key handling, bounded final PTY output drain, and signal-exit
reporting. A detached process-group guard retains group membership until the
service closes a private pipe on leader exit or teardown, then kills its own group.
This also removes quiet same-group descendants that ignore hangup, without
signaling a saved PID after Qt reaps the leader. The guard closes inherited file
descriptors and is detached before CLI exec, so the CLI does not inherit a hidden
child. Processes that deliberately regroup or detach need separate platform
qualification. A final output tail is bounded to 16 MiB after child exit.
Snapshot-size limits detach the display with an explicit status while keeping the
child alive; reattachment succeeds once its screen fits again. Socket ancestors
must be trusted and not shared writable unless sticky, with an owner-only 0700
immediate parent. Oversized paste remains an atomic rejection at 64 KiB.
Cursor rendering honors block, bar, underline and hollow-block shapes and optional
cursor color; a filled block redraws its covered grapheme for readability.
See the [review repair receipt](../evidence/pr2-review.json) and
[merge preparation receipt](../evidence/pr2-merge.json) for exercised cases.

### Milestone 1 implementation and acceptance

Planning baseline: merged `99567cb` (September 18, 2026), followed by the verified
session checkpoint `31cabfe`. Slice A is implemented with qualification recorded
in [its receipt](../evidence/session-reconnect.json). B1 now has a verified macOS
cell-grid checkpoint for fallback fonts, wide characters, decorations and resize.
Disk history, Qt input-context lifecycle and automated native input acceptance
are implemented and exercised. Milestone 1 software acceptance is complete on macOS,
with the scope and measurement limits in the [receipt](../evidence/milestone-one.json).

| Order | Reviewable slice | Acceptance result |
| --- | --- | --- |
| A: implemented | Session identity and safe attachment | GUI reconnect preserves session identity and child; a replaced attachment cannot send input; input stays disabled until the initial authoritative screen is applied; service replacement is reported distinctly |
| B1: exercised checkpoint | Cell layout and font fidelity | Native Vulkan pixel regressions align wide/combining and fallback glyphs, backgrounds, decorations and cursor through resize; shell/Codex captures pass; cross-cell contextual shaping remains open |
| B2: exercised | Native input and first responsiveness measurements | OS-generated keyboard events, paste and the real Apple Japanese IME work without losing input ownership; selection/clipboard/accessibility gaps are explicitly exercised or remain open; correlated input/service/frame measurements report p50/p95/p99 and their endpoint limits |
| C: exercised | Bounded older history | Recent screen stays warm while older history is stored under per-session/global quotas; scrollback retrieval, eviction, disk-full and interrupted-write recovery remain bounded and preserve the live screen |
| D: deferred Linux port | Integrate A through C and native input fixes when scheduling the port | Named Linux host, display stack and driver exercise PTY lifecycle, real Vulkan rendering, input, resize and detach/reattach; a headless or software-only result does not qualify the GPU desktop |

The renderer places runs at engine cell coordinates. Printable ASCII batches
only when styled advances match the grid, with kerning and optional ligatures
disabled. Other graphemes shape locally at a common baseline. Backgrounds precede
glyphs, decorations follow, and unchanged rows retain their nodes. Native Vulkan
regressions cover wide/combining characters, emoji, Hebrew/Arabic fallback,
styles, resize and cursor movement. Cross-cell contextual shaping and curly
underlines remain gaps; see the [fidelity receipt](../evidence/terminal-fidelity.json).

Native input acceptance uses OS-generated keys through AppKit, an actual macOS
input method editor (IME), Qt and a controlled PTY. The built-in Japanese IME is
one reproducible composition fixture: a Roman key produces provisional text
(such as `a` → `あ`) that can be committed or cancelled. This exercises behavior
that ordinary direct English typing does not. It does not set the application's
language or qualify every IME. Control/Option keys and paste are tested with the
US layout. No physical typing gate is required. The
[contribution guide](../CONTRIBUTING.md#history-and-input-qualification) owns the
commands, prerequisites and restoration procedure.

The assembled macOS qualification covers service identity, native Vulkan
rendering, Qt and native input, history quotas/backpressure/corruption/real ENOSPC,
preview captures and live CLI fixtures. ASan/UBSan and TSan run separately;
GUI runs stay serial even when independent builds run concurrently. Timing
receipts distinguish frame submission from pixel visibility and keep latency
targets provisional. The Linux desktop port has no current host requirement or
acceptance date; qualify an actual graphical host when that port is scheduled.
Real Codex attention integration is qualified by Milestone 2. Multiple live
sessions, automatic carousel behavior and the 32-session benchmark remain in
the following milestones.

#### Milestone 1 completion contract

The completed macOS slice is consolidated on a feature branch for PR review.
Wire v4 adds attachment-bound, request-correlated history paging independently of
live snapshot sequence. Snapshot envelopes carry monotonic nanosecond timestamps
for the most recent PTY read, parse completion and publication; zero means no PTY
output has been observed. Desktop measurement reports native/Qt input receipt,
snapshot application and the correlated frameSwapped proxy separately. Older v3 endpoints remain running and are rejected by new
clients rather than adopted silently. Historical pages are read-only and retain
their original cell geometry; returning to Live restores the latest warm screen.
History browsing must not resize the child or receive terminal input.

The service extracts primary-screen scrollback into owned pages before clearing
that engine history. A dedicated I/O worker stores pages atomically under private
runtime storage with per-session and shared-root global quotas. Queues and record
sizes are bounded; archive errors are surfaced while the live process and screen
remain usable. Disk format/version and checksums are independent of the wire.
Resize reflows current engine history; archived pages preserve their recorded
geometry. Live-process recovery after service failure/reboot remains outside this
milestone. Automated native-input evidence and frame-submission proxies are labeled
separately from Qt-injected tests and actual on-screen presentation.

#### Session identity and input readiness

Wire v4 retains the identity and readiness contract introduced in v3; there is
no session registry or additional live card. The v2 launch fingerprint alone
could not distinguish the original child from a replacement at the same endpoint.

Implemented contract:

- A session ID remains stable for the running session across GUI
  detach/reattach. A fresh service incarnation has a fresh epoch; the PID is
  diagnostic data, never the authority for identity. Reopening an ended session
  must not silently present a newly launched child as the old session.
- Every successful attachment receives a new generation. Input, paste and resize
  identify the session, service epoch and attachment generation; the service
  rejects stale tuples. A new attachment retires the previous client's authority,
  except a join (added within v6, September 24), which adds a view beside it.
- The client distinguishes connecting, synchronizing, ready, disconnected and
  ended/replaced states. It enables terminal input only after applying a full
  snapshot for the accepted identity. An explicit reconnect action makes bounded
  attempts to that same identity; it must not respawn an agent or replay buffered
  input implicitly. Automatic reconnect policy follows separately.
- Distinguish first launch from reconnect. Retain the last accepted session/epoch
  and launch fingerprint in a bounded owner-only local descriptor so a GUI restart
  can detect endpoint reuse. Treat that descriptor as a hint to verify against
  the live service, not proof of liveness or authorization. Missing/corrupt state
  requires explicit discovery/new-session handling, not presumed continuity.
- GUI loss preserves the service-owned child and parser. Service loss/reboot
  invalidates the attachment and reports loss of the running session. Durable
  launch profiles and recovery of old processes are separate later contracts.
- Keep bounded full snapshots for this slice. Order them within a service epoch;
  intentional display coalescing may skip terminal revisions. Do not confuse
  those skipped display revisions with loss of input or control events. Reconnect
  starts with an authoritative screen, not replay of an unbounded byte backlog.
- Version incompatible envelopes. Wire v4 added history paging and
  timing to the v3 identity contract. Leave older endpoints untouched and use
  `runtime/desktop-v6.sock` by default. Shared serialization and client/server
  behavior must change together; test rejection of mismatched versions.

The wire uses big-endian integers and nonzero raw 16-byte UUIDs. Attach contains
u32 version, fingerprint[32], u8 mode (discover/reconnect/create), expected
session[16] and epoch[16]. Missing expected fields are zeros on the wire. Discover
requires both missing; reconnect requires both; create requires only session.
An attachment tuple is session[16], epoch[16], u64 nonzero generation. Hello is
u32 version, tuple, u64 child PID. Snapshot is tuple, u64 publication sequence,
then three u64 monotonic timestamps (PTY read, parse end, publish), followed by
the terminal snapshot encoding. Its fixed envelope is 72 bytes. Text, paste, key and resize carry
the tuple before their existing payload (maximum 64 KiB). Ready acknowledges the
tuple and first applied sequence. Typed status distinguishes rejection, ended,
replaced and overload. Full frames remain bounded to 8 MiB. Publication sequences
can skip coalesced terminal revisions; the client rejects duplicate/regressing
sequences and mismatched tuples.

`--new-session` generates a creation ID and passes it to the detached service;
that service creates a fresh epoch. A racing creator cannot adopt or displace a
service with another ID. Default launch reads `<socket>.session`: a 73-byte hint
(`LAPIS-S1\n`, session[16], epoch[16], fingerprint[32]), owned by the current user,
regular, singly linked and mode 0600. Writes sync a temporary file and atomically
rename it. The desktop performs the synced write on a bounded Qt worker pool;
its completion is checked against the current attachment before enabling input.
Retired sockets can drain a final status for up to one second, with at most eight
retired sockets retained. A non-reading peer may see EOF before the final status.
This is an identity hint, not durable process recovery. Explicit
discovery may replace corrupt contents only in a safe file. A GUI reconnect
makes at most 30 connection attempts, 100 ms apart, with no automatic retry after
an established connection is lost. Unsynchronized input is rejected, not buffered.

The service harness exercises successive attachments, stale-generation text and
resize, fragmented handshakes, pre-ready input, missing acknowledgements,
non-reading clients, bounded PTY input overflow, atomic oversized paste rejection
and replacement at the same endpoint. A separate real Qt socket fixture exercises
desktop synchronization, loss before the first screen, explicit discovery,
descriptor reuse, stale snapshots and old-server rejection. GUI capture fixtures
close and reopen against the same child. Neither these nor fake-server tests
qualify physical keyboard or IME behavior.

Acceptance commands are `python3 scripts/check_cpp.py dev`,
`python3 scripts/check_cpp.py desktop` and
`python3 scripts/check_cli_launch.py --desktop`, plus the documented
[desktop-enabled ASan/TSan suites and service harness](../CONTRIBUTING.md#desktop-sanitizers).
Repeat the installed no-prompt Codex fixture when checking the updated launch and
attachment path. Retain the source hash, identities observed and failure outcomes
in sanitized evidence; update README status only after the behavior passes.

#### Shared implementation ownership

Contributors work together on the same feature and share ownership of the project.
Coordinate overlapping edits and build runs per task; temporary worker file
assignments prevent collisions, not permanent responsibility boundaries. Agree on
shared contracts before dependent edits, preserve each other's changes, and review
the combined behavior together. Use separate worktrees when useful and one owner
for each active build directory. GUI checks remain serial across worktrees.

#### Following product checkpoints

The macOS A through C qualification led to the now-qualified attention state
machine and managed Codex route. The
[Milestone 2 plan](#milestone-2-attention-and-codex-plan) records its acceptance
and evidence. Preserve the ordinary CLI view and qualify any additional route
with live observation, explicit response and reconnect evidence. A notification-only
hook must leave the answer in the originating terminal; a separately owned
app-server remains a distinct session type. Use disposable fixtures, a declared
provider/model, bounded turns and explicit approval settings for new qualification.

Next replace fixture cards with **two actual retained sessions**, deliver manual
navigation and keyboard ownership guards, and add pin/snooze and the opt-in
attention carousel. Only after that behavior works should the 32-session workload
and second independent adapter qualify scale and tool independence. The
[following milestones](#following-milestones) retain their acceptance gates;
packaging/notices/SBOM work can proceed independently and must finish before
binary distribution.

The history store defaults to 64 MiB of committed pages per session, 256 MiB
across its configured root, and 4,096 pages globally. Limits count page files;
there is at most one 8 MiB temporary write plus one replacement page while the
root lock is held. The scan recovers the store's known `.pending` artifact before
another write. Unknown files are not deleted or charged as lapis pages; directory
and scan-count bounds stop accumulation from turning into unbounded work. Session
counters and directories are metadata, capped by the 1,024-directory scan limit.
Eviction preserves monotonic page IDs; the oldest committed pages go first.
The service queue is capped at 128 operations / 16 MiB of page data (plus one
active operation). It pauses PTY reads while a harvest waits for queue capacity;
resize waits until that harvest releases the engine viewport. Filesystem work
runs on one dedicated worker thread. Root-lock contention waits up to 500 ms on
that worker; concurrent-writer tests enforce the shared quota. A failed archive write leaves older committed
pages intact, records a visible gap message when browsing, and keeps live I/O
usable. Browsing retries storage after repair. Normal child exit and direct service
error shutdown allow up to three seconds for queued pages to drain; forced service termination may lose the queued tail. Archive storage
does not restore a live process after service death or reboot.

Qt input tests exercise committed Unicode, cancellation (including empty native
preedit cancellation), unsupported replacement rejection, atomic clipboard paste,
focus/document/history/disconnect transitions, and recovery with a fresh
composition. The native context resets when the window becomes inactive.
Replacement of already-sent text is intentionally unsupported: lapis cannot erase
bytes already consumed by a CLI. Selection/copy from terminal cells and a terminal
accessibility tree are open gaps. The separate native-input probe qualifies actual
Apple Japanese IME commit/cancel, focus ownership, resize and recovery through
AppKit/Qt and the PTY. It checks the native candidate anchor rectangle, not candidate
window pixels. It restores the clipboard and selected/enabled input sources.
Physical keyboard hardware is outside this milestone's software acceptance.
The latency probe correlates service sequence/revision to `afterSynchronizing`
and `frameSwapped`; the latter is a submission proxy, not measured pixel visibility.
Cross-session switching remains a later milestone because this slice owns one
live terminal; history-to-live restoration is a retained-screen operation.

### Engine experiment decision

Use **Ghostty VT with a C++20 service**; the first adapter now implements this decision. At pinned
revision `5de703a1b6ca0b91fcebe932b44be1df2de0a683`, the unpatched headless library
builds with Zig 0.16.0 and its public C API supports a C++20 consumer. All eight
shared cases pass on this macOS ARM64 host and native Ubuntu 24.04 ARM64. No Qt,
GPU or application UI is needed by that consumer.

Contour revision `6777ff05014f8ff163b071e8b0e942830119db80` also builds headlessly,
with C++23 isolated to its experiment. Seven cases pass; bytewise input of
`A界éZ` produces an extra U+FFFD before the wide character on both platforms.
The result reproduces through direct ingestion and upstream mock-PTY processing.
Keep this failing comparison visible; it is not a complete assessment of Contour
or a reason to alter the shared expectation.

The exercised boundary copies viewport graphemes, widths/continuations, bold and
resolved RGB colors, cursor position and tested input modes into owned values.
The color comparison applies inverse to resolved foreground/background colors,
keeps bold independent of palette brightening, and pins palette/default colors
in replay. Indexed, explicit bright, inverse, bold-inverse and reset cases now
exercise that policy on both platforms; the earlier truecolor-only case did not.
Snapshots survive parser mutation, resize and engine destruction. Key-up and paste
encoding follow terminal modes. This was evidence for the snapshot-fed surface now present in the desktop
checkpoint. The production adapter has since added styles, tagged colors, wrap
spacers and cursor data. The experimental header is
not a serialized service contract. Dirty-row APIs exist in Ghostty but incremental
damage extraction has not been qualified; begin with bounded full snapshots.

The September 22 composer-background repair also reads Ghostty's raw-cell
background-only content tags. Erased cells can keep their RGB or palette-index
background there rather than in the style; ignoring these tags removed the empty
parts of TUI input panels. The regression covers colored erase-line and
erase-character operations, subsequent palette changes, and resetting to the
default background. The fix is in the session-service snapshot adapter, so an
already running service does not acquire it when only its GUI reconnects.
Measure allocation churn when building the production extraction path; the
correctness probe uses per-cell scratch buffers and is not a performance baseline.

Ghostty's configured 1 MiB history setting is read back through the API; the burst
case checks viewport size, not a process memory ceiling or disk-backed history.
A September 18 exploratory C API probe on the selected Ghostty static library
from build run `aea255c0f528427e8e263ace819e3ea3` (SHA-256
`ee4e3e23bbd9e9213db66afd80c764ca65f7f505fd9a175fa661cfb602934477`)
encoded and decoded a 2,186-byte snapshot with unfinished CSI input, restoring
an 81-row scrollable area containing 79 history rows; absolute viewport requests
clamped beyond the end back to offset 79. It did not exercise multipage history,
text/style equality, disk persistence or failure recovery. The service diagnostic
`.log` is not PTY replay.
The consumer passes ASan/UBSan; upstream Zig uses ReleaseSafe, which is a distinct
kind of checking. No latency, shaping, IME, PTY or GUI-persistence claim follows.

Keep the unstable API pinned behind the adapter. Before redistribution, package
all dependency notices: uucode's archive omits referenced notices, and the bundled
simdutf header identifies 9.0.0 while wrapper metadata says 5.2.8. The receipt has
the observed dependency/license inventory and retrieved notice references. The
shared dependency scanner cannot recognize this C++/Zig graph; direct OSV commit
queries returned no advisories for the queried pins, which is not full coverage
or vulnerability clearance. Runtime dependency packaging remains unfinished.

### Deliver milestone 1: one persistent terminal

Build the selected engine into a service that launches one shell under a PTY.
Attach a minimal desktop terminal view. Include Unicode/font handling, input,
resize and history sufficient for the exercised shell and interactive TUI.
Acceptance requires:

- Launch, input/output, deliberate resize, exit and resource cleanup work.
- Closing/reopening the GUI preserves the child PID and restores current terminal
  state. Output continues draining during detachment, including sustained output;
  retaining an open descriptor alone is insufficient.
- Current screen/recent history and older disk-backed history have explicit
  per-session limits. Reattachment does not replay unbounded output.
- Safely transferred snapshots isolate the renderer from parsing. A stalled GUI
  does not block process control; snapshot/delta gaps cause resynchronization.
- Service failure is distinguished from GUI detachment. Live-process survival
  across service failure or reboot is a separate recovery problem.
- Relevant cases pass `just check`, `just asan` and separate `just tsan` runs;
  the minimal GUI and actual GPU backend have direct macOS evidence.
- Record input, frame and switch timing from the first interactive view on the
  reference machine. Report tails, missed deadlines and warm-cache memory use;
  do not defer responsiveness instrumentation until the 32-session milestone.

Keep process I/O and parsing off the GUI thread. Bound queues and processing
work, coalesce display updates and preserve input/lifecycle events. Pin each input
operation to one session. GUI detach and session termination are separate commands.

### Milestone 2: attention and Codex plan

**Completed on macOS:** following PR #6, 2C, 2D and assembled 2E qualification
are recorded in `evidence/milestone-two.json`. Keep one
dedicated Codex app-server per live terminal, with the ordinary TUI and a
service-owned observer connected to that server. Reuse the
[shared contribution standards](../CONTRIBUTING.md#code-standards).

Planning baseline: merged `e53c4fe` (September 18, 2026). The first implementation
checkpoint adds a standalone attention reducer and isolated live Codex probes.
The original desktop attention map and replay controls remain isolated development
fixtures. Checkpoints 2C and 2D now connect the reducer to a production Codex
adapter, service IPC and the live pane's explicit response dialog.
The September 19 reconciliation preserves main's one-command launcher, configurable
navigation, four layouts and appearance settings. These operate over the existing
single live service session and fixture cards; they do not complete Milestone 3.
Session navigation is a flat list. The historical category-named shortcut actions
are compatibility aliases for that list; category definitions in old configuration
files are not consumed. Grouping sessions remains part of the supervising desktop
milestone, rather than a settings-only feature.
The retained UI is exercised through all appearance controls, shortcut reload,
modal focus and four rendered layouts. Blocks wraps into responsive rows and
Stack fills the viewport while keeping manual selection visible. Appearance
writes are atomic and preserve unrelated JSON values. The attention reducer
remains independent of those UI changes.
The original `probe_codex.py` still establishes schema, initialization and listing
only; a separate opt-in runner exercises actual model turns.

The core owns one session/adapter source, preserves numeric versus string IDs,
tracks response-in-flight independently of resolution, and requires authoritative
reconciliation before enabling actions. Local sequence gaps establish a recovery
watermark: snapshots older than any observed event cannot re-enable responses.
Retired IDs remain bounded within an epoch; exhaustion requires a new source epoch
rather than silently forgetting duplicate history. Queue ordering, aging, snooze
and acknowledgement cooldown are exercised within that source. Cross-session
aggregation remains later work. The library has single-thread ownership, no I/O,
and no focus or GUI-attachment policy; those checks belong in service integration.
An unchanged request in a same-epoch snapshot preserves its decision token only
while the source remains synchronized. Recovery, changed payloads and new epochs
rotate tokens; a submitted response remains non-retriable within its source epoch.
Snooze and acknowledgement accept only synchronized pending requests. Empty
choice lists represent observation-only notices that the originating adapter
must resolve from source state.

Real GLM turns against a dedicated Codex server deliver user-input and command
approval requests. An observer receives pending requests on resume, receives the
same typed IDs after reconnect, responds, and observes matching resolution and
successful turn completion. An ordinary Codex TUI attached to that server displays
both request kinds. This qualifies the exercised shared-server path, not a lapis
service-to-desktop adapter. The Unix endpoint uses WebSocket framing; the installed
`app-server proxy` forwards raw bytes rather than converting newline JSON.

The selected route is a dedicated shared server with the ordinary TUI and a
service-owned observer. Reconciliation for the two exercised blocking request
kinds uses `thread/resume` followed by `thread/read` for the same thread. The
inspected implementation serializes those methods on an exclusive thread key;
resume waits until pending-request replay is queued before releasing it. The read
reply therefore marks a replay boundary. The live probe checks pending requests
at that boundary and requests answered while another observer is disconnected,
including the subsequent idle state and completed turn. Resolution wins over a
late replay of the same typed ID.

This is an implementation-specific contract, not a schema guarantee or a quiet
interval. Requalify changed Codex binaries before enabling responses. Other request
kinds remain unqualified. Cancellation and simultaneous requests were not covered
by this standalone checkpoint; the assembled acceptance below exercises them.
Unknown source versions and failed reconciliation keep responses disabled. Native
hooks remain an unqualified alternative; they are not needed for the selected
shared-server route. Production service/IPC and desktop wiring are described below in 2C/2D.

The 2C implementation uses opt-in managed Codex launch, distinct from plain
terminal launch in the launch fingerprint. The service owns a dedicated Unix
app-server endpoint and an ordinary TUI attached to it, plus a separate observer.
Startup asynchronously probes for a listening backend before starting either
client; socket-path existence alone is insufficient. This startup-only retry is
bounded to ten seconds. GUI attachment cannot reconnect an observer that has not
yet been started. Before launching the backend, the service hashes the entire
executable against the qualified binary identity. File metadata or a prefix digest
is not sufficient to enable responses. This synchronous check runs in the separate
service process; it adds startup latency, not GUI-thread work. Backend exit
diagnostics include normal exit codes or crash status, without publishing raw
backend stderr.
Both child groups use the existing POSIX ownership guard, including cleanup on
abrupt service loss. Production inherits the caller's Codex home and policy;
private homes and explicit model/approval settings belong only to qualification
fixtures. The observer binds one persistent TUI thread. Live source metadata also identifies
ephemeral backend threads; discovery classifies these explicitly and does not route
their requests through the TUI controls. Events from unknown senders after binding
stay non-actionable, including during reconnect replay. Classification releases
their bounded traffic accounting; overflow fails closed. A second persistent
thread disables structured responses pending a future thread-switch contract.

Wire v6 retains terminal and history envelopes and adds attachment-bound attention
snapshots and decisions. The adapter caps retained pending details at 512 KiB,
with bounded identity fields and at most 128 requests. Aggregate overflow disables
the source while retaining the previous bounded request evidence. The IPC encoder
also rejects oversized snapshots incrementally at 1 MiB.
Snapshots include typed source IDs, source epoch, revision,
readiness and pending/response-in-flight/stale state. Decisions require the active
GUI attachment and exact source token. Sending consumes the token but does not
resolve the request; only source resolution does. A transport failure after token
consumption leaves delivery uncertain and disables responses until explicit
reconciliation; it never triggers an automatic resend. The private WebSocket
transport bounds queued writes at 2 MiB. An exhausted write queue, including a
pong or close echo that cannot be queued, fails closed rather than adding a
second retry queue. A rejected send must be handled by its caller. Source reconnect must reconcile
before enabling decisions, and unqualified Codex binary hashes keep structured
responses disabled. Wire v6 adds `attention_retry` (kind 14): an attachment-bound echo of the decision
identity/choice with empty answers. The service sends it only when rejection
occurred before submission and the exact request is still pending. This releases
the UI's provisional duplicate guard for a corrected explicit response; ordinary
queued snapshots cannot do so. Older v4/v5 services are neither migrated nor stopped.
The service qualification in `evidence/codex-service.json` exercises approval and
user-input responses, same-child reattachment, stale/duplicate decisions and
cleanup after abrupt service death. Desktop controls are not included in that receipt.

The desktop exposes request count/reason and stale diagnostics without automatic
focus changes. A changed source epoch or request revision produces a fresh
attention cue even when the source reuses an ID; duplicate snapshots and a changed
GUI attachment alone do not. Its modal response dialog binds a frozen draft to an opaque native
attachment/epoch/revision token; request IDs and revisions never round-trip through
JavaScript numbers. Incoming updates invalidate actions without replacing typed
answers or composition. Modal ownership blocks workspace shortcuts and terminal
focus restoration. The production QML controls are exercised against a disposable
real Codex session by `check_service_attention.py --live-glm --desktop`.
Thread close/archive events disable the source even while the backend remains
alive. Restoring that thread and explicitly reattaching reconciles under a fresh
epoch; no response is retransmitted.

**Outcome:** a real Codex session can request attention, receive an explicit
decision through a verified route, and continue. lapis retains the exact request
identity, distinguishes disconnection from completion, and prevents stale replies.
Start with one live terminal and a minimal attention view. Workspace-wide
attention aggregation and the guarded automatic carousel remain Milestone 3.

#### Ordered implementation slices

| Slice | Work and dependency | Acceptance before moving on |
| --- | --- | --- |
| 2A: route qualification | Extend the disposable Codex investigation; in parallel, draft the normalized contract below. Probe an ordinary TUI and observer connected to the same dedicated server first; evaluate isolated native hooks independently. | A hashed binary and declared provider/model produce actual approval and user-input events. Record which client receives each event, which can answer, how resolution is observed, and what survives reconnect. Select the route from those results. |
| 2B: attention core | Add a small C++20 library under `services/session/`, independent of Qt rendering and Codex payload types. Implement the versioned event/request contract, reducer and deterministic queue policy. It can begin alongside 2A using explicitly synthetic fixtures. | Replay proves typed identity, independent activity/connection/request state, deduplication, cancellation, aging, cooldown/snooze and recovery. An injected clock makes ordering reproducible; limits and overflow have tested outcomes. |
| 2C: Codex adapter and service | After the route gate and common contract settle, implement mapping, explicit replies and reconciliation under `adapters/codex/`; connect it to the persistent service. Add versioned IPC for authoritative attention snapshots and targeted decisions. | A real request reaches the service, a validated decision reaches its source once, and resolution/continuation are observed. GUI detach preserves source ownership; source loss disables replies. Compatibility tests reject mismatched clients safely. |
| 2D: minimal desktop integration | Bind the existing live pane to service attention state; display pending reason/count, stale state and supported response controls. Keep preview replay isolated. Depends on 2C. | A live request appears without moving focus; an explicit decision resolves only its request. Reattachment restores pending state before enabling actions. Unsupported response capabilities direct the user to the originating terminal. |
| 2E: assembled qualification | Exercise the combined head with replay, live Codex, service recovery and macOS input regression. Update status and operational procedures from the result. | Every exit condition below passes with sanitized receipts, or the remaining capability is explicitly incomplete. Separate adapter/replay success from end-to-end success. |

The first implementation checkpoint is **2A plus the 2B contract/replay scaffold**.
Do not make UI or transport implementation depend on an unverified Codex route.
Use reviewable checkpoints on feature branches; the slices are work packages for
shared contributors, not permanent ownership assignments.

#### Codex route decision

Preserving the ordinary CLI interface is the product preference. A shared-server
route is eligible only if live evidence establishes event delivery, response
ownership and reconciliation for that same TUI session. A method in an exported
schema or an empty list from another server cannot establish those capabilities.
The [Codex investigation](../adapters/codex/README.md#next-qualification) owns the
probe details and capability matrix.

Hooks that only notify are useful partial integration. Keep answers in the
originating terminal and report response/reconciliation capabilities as unavailable
until exercised. Do not call notification-only support full Milestone 2 acceptance.
If neither route qualifies, preserve the completed core work and record the
specific gap before choosing a different product route. A standalone lapis-owned
app-server is a distinct session mode, not a transparent substitute for the TUI.

Use bounded, disposable turns and explicit fixture approval settings. Record the
effective inference provider/model from runtime evidence, binary hash, backend
ownership, prompts/fixtures safe to reproduce, deadlines and cleanup results.
Keep private transcripts and credentials out of receipts. Do not modify global
hooks, ordinary launch policy or the user's running daemon. Provider failure or an
event that cannot be elicited is an unqualified case, not a simulated live pass.

#### Attention contract v1 and integration boundary

The logical contract below guides the implemented core and remaining integration.
`attention.hpp` supplies the single-source C++ types; `Position` keeps the source
epoch and local sequence together. Decisions use per-request revision tokens so
unrelated activity cannot invalidate an otherwise current response.
It is separate from terminal IPC v6. Any incompatible IPC extension gets a new
wire version and private endpoint, with explicit rejection of older clients;
existing service processes and sockets are left intact.

| Contract element | Required semantics |
| --- | --- |
| Identity | Stable lapis session and adapter IDs; source connection epoch; original typed request ID and available thread/turn/item IDs. Numeric `1` and string `"1"` are distinct. Preserve IDs losslessly or reject unsupported representations. |
| Event envelope | Contract version, kind, local sequence and monotonic receipt time; retain source sequence only when supplied. Local ordering cannot prove no upstream event loss. Declare gap-detection/reconciliation limits per adapter. |
| Session state | Connection health, activity, pending requests and reconciliation readiness are independent. Silence is unknown; a completed turn is not task completion or process exit. |
| Request state | Preserve pending, response-in-flight, uncertain/stale and terminal outcomes. Conflicting payloads for the same identity require reconciliation, not silent replacement. Matching source resolution or authoritative reconciliation retires a request; sending, viewing, acknowledging or focusing does not. |
| Decisions | Bind an explicit choice to session, source epoch, request identity and current service revision/GUI attachment. Validate supported choices and payloads in the service; reject duplicates and stale or mismatched decisions. |
| Recovery | GUI detach refreshes from the healthy service; source disconnect invalidates response eligibility. Never replay an ambiguously delivered response automatically. Reconcile first, or expose the unresolved state and require action in the source terminal. |
| Capabilities | Advertise observation, response and reconciliation separately, including supported request kinds and evidence level. Bells and prompt heuristics remain advisory. |
| Bounds and queue | Set explicit limits for frames, request count/payloads, event queues and retired-ID bookkeeping. Preserve identities; do not silently truncate or drop control events. Overflow marks loss of synchronization and disables replies until recovery. Order eligible attention deterministically with aging and cooldowns; snooze/acknowledgement never resolve it. |

Service policy produces an attention ordering, never a keyboard-focus command.
Use multiple synthetic session IDs to prove ordering and non-starvation now;
workspace-wide aggregation across live service processes belongs to Milestone 3.

#### Verification and exit conditions

Register the new reducer/replay cases in normal CTest so they run through existing
`just check`, `just asan` and `just tsan` workflows as applicable. Cover duplicate
requests/resolutions, typed-ID collisions, unknown or late resolutions, ID reuse
across epochs, cancellation, simultaneous requests, deterministic aging/snooze,
malformed or oversized payloads, queue overflow and unsupported methods.
Exercise resolution racing with a decision, duplicate GUI submissions, disconnect
during reply, failed reconciliation, and a stale GUI attaching to a replaced source.

The separate opt-in live qualification runner is `scripts/check_codex_attention.py`;
its commands and isolation procedure are documented in CONTRIBUTING.md.
Keep `scripts/probe_codex.py`'s default no-turn inspection behavior. The live runner
must have deadlines, bounded attempts, isolated resources and a nonzero exit for
failed acceptance. A fixture transport proves failure handling; it cannot replace
real model-turn evidence. Required live cases are an approval and a user-input
request, explicit responses, matching resolution and continuation, plus source
reconnect reconciliation. Add denial/cancellation and simultaneous requests to
the adapter replay corpus; exercise them live where the selected route permits.

Integration uses `just desktop`, `just cli-check`, `just ui-check`,
`just native-input`, and desktop-enabled ASan/TSan according to the existing
[required-check matrix](../CONTRIBUTING.md#checks). New Python tooling also needs
the documented lint/format checks and success/failure cases. Run GUI checks
serially; no physical typing gate is introduced. Re-run unrelated suites only when
the change or a failure warrants it. Audit any new dependency before adoption.

Completion requires the qualified route and the service-to-desktop request/decision
flow on the same assembled source revision, not just a passing standalone probe.
Keep sanitized receipts under `evidence/` with commands, source/binary hashes,
capability results and limitations; raw logs stay under ignored `build/`.
The first checkpoint receipt is `evidence/codex-attention-route.json`.
`evidence/milestone-two.json` records assembled acceptance: real desktop approval
and answer controls, exact rejection/retry, pending reattachment, archive/restore
reconciliation, a new request cancelled after recovery, and two simultaneous real
approvals resolved independently. Normal, ASan/UBSan and TSan suites, CLI checks,
preview captures and native macOS input pass. This qualifies the one-session
Milestone 2 scope on its recorded binary. The Milestone 3 section below records
the completed multi-session qualification and consolidated evidence.
Update README's status table only as those capabilities land. Linux desktop,
32-session load, second adapters, selection/accessibility/contextual shaping and
fresh presentation-latency targets are outside this phase. Packaging/notices/SBOM
remain an independent prerequisite for binary distribution.

The [2026-09-22 Codex qualification](../evidence/codex-binary-update.json)
updates the single production binary pin to `81f1d50b…`. The dedicated backend,
ordinary TUI and observer boundaries are unchanged. The newer binary retains the
resume/read reconciliation contract; live service and two-session desktop checks
exercise its response path. Unknown hashes remain disabled and keep their explicit
failure reason across reconnect attempts. Qualification trusts each disposable
fixture in its private Codex home; it does not change the user's trust settings.

### Milestone 3: supervising two live sessions on macOS

**Superseded in the desktop by the category workspace below.** Its flat
manifest (`--workspace`), aggregate supervisor queue and carousel UI were removed
when the agent strip landed; the service, transport, Codex pin and Claude Code
adapter it qualified are retained. This section remains the record of that
qualification.

**All three checkpoints qualified together on macOS; evidence is recorded in
the [workspace receipt](../evidence/milestone-three-workspace.json).**
Its consolidation record refreshes the retained-workspace, two-source Codex and
native-input checks after the Claude hook extension; the original checkpoint
receipts remain dated evidence with their tested source hashes.
The outcome is one desktop workspace that retains and supervises two real
sessions: both continue running and consuming output, manual navigation sends
input only to the selected session, and attention from either session can be
reviewed and answered explicitly. The guarded, opt-in carousel uses the same keyboard ownership policy.

The branch retains the Milestone 2 corrections and UI repairs merged in PRs
#7, #8 and #9. Use the three sequential checkpoints below; passing the first
checkpoint does not mean the entire milestone is complete.

#### Scope and design decisions

- Qualify **two live sessions in one macOS window**. Use two controlled shells
  for deterministic input/lifecycle tests and two managed Codex sessions for
  real cross-session attention. Two sessions is the acceptance workload, not
  evidence of 32-session capacity.
- Keep one existing service process, PTY, terminal engine, history store and
  optional Codex observer per session. A workspace registry discovers and
  remembers these services; it does not take ownership of their child processes.
  No service multiplexing rewrite or always-running workspace daemon is needed.
  Preserve the explicit `--socket` launch/reconnect path and require explicit
  adoption of older single-session endpoints; never silently migrate their state.
- Add a bounded, versioned, owner-only workspace manifest under `runtime/`, with
  atomic writes and a single writer. Record stable lapis session IDs, endpoints,
  expected service identities, launch fingerprints, display metadata and card
  order. Do not store prompts, environment secrets or arbitrary command lines.
  Reuse the service session ID as the stable lapis ID when creating or adopting
  an entry; do not identify a session by its PID. Separate fresh-launch arguments
  from the metadata needed to reconnect. The manifest and existing `.session`
  hints are not proof that a service is alive.
  Persist a verified identity only after accepting its initial attachment/screen.
  Reject duplicate endpoints, conflicting identities and corrupt manifests without
  creating replacement processes or discarding the usable neighboring session.
- Reuse v6 per-session IPC unless a demonstrated missing field requires a
  separately reviewed version change. Keep lapis identity, service epoch,
  attachment generation and adapter source epoch distinct. Route through stable
  session identities, never a mutable card index or title.
- Keep a connection and retained screen/history model for each live session.
  Selecting another card must not detach its predecessor or recreate its process.
  Make the session collection observable and handle an empty workspace, failed
  launch and removal of the selected entry without dangling focus or dialog targets.
- Preserve current layouts, themes, density controls, keybindings and the isolated
  QML preview workflow. Live mode shows real session entries; fixture cards remain
  in preview mode. This milestone does not redesign the interface.
- The service remains authoritative for each source's pending requests and
  response validity. Desktop workspace policy combines those requests for display
  and navigation; viewing, pinning, snoozing or changing focus cannot resolve or
  approve one. Queue identity includes the session and typed source request ID,
  with epoch/revision checks on actions.

#### Checkpoint 3A: retained sessions and manual switching

Checkpoint 3A passed the two-session functional checks recorded in the
[workspace receipt](../evidence/milestone-three-workspace.json). The implemented slice is:

1. Create, explicitly adopt and reconnect individual entries. Start new shell or
   managed Codex sessions using the existing launch validation and approval-policy
   inheritance. Reopening the workspace reconnects recorded identities; it never
   automatically replaces a dead service, launches a new child or resends input.
2. Replace the hard-coded live-plus-fixture list with actual session entries and
   wire existing card clicks and navigation bindings to the selected identity.
   Closing the workspace detaches all connections and leaves services running.
   Removing an entry detaches it; it does not terminate its CLI. Process exit or
   service loss remains visible with an explicit reconnect/new-session choice.
3. Centralize keyboard ownership before enabling switching. A key sequence,
   paste or composition stays with its originating session. Defer a switch during
   an in-flight paste or held-key sequence; handle manual composition changes with
   an explicit finish/cancel boundary so late IME events cannot reach the new
   session. Preserve each session's history position and retained live screen.
   History views stay read only until the user explicitly returns to Live.
4. Resize only the selected interactive terminal to the active pane. Background
   sessions retain their last geometry; scaled previews never resize their PTYs.
   Continue consuming background output with bounded updates, throttle visible
   previews and stop drawing hidden surfaces. Preserve per-session bounds and
   account for the combined workspace snapshots, queues and render caches.

**Exit evidence:** two distinct service identities and child PIDs; output from
both while switching; targeted key/paste/resize traffic; retained history positions;
GUI close/reopen restores both same children; failure of either session leaves
the other usable. Exercise simultaneous archive writes and eviction under the
shared history-root quota, including contention/failure without losing either
retained live screen. Keep automatic switching disabled for this checkpoint.
The receipt records two real shells, production QML creation and all four layouts,
separate service/child identities across GUI reopen, shared-quota archive writes
and failure isolation, and native IME commit followed by a guarded switch.
Entries are persisted only after the first verified handshake; closing during
initial connection is outside this retention guarantee. The eight-entry storage
bound is not an eight-session qualification. Cross-session Codex requests and
workspace latency/memory baselines are qualified by checkpoints 3B and 3C below.

#### Checkpoint 3B: workspace attention and explicit decisions

Aggregate requests without moving cards or keyboard focus. Show per-session
counts and an inspectable workspace queue; select a request explicitly to open
its session-bound dialog. Keep drafts and provisional-send state attached to the
originating request. A modal dialog blocks navigation that would invalidate its
input owner; source loss disables submission and a session removal safely closes
or invalidates the target.

Use a bounded deterministic queue, a monotonic clock and explicit tie-breaking.
Do not compare undocumented clock epochs from different service processes.
`WorkspaceSupervisor` owns this GUI-thread aggregate, with at most eight sources
and 128 requests per source. It observes the existing session models and routes
review/snooze by stable session ID plus the complete opaque request token. Service
IPC remains v6. The supervisor supplies queue display and navigation policy; the
originating session still validates and sends every explicit decision. Dialog
drafts are retained only for their exact source/request token, never a card index.
Reconnect must reconcile a session before enabling replies, and cannot erase a
healthy neighbor's queue entries. Keep the service's exact token checks for every
response; matching numeric or string request IDs in different sessions are distinct.

**Exit evidence:** simultaneous real requests from two Codex sessions, including
approval and structured user input; each explicit response reaches only its
originating source and receives matching resolution/continuation. Deterministic
cases cover colliding IDs, duplicate delivery, cancellation, stale decisions,
reconnect and removal while a dialog or draft exists. Pending background attention
never changes the current typing destination.

#### Checkpoint 3C: guarded, opt-in carousel

Enable automatic navigation only after 3A and 3B pass. Keep it off by default and
off after reopening the workspace. Add pin-current-session, request snooze and
explicit pause/resume controls. Pinning blocks automatic departure without
reordering cards or preventing manual navigation; snoozing suppresses automatic
surfacing until its deadline without dismissing or approving the source request.

One focus policy arbitrates manual and automatic navigation. Automatic changes
require an active lapis window and an idle interaction state: no recent typing,
held keys, paste, composition, modal work, drag or selection gesture. Manual
navigation takes precedence and starts a cooldown. Recheck destination identity,
readiness and interaction state at execution; automatic switches are never queued
or replayed after reconnect. Use aging and cooldowns to
avoid repeated requests from one session starving the other, including a quiet
eligible session with no pending request. Respect reduced
motion, and never wait for an animation before accepting input.

The supervisor uses the local steady clock, with an injected clock for
deterministic qualification. Initial policy intervals are 1.5 seconds of input
quiet, a 3-second manual-navigation cooldown, a 5-second minimum dwell, and a
15-second fairness interval for eligible quiet sessions. These are navigation
defaults, not performance gates. The window host reports activation and user
interaction; the existing workspace focus owner applies automatic requests
synchronously after checking readiness and interaction blocks. Only manual
requests can enter the deferred focus queue. Carousel state and snoozes are
ephemeral, so reopening never enables automatic navigation.

**Exit evidence:** deterministic clock-driven ordering, aging, snooze, pin and
cooldown cases plus actual macOS GUI tests while typing, holding keys, pasting,
composing, opening a response dialog and leaving lapis inactive. Neither output
nor an incoming request can steal another application's OS focus or split input
between sessions. Disabled and paused carousel modes leave manual operation intact.

#### Integration, verification and completion

The existing seams are `Workspace`/`SessionPreview`, `LiveConnection`,
`TerminalSurface`, the attention view/dialog and the per-session descriptor API.
Define the manifest v1 and session/focus routing contracts before parallel edits.
Contributors share the milestone; assign temporary file scopes per task, one
integration coordinator and one build owner per build directory. Independent
registry, UI investigation and verification work can proceed concurrently; focus,
attention and registry integration share one reviewed contract.

Apply the [required-check matrix](../CONTRIBUTING.md#checks), including
the `workspace`, `workspace-registry` and `workspace-supervisor` CTest suites in the normal desktop run.
On macOS, run the opt-in workspace UI probe serially with other GUI checks:

```sh
build/desktop/apps/desktop/lapis_workspace_ui_probe \
  --json-file build/workspace-ui.json --output-dir build/workspace-ui
```

The [workspace test procedure](../CONTRIBUTING.md#checks) also specifies the
two-source live Codex probe and measurement limits. Native input includes the
workspace composition and paste guards in
`just native-input`. Also run relevant ASan/UBSan and TSan suites, quality
checks for Python, and affected CLI/UI probes. Keep GUI runs serial. Automated
macOS key, Option, paste and native IME checks are required; physical typing is
not a completion gate. Replay tests supplement, and do not replace, the two live
session sources required for the milestone.

Record two-session warm-switch and input/frame timing distributions, retained
memory and idle/background activity with the existing profiling procedure. These
establish a baseline; provisional latency targets are not new pass/fail gates.
State workload, output rates, source revision, display rate, binary/provider
identity and instrumentation endpoints. Store sanitized assembled evidence under
`evidence/`, raw logs under `build/`, and update README only as each behavior lands.

The milestone completes only when all three checkpoints pass together on the
same assembled source. The original three-checkpoint qualification excludes
Linux UI, a second CLI adapter,
32-session qualification, multi-window/multi-client attachment, automatic recovery
after service death or reboot, new terminal selection/accessibility/shaping
features, renderer replacement, packaging and binary distribution. Preserve
portable boundaries and existing terminal behavior while these remain deferred.

A failed registry load or save pauses registry mutations and reports the failure;
live sessions remain attached after save failures. **Session → Retry workspace**
performs one explicit attempt after the cause is corrected: it reloads a manifest
that has not loaded successfully, or retries saving the retained session entries.
Retry revalidates ownership, permissions, file type and existing contents through
the normal read or atomic-write path. Persistent corruption remains untouched, and
controls stay disabled until the operation succeeds. There is no timed retry loop.
The workspace tests exercise lock contention, repeated failure, corruption refusal,
repair and successful persistence without restarting live services.

#### Claude Code hooks: an observation-only extension

Claude Code sessions use the same retained terminal, workspace queue and guarded
carousel. Choose **Claude** when adding a session, or launch an explicit session:

```sh
python3 scripts/lapis.py run --claude --socket runtime/claude.sock \
  --cwd /absolute/project -- claude
```

The internal agent-mode enum adds `claude`; workspace manifest v1 stores that
name and launch fingerprints use a separate
`lapis-claude-v1` domain. Service IPC remains v6.

The session service owns a private local hook listener and temporary settings
file for the life of its Claude PTY. It passes those hooks using `--settings`,
preserving normal user/project settings and permission policy. Explicit
`--settings`, `--bare` and `--safe-mode` arguments are rejected for managed
launches because they conflict with this contract. Globally disabled hooks or
managed policy may prevent delivery; no observed session boundary means attention
is unavailable, while the terminal remains usable. Existing independently
launched Claude processes are not automatically observed.

Command hooks send only bounded event and identity metadata through the private
socket. They never return decisions, permission changes, prompt context or stdout;
transport failure returns successfully to Claude. No raw prompt, tool input,
output or transcript enters the attention ledger. Hook commands use a fixed
quoted executable/socket/token; event contents never become shell commands.
The listener and ledger survive desktop detach. Reopening the GUI restores that
service's ledger, not a reconstructed Claude event history.

`SessionEnd` retires the conversation's notices without closing the service-owned
listener. A subsequent `SessionStart` with a fresh source identity begins a new
observation epoch, so `/clear` can continue in the same Claude process. Delayed
hooks from retired sources cannot rebind them. The observer remembers up to 1024
retired sources and stops observation on overflow. Reopening an already retired
conversation is not qualified; start a new managed session for that case. Process
exit or explicit observer shutdown still closes the listener. State changes are
immediate; observer notifications are coalesced onto the service event loop after
transport callbacks return. A notification receiver may stop or destroy the
observer safely, and repeated stop calls are inert. The
[PR #10 repair receipt](../evidence/pr10-review.json) records the reproduced
failure, live `/clear` recovery and retired-source regression cases.

The adapter declares observation only: response and authoritative reconciliation
are unsupported. A well-formed unsupported decision from a wire client
is rejected with a terminal-only diagnostic while preserving its attachment.
Only an exact pending source/epoch/request/revision receives a retry token; no
decision is forwarded to Claude. Malformed and stale-attachment frames retain
the normal protocol rejection behavior. A verified initial `SessionStart` or `UserPromptSubmit` boundary
starts an empty observation ledger; the shared reducer cannot use that operation
to replace stale pending requests. Local delivery order is not an upstream
sequence or proof that no hook was missed. Service restart and delivery loss
cannot recover unobserved requests automatically.

Claude 2.1.280's actual `PermissionRequest` payload has `session_id`, `prompt_id`
and `tool_name`, but no `tool_use_id`. An `AskUserQuestion` input notice already
covers its accompanying permission hook, so that hook adds no duplicate row.
Other imprecise notices are session/turn scoped and remain advisory until a new prompt or completion boundary. A neighboring tool's
completion cannot resolve them. `PreToolUse` for `AskUserQuestion` and any
requests with an exact tool ID can be retired by the matching `PostToolUse` or
`PostToolUseFailure`. Permission/idle notifications are advisory; silence and
absence of post-tool events do not establish approval, denial or task completion.

Ordinary tool completions use a separate adapter-local set capped at 16,384 distinct
IDs per valid prompt epoch. Only exact pending attention requests consume the
shared reducer's retired-request budget. Delayed input or permission hooks for a
completed tool cannot resurrect it; duplicate boundaries retain those IDs. A new
verified prompt resets them, and exhaustion reports loss of observation rather
than silently evicting identities. The eight-connection listener reports refused
connections without treating unauthenticated traffic as a source-state reset.

The installed 2.1.280 binary emitted `PostToolUseFailure` for a controlled failed
Read against a temporary missing file. This check used synthetic model replies
from a loopback API, separately from the real-provider permission/input probe.
A controlled HTTP 400 in print mode did not emit `StopFailure`; that event is not
registered until its delivery is qualified. This does not establish its absence
in every CLI mode. The nine registered events retain the existing terminal-only
response policy. See the [review follow-up receipt](../evidence/pr10-review-followup.json).

All Claude notices have empty response choices and instruct the user to answer
in the originating terminal. The supervisor separately tracks attention
eligibility and response availability, so a current terminal-only notice can
receive carousel priority without enabling GUI approval. Arrival, review,
snoozing and focus never send an answer. The existing typing, paste, IME, modal
and inactive-window guards still apply.

This extension does not claim Milestone 5's independent response/reconciliation
qualification or Linux UI support. Runtime qualification uses a disposable Claude
configuration and an explicitly selected test provider; normal launches inherit
the user's configuration. The
[Claude hook receipt](../evidence/claude-code-hooks.json) records separate runtime
and GUI evidence; reproducible procedures are in CONTRIBUTING.

### Daily-use agent workspace direction (September 21 review)

#### Penthouse comparison (September 22, read-only Opus 5.5 review)

The Mac has three separate app implementations, not interchangeable clones:
`dev/tools/penthouse` at `8baeb32` is the early Tauri room/timeline shell;
`dev/penthouse` at `81b0601` is the archived Rust/GPUI chat-first app;
`dev/tools/ghostty` at `64a549f1e` is the later terminal-first fork, whose launcher
selects `macos/build/ReleaseLocal/Penthouse.app`. The separate marketing site is
at `dev/sites/penthouse.sh`, revision `db47ca1`. No running Penthouse app was
observed; a current production deployment was not established. Both later app
repositories call themselves paused as of August 21.

A second Linux host's `dev/penthouse-spike` is an unversioned remote-driver copy
with no desktop app. Its older single-file driver exactly matches the Mac's SSH
spike. Of 19 shared driver/schema/replay files, 8 match and 11 differ; none
exists only on that host. The Mac has later native-resume, agent-option,
model-discovery, OMP and tool-update work. The Linux test host has hook/resume
helpers but no app checkout found in the
home-directory search through depth four. No Penthouse files were changed or
merged. Local receipts: `build/penthouse-review/reconciliation.json` and
`build/penthouse-review/opus-review.txt`.

The review's adaptations are now implemented in the desktop presentation layer,
with no service, protocol or dependency change:

- [x] Quiet hierarchy. Categories are unfilled group labels: the active one has a
  2-pixel leading focus edge and full-weight text, and the first four show a
  fixed-width recall number for their shortcut. Agent tabs remain the filled
  session cells. The narrow selector uses the same edge and an outlined `+N`
  count for requests in hidden categories.
- [x] Semantic colors. Each theme adds `activity` and `fault` tokens.
  `focusedBorder` marks selection and terminal keyboard ownership; the stage edge drops to the
  neutral border whenever a dialog owns input. `attention` means pending
  requests only. A lost connection and form/workspace errors use `fault`; an
  ended process is neutral. Every status class also has its own tab mark: dot
  working, diamond pending request, ring ready or turn finished, square lost
  connection, dash ended, hollow box opening or unknown. The selected tab keeps
  its text label.
- [x] One configurable fixed-width family. `terminalFont` in `lapis.json`
  (`family`, `size` 10–32 pixels, default 16) is edited only in Appearance and
  applies live. The GUI resolves it; a missing or proportional family falls
  back to the platform fixed-width font and Appearance says so. The terminal,
  paths, shortcut keycaps, status labels and counts share the resolved family;
  names and prose keep the UI face. Explicit ANSI colors are untouched.
- [x] Tactile controls. Commands separates keyboard selection (filled row with a
  focus edge) from pointer hover (lighter wash), and shows shortcuts as
  fixed-width keycaps. Appearance uses one hover/press/selection treatment, a
  font family picker previewed in each face, and a size stepper. Feedback is
  color only, within the theme's motion duration; reduced motion and
  zero-duration themes change instantly. Dialogs open without an enter
  transition, so typing is never gated. The sidebar remains one discrete resize.
- [x] Stable tab geometry is preserved through selection, status changes and
  long names.
- [x] Preserve opaque terminal rendering. The Ghostty fork's ink shader classifies
  pixels by brightness (`smoothstep(0.16, 0.34, brightest)`), so it can replace
  intentional dark terminal colors, not just the background. This is a concrete
  fidelity concern from source inspection, not a measured rendering result.

A font change rebuilds the retained terminal rows once and requests the new cell
grid as one resize; a failed configuration save rolls back without applying the
tentative font. Linux software-rendered tests cover font persistence and
rollback, live grid change, fallback, semantic state distinction, the stage
focus edge and five viewports in both default and minimal-density/24-pixel
variants. The synthetic fixture scales its fixed-size snapshot to the stage, so
larger terminal text is verified through the requested grid rather than those
captures. Linux ThreadSanitizer stopped on reports in Qt startup paths; it does
not provide race-clearance evidence for this change. The coordinator rebuilt
and opened the Mac app with the existing agent, verified Menlo in Appearance
and the shared readouts, and exercised a live text-size change from 16 to 17
pixels and back. The full Mac native IME/paste suite was not rerun. The
[integration receipt](../evidence/penthouse-ui.json) records source hashes,
reused checks and final coordinator checks. Four activity/fault colors were
raised so those text tokens exceed 4.5:1 contrast against both normal and
selected tab fills in all six themes.

The proposed game direction is RTS-style group recall for categories and tabs,
with restrained cockpit typography and game-menu feedback inside Commands and
Appearance. These are design analogies, not evidence that Penthouse copied a
particular game. Do not import the chat/timeline hierarchy or draw scenery over
agent output. The reviewer inferred that all mascots were rejected from removal
of one joke shortcut; that broader preference is unproven and is not adopted.
The review's continuous-resize concern is also source-based, not profiled.

#### Implemented workspace

On macOS the native title bar uses the current window theme color, with its
default gray material, separator and duplicate visible caption removed. Native
traffic-light controls, the titled-window type and title-bar drag region remain
unchanged; the window title is retained for accessibility and window management.
The AppKit helper runs on visibility and theme-color changes and does not create
hidden windows. Linux retains its window-manager-owned decorations. This styling
does not resize the client area or replace native window controls with QML.

The workspace has no sidebar wordmark or separate project breadcrumb row.
Category labels and agent tabs share a top edge and row height. Commands lives
at the right of the tab row; a collapsed sidebar still exposes the category
selector above it. Project paths remain in default tab titles and useful tab
tooltips, including custom-named sessions.

The new-agent form first selects an installed harness, then asks for a project
folder starting at the platform home directory. It supports keyboard folder completion and an optional native
folder picker; no name, model or execution-policy settings are inserted before
launch. Folder discovery uses Qt's asynchronous `Qt.labs.folderlistmodel` module
from the pinned Qt 6.11.2 SDK. Suggestions clear immediately when input changes,
show at most 100 entries and inspect at most 4096 model rows per update.
The allowlisted CLI launchers are Codex, Claude, OMP, Grok, Kimi, OpenCode,
Gemini and Antigravity. Discovery resolves executables from PATH and their
standard per-user install locations; launch revalidates availability. Codex
retains its managed observer and Claude runs under the service's Claude Code
hook adapter. The others use the existing direct PTY transport and have no
activity or approval observation capability yet; a connected TUI does not imply
working, finished or approved. No global hooks, model flags or approval flags
are installed. Native Codex and Claude startup and same-process
reconnection have been exercised without model turns. OMP currently exits even
outside lapis because its configured credential broker is unreachable.
Default new tabs and window titles show the home-relative project path; existing explicit names are retained.
Marks identify the harness, not dynamically detected model-provider metadata.
`qml/AgentMark.qml` reuses Penthouse's Codex, Claude, OMP, Grok, Kimi and OpenCode
`assets/logos` paths; other harnesses use initials. The Codex asset cites
OpenAI's brand page and the Wikimedia 2025 symbol. These remain brand assets,
not new software dependencies or endorsements. Redundant theme-name hover tooltips are removed; truncated tab paths
can still reveal their full location.

#### Agent strip, attention pulse and closing agents (September 22)

The user first asked for IDE-style Command-W and iTerm2-style tiling; split
panes were built and then removed the same evening at the user's request
("we don't want window tiling at all"). A preview strip removed that morning
was reinstated and now replaces the tab row, superseding the "no preview
windows" direction below.

The strip under the stage lists the category's agents in tab order as live
previews and is the category's navigation. Each card is a `TerminalSurface`
with `interactive` off, so it never takes input or resizes a PTY;
`frameInterval: 250` coalesces output redraws to four per second, and
`minimumScale: 0.5` draws the rows ending three lines below the cursor instead
of an unreadable whole screen. The list instantiates only visible cards and
drops its model while hidden. Selection scrolls like Neovim's `sidescrolloff`:
the selected card may approach either edge but 40% of the neighboring card (or
the trailing new-agent card) stays in view. Short windows shrink the cards
rather than hiding the strip; `previewsVisible` hides it. Requests and Commands
sit at the foot of the category rail, or beside the category selector when the
rail is collapsed. Categories take Command-Option-left/right (as originally
approved) and Command-Shift-up/down.

An agent that goes from working to finished or idle, or gains a request,
while another agent is selected is marked `unseen`; selecting it
clears the mark. Its card edge pulses slowly (1.8 s period; ink for a finished
turn, the attention color for a request) and its category shows a pulsing dot;
reduced motion keeps both steady. Activity comes from three declared sources:

- Codex: the managed app-server observer (working, idle, turn completed, requests).
- Claude: new Claude agents launch in `AgentMode::claude`; the session service
  passes its hook relay to that one process and reports turns, permission
  prompts and input requests through the same attention snapshot as Codex (see
  [Claude Code hooks](#claude-code-hooks-an-observation-only-extension)).
  Requests must still be answered in Claude's own TUI. The registry records
  `"mode": "claude"`; Claude agents saved before the adapter keep terminal mode,
  because their running service was created with that launch fingerprint.
  An earlier desktop-side hook file (`--settings <endpoint>.hooks.json`, polled
  every 400 ms) was replaced by the adapter before merge.
- Other harnesses: an output estimate labelled "Output active"/"Quiet": three
  frames within 1.5 s mark activity and 4 s of silence after it a pause,
  ignoring the first 3 s after attaching (screen replay). It is advisory and
  never reads as a finished turn.

Command-W closes the focused agent. An ended or fixture agent closes at once. A
reachable running agent is confirmed and then ended through a client frame,
`terminate` (kind 15, an additive v6 client kind): the service sends SIGHUP to
the agent's verified process group, SIGTERM after 1.5 s and SIGKILL after 3 s,
and the ordinary exit path reports `ended`, after which the card closes and its
right neighbor (or left, at the end) takes the stage. A service built before
this change rejects the frame and drops the attachment while its agent keeps
running; the desktop reports that, reattaches and keeps the card. An
unreachable agent can only be abandoned after a confirmation stating it may
still be running. The window's close button and Command-Q still only detach.
The [receipt](../evidence/agent-strip.json) records the checks.

The session service clears parent-session markers from its environment before
starting any agent or Codex backend. A desktop opened from a Claude Code
terminal otherwise passed `CLAUDE_CODE_CHILD_SESSION`, the parent's session ID
and messaging socket down, and the resulting Claude agent ran as that session's
child with transcript saving off. The list covers Claude Code's session
variables and `AI_AGENT`, Grok's `GROK_AGENT`/`GROK_SESSION_ID`, OpenCode's
`OPENCODE`/`OPENCODE_PID`, OMP's `PI_SESSION_FILE`/`PI_TOOL_BRIDGE_*` and Codex's
sandbox flags. Claude, Grok and `AI_AGENT` names were observed on this Mac; the
others come from the harness binaries. User configuration such as
`CLAUDE_CODE_EFFORT_LEVEL` is preserved. `workspace` exercises the real service.

The OLED black theme (`oled`) makes the background, surfaces and resting cards
`#000000`; borders, text and selection carry the structure, and only hover and
the selected tab light a faint fill. No terminal override is needed: the pinned
Ghostty engine's default background is already `#000000`, so a live agent is
black unless it sets its own background (OSC 11). The navy seen behind an
unreachable agent was lapis's sample palette, which every session's placeholder
screen used to receive; only preview fixtures get it now, so a live agent's
placeholder matches its first real frame in every theme. OLED text and status
colors measure at least 5.6:1 against black, hover and selected fills.

Registry version 2 stores harness identity and an optional validated Codex
`resumeThread` UUID. Version 1 records remain readable and default to Codex.
Older readers reject version 2 rather than relaunching another harness as Codex.
The resume identity maps to native `codex resume UUID`, not an arbitrary command.

The September 22 implementation request supersedes the preview/layout discussion
below: workspace -> category -> ordered agent tabs, with exactly one terminal
stage and no product preview windows. Each category retains its selected agent;
moving an agent preserves its process, history and identity. The Command theme
uses a tactical command-room structure and restrained spacecraft styling.
Categories are user organization, not working/waiting/finished state buckets.

The workspace chrome now exposes category navigation, agent tabs, a small new-agent
button and Commands. Commands is searchable and scrollable, shows configured
shortcuts and explains unavailable actions. Category editing, agent management,
recovery and Appearance remain discoverable there without permanent toolbar buttons.
Command-Shift-P opens it; Command-B retracts the sidebar and persists the preference.
Command-V remains paste, and Command-Shift-brackets stays within the current category.
Default agent tabs show the home-relative project path and carry status.
Truncated paths and custom-named sessions reveal the full path on hover.
Pending requests remain visible.
The [Commands follow-up receipt](../evidence/command-palette.json) records 20 Linux
CTest suites, the full 51-check gate, affected sanitizer suites, and the rebuilt
Mac window with its existing agent restored. Earlier live protocol and native
IME qualification remains attached to the earlier workspace receipt below.

The current implementation adds a private, atomically written workspace registry,
independent per-category selection, agent creation/rename/move/reorder, real status
projection, a responsive rail/selector, coordinated themes, and machine-local
window geometry. The old six-session content remains an explicit automated
fixture only. All 20 Linux CTest suites have passed; current Mac Codex 0.155.1 passed
live headless approval/input/reconnect/cancellation qualification, and two real
Codex processes survived workspace destruction/restoration with unchanged PIDs.
The full analyzer gate, all 20 ASan/UBSan suites with leak detection, 15 service/GUI
integration cases and 10 capture checks pass. The scheduled final Mac pass also passed all 20 native keyboard/IME cases and
12 live desktop-response cases, restoring clipboard and input-source state.
The final normal workspace was opened and visually inspected on the Mac.
[Current evidence](../evidence/agent-workspace.json) records the delivered build
and its platform limits.


The user's daily-use product is an agent workspace, not a general terminal
emulator. Normal launch must show only actual agent sessions. PTYs and terminal
rendering remain infrastructure for agent TUIs; standalone shell launch and sample
cards belong to explicit development/qualification paths. Demo mode must use the
same presentation components and state contracts with clearly synthetic data,
without adding fixture cards to a live workspace. These are implementation
requirements for the next milestone, not claims about the current build.

Independent source reviews by Fable 5.1 and Grok 4.7 Fast examined `79be97a`.
Their common finding is a product-contract gap: `workspace.cpp` creates a shell
and five fixtures in live mode, while `AttentionSnapshot.activity` reaches the
desktop but is not used by the visible activity text. `LiveConnection::report`
instead fills that text with transport diagnostics. The 980-by-700 minimum and
forced built-in-screen placement also conflict with ordinary tiled-window use.
The review receipt is [product-workspace-review.json](../evidence/product-workspace-review.json).
No GUI replay or new runtime qualification was performed for this review.

The settled direction is one workspace with real retained sessions, a readable
project/agent identity, and a concise state treatment shared by the selected
session and its navigation entry. Keep connection, agent activity, process
lifetime, pending requests and the last turn outcome independent in the model.
Present the action the user needs most prominently, without displaying every
internal state as a competing badge. A stale source must visibly disable responses;
a pending request must remain discoverable even after a turn ends. Working,
ready for another prompt, turn finished, interrupted/failed, reconnecting and
observation unavailable are distinct conditions. Never call a task done based
on a completed turn or output silence. Failed/interrupted turn preservation
requires extending the current lossy mapping, not merely changing a label.
Detailed transport errors remain available through session details and contextual
recovery actions; they are not the permanent top-left headline.

First launch should offer a clear project and supported-agent start action.
Managed Codex is the first qualified adapter, not a promise that every installed
CLI can provide reliable status. Restore saved identities on subsequent launch
without implicitly creating a replacement when an old endpoint is unreachable.
A second agent in the same directory must receive a distinct session identity;
launch arguments or directory hashes alone are not session IDs. Keep unsupported
binary observation/response limitations explicit and preserve qualification gates.
Do not silently fall back to a shell. Closing the window detaches the view;
stopping an agent is a separate explicit action. Dock activation/relaunch must
recover a usable window without accumulating hidden GUI processes. (Superseded
September 22: Command-W now closes the focused agent, as in an IDE; the close
button and Command-Q detach. See "Agent strip, attention pulse and closing agents" above.)

Window placement belongs to the user and OS. Normal launch should restore valid
geometry or let the OS place a new window. Explicit screen overrides remain for
qualification. Restore against currently available displays and recover an
accessible frame after a display is removed. Window geometry is machine-local
runtime state, not a reason to rewrite tracked project configuration. Saving geometry
may tighten a current-user-owned real runtime directory to mode 0700 when a project
tool created it with the default umask. The repair uses an open directory descriptor
and rechecks its identity; symlinked or foreign-owned directories and permissive
existing geometry files remain rejected. Restoring geometry does not change permissions.
Native window-manager positioning should work without a dedicated Raycast integration
or changes to global shortcuts.

The original review suggested retaining layout choices. The later category-first
request instead selects one terminal stage and removes the product layout picker.
Adapt navigation to the actual viewport and real session count. Narrow or short windows collapse the preview rail/strip
before sacrificing the active agent. Dialogs must clamp and scroll, controls must
remain reachable at larger font sizes, and thumbnails must not resize a PTY.
An intentionally active tiled terminal needs an explicit readable-size policy;
otherwise fall back to a focused pane instead of shrinking the agent to a tiny
interactive preview. Exact breakpoints and minimum dimensions remain test targets,
not conclusions from estimated font metrics. Avoid treating a wallpaper of tiny
terminal previews as the primary session-navigation mechanism.

On macOS, use familiar Command-based workspace navigation, preserve terminal
Control chords and Command-left/right line editing, and keep all bindings
reconfigurable. Linux gets appropriate platform defaults in the same contract;
its qualification is already underway and is not deferred by this review.
Verify actual Qt shortcut precedence rather than assuming keyPressEvent proves
which handler wins. Navigation must respect held keys, paste, IME and modals.
Opening a request may select its details, but must not pre-arm an approval or
make an incidental Return approve a command. Keep explicit responses, preserved
drafts and no attention-driven focus stealing.

Implementation order and acceptance:

1. Establish the live/demo boundary, a truthful status presentation and an
   actionable empty state, sharing presentation with the explicit demo. Cover
   unknown/stale sources, simultaneous requests and failed/interrupted turns.
2. Integrate with the other developer's current multi-session work before
   assigning overlapping files. Deliver two real retained agent sessions with
   separate identities, creation, manual switching and restoration. Qualify one
   noisy session beside an interactive session; closing the GUI must preserve
   both, and reopening must not start duplicate agents.
3. Qualify keyboard routing and adaptive/native window behavior together. Proposed
   viewport cases are 640x480, 700x900, 980x700, 1400x960 and 2560x1080 logical
   pixels, including larger fonts, long names, multiple requests, reduced motion,
   screen removal and each supported layout. Verify bounds, input ownership and
   readability, not just that a screenshot can be written. These cases have not
   yet been exercised. Routine coverage stays on the Linux test host; native Mac window-manager
   and input acceptance is separately scheduled.

Automatic carousel, 32-session scale, additional adapters and packaging remain
later milestones. Do not add an unmeasured threading rewrite to solve a styling
problem. Conversely, C++ alone proves no memory advantage: measure GUI, service
and agent memory separately, warm-switch/input tails and idle work on the same
workload before making comparative claims. The main product risk is truthful
looking but stale agent status; source epochs and reconciliation gates must remain
visible in behavior even when diagnostics are visually quieter.

#### Break-it QA, selection and follow-up fixes (September 23)

`tools/qa/breakit.py` drives the real GUI on the Linux test host's isolated display with
synthetic X11 input. Its agents are `tools/qa/fake_agent.py` installed under
harness names on a private PATH; scenarios cover creation, typing while
switching, finish pulses, floods, categories, closing running and signal-ignoring
agents, crashes, GUI SIGKILL and quit/restore, service SIGKILL, rapid shortcuts and
five window sizes, checking the registry, processes and GUI warnings. `--soak`
runs 32 bursting agents across four categories and samples GUI/service memory and
CPU. `scripts/fake_models.py` serves scripted OpenAI Responses and Anthropic
Messages replies so the installed Codex and Claude Code binaries can be exercised
without model usage; Codex's question tool needs Plan mode. Two Codex Computer Use
sessions drove an isolated macOS copy; a copy started by macOS without its fake
environment reached real models once, so such copies need an environment that
survives any launch path and a guard that stops services without it.

Decisions from these runs:

- The committed `lapis.json` carries no keybinding overrides. It had kept the
  pre-strip bindings, which replaced platform defaults on fresh checkouts
  (Control-W detached the window and Control-Q quit, both terminal chords).
- Claude Code's `idle_prompt` notice is advisory in the desktop: snapshots drop
  `idle` requests, so a finished Claude turn reads **Turn finished** and pulses
  once instead of showing a request with no response. The adapter still reports
  it; other clients may rank it.
- A new Codex 0.155.1 agent has a thread but no rollout until its first turn;
  `thread/resume` answers "no rollout found" and the observer retries each
  second. The observer keeps "Waiting for Codex thread history" through those
  retries, and the card reads **No prompt yet** (held across an older
  service's retry messages). Other reconciliation still reads **Status pending**.
  Verified with a fresh agent against the fake model, not only from source.
- Ended or unreachable agents get a stage bar with the service's reason and the
  close key. Agents sharing a default title are numbered. Narrow cards drop the
  status text; the new-agent card drops its hint rather than overflow. A card's
  accessibility press action selects it.
- Selection is in screen cells. The selected text is captured when chosen, so a
  redrawing agent cannot change what is copied; the highlight clears when that
  text moves or changes, or on typing. Command-C (Control-Shift-C elsewhere)
  copies and never reaches the agent. The wheel pages archived history on the
  normal screen and sends arrow keys on the alternate screen.
- Endpoint permission errors name the directory and the `chmod 700` fix; existing
  directories are still never chmodded.
- The Dock badge (`QGuiApplication::setBadgeNumber`) counts agents that finished
  unseen or have an actionable request, across categories. A new request while
  the window is inactive calls `QWindow::alert(1000)`: one bounce, not a
  persistent alert; finished turns only update the badge.
- `nextAttention` (Command-J) is the manual half of the attention queue: it walks
  categories and strips from the selected agent, taking pending requests before
  unseen finished turns. Pinning, snoozing, aging and the opt-in carousel remain
  unported from the flat supervisor.
- `harnessArguments` in `lapis.json` is explicit user configuration for new
  agents (shell aliases do not reach lapis launches). The registry saves each
  agent's full argument list, since arguments are part of the launch
  fingerprint; adapters still reject conflicting flags such as Claude's
  `--settings`.

Each agent and each Codex backend runs under a group guard process that kills its
group when the service's pipe closes; SIGKILL of the service alone removed the
backend on the Linux test host. Guards share the service's command line, so cleanup that kills
every matching process also kills the guards and leaves backends running.
Session restore (September 23, requested to match an iTerm2 restore plugin). A
card still in the workspace whose service is gone when lapis opens is restarted
in place, with `AttachMode::create` at its endpoint, instead of staying
unreachable; this deliberately replaces the earlier rule against implicitly
replacing an unreachable endpoint, and Command-W remains the way to remove an
agent. Each service keeps `<endpoint>.resume` as owner-only JSON with the agent,
conversation and provenance. Version 2 distinguishes an independent observer
from advisory OSC 1337 `SetUserVar=agent_checkpoint=<base64 JSON>` printed in
terminal output. Printed fields cannot claim observer provenance, replace a
managed Codex/Claude observer, or downgrade an already observed record. Version 1
records remain readable as advisory until a current observer or the Codex rollout
probe confirms the conversation. Unknown versions and malformed sources fail
closed.

Automatic resume uses only observer records. Codex and Claude currently provide
that independent channel; other CLIs' printed checkpoints remain advisory and
restart fresh unless the user explicitly supplied a resume argument. Remote
hosts, unknown agents and identities that could be options are refused. When
lapis adds a resume pair, the registry records its index and identity as
`managedResume`; later verified observations update only that pair. An advisory
record retires a still-matching lapis-owned pair. Stale provenance loses automatic
updating instead of making the whole workspace unloadable. Explicit user
arguments, including `--option=value`, remain authoritative. A managed append
cannot exceed the same 64-argument limit enforced by the registry loader.
Claude and Codex also require a saved transcript or rollout. The known native
flags for other harnesses remain available to explicit configuration and future
verified adapters; an OSC payload alone does not qualify such an adapter.
A service is gone only when its socket refuses or is missing. Real Claude Code
2.1.280 and Codex 0.155.1 were checked headless against the fake model: the
record appeared, and the resumed agent showed the earlier exchange.

Continuity across builds (September 23). Quitting one lapis build and opening
another must leave running agents untouched, so these are compatibility
contracts rather than implementation details: the launch fingerprint (pinned by
known SHA-256 values in the `launch-spec` suite for terminal, Codex and Claude
modes), service IPC version 6 with only additive frames, registry versions 1
and 2 on read, the `LAPIS-S1` descriptor, and the
Claude hook relay's command line. Resume records now write version 2 and read
version 1 without assuming observer provenance. Changing these contracts needs a migration that
still reattaches services started by the previous build. For Codex services
started before resume records existed, the desktop recovers the thread every
60 seconds: it finds the app-server by its exact `app-server --listen
unix://<endpoint>.codex` command line and reads the rollouts it holds open
(the rule is under Restore at login below); this was checked against real
Codex 0.155.1 with the fake model after deleting the record. Restart agent (Commands) applies
the restore path to one ended or unreachable card and refuses while its service
answers. Explicit Antigravity resumes use `agy --conversation`; its printed
checkpoint does not authorize automatic resume.

Restore at login and after power loss (September 24, requested because losing
agents to a reboot is the user's main pain point). `lapis_desktop
--restore-agents` runs the restore path without a window (offscreen Qt
platform): it restarts only cards whose services are gone, waits up to 90
seconds for each to accept input, and exits, leaving services it did not start
for a window to reattach. `scripts/restore_at_login.py` installs it as the
`dev.lapis.restore` LaunchAgent with the installing shell's PATH, locale, shell
and explicitly set `CODEX_HOME`, `CLAUDE_CONFIG_DIR` and `LAPIS_HISTORY_ROOT`,
since agents inherit the helper's environment. Other environment variables,
including provider tokens, are not copied into the plist. Reinstall the helper
after changing these directory overrides. The helper and a window share the registry
lock. The helper writes its process ID to `<registry>.restoring` (owner-only)
while it holds the lock, because QLockFile records the process name rather
than the application name; a window finding that marker waits up to two
minutes for the lock (longer than the helper's own limit). A window allows a
bounded two-second grace period for a helper that has taken the lock but not
yet published its marker, and acquires the lock if that helper exits meanwhile.
A competing window fails after that grace period; a helper finding any holder
fails immediately.

Gaps found by simulated power loss are closed in the service. A Codex build
the observer has not qualified reports no thread, so the service also reads
the rollouts its app-server holds open (`lsof` on macOS, `/proc/<pid>/fd` on
Linux) every 5 seconds until it finds one, then every minute, and records
the conversation in use. Codex keeps every loaded thread's rollout open,
including the previous conversation after `/new` or `/resume` (observed with
Codex 0.156.1), so the rule is the main thread written last: subagent threads,
whose rollout's first line has a `{"subagent": ...}` source and a parent
thread, are left out. Unreadable, malformed and oversized headers are skipped. The desktop's minute
check applies the same rule to services that predate the scan, replacing a
saved thread only when it is still open and another main thread was written
after it. Codex 0.156 listens through a symlink it removes only on a clean
exit; a dead link at the service's own `.codex` path is removed, while one
that still answers is refused.

`scripts/check_restore.py` qualifies this end to end on macOS: real Codex and
Claude Code against the fake model and fake stand-ins for Grok, Kimi, OpenCode
and OMP, started by the helper. Codex and Claude each hold a conversation and
then start another with `/new` and `/clear`. Two rounds of SIGKILL on every
process, with stale sockets left behind, are each followed by the helper; the
second runs as a launchd job with the LaunchAgent's minimal environment
(launchd kills a job's process group when it exits; services leave it through
`startDetached`'s new session). Codex and Claude must show their earlier exchange,
accept a follow-up, and keep the verified identity with exactly one resume
argument; the fake model must receive the growing conversation. The four
OSC-only stand-ins must restart fresh without injecting their advisory IDs into
argv. The probe uses the existing CLI test wire client and an ephemeral local
model port, so it runs independently of the phone PR and other local listeners.
A final helper run with everything alive must restart nothing.

CLI updates (September 24, requested so agents never open on an update
prompt). Before a new agent starts, the desktop runs that CLI's own
non-interactive update command (Claude `update`, OMP `update`, Grok `update`,
Kimi `upgrade`, OpenCode `upgrade`, Antigravity `update`) with no input and a
two-minute limit, at most every 30 minutes per CLI; the card shows Updating
<CLI> and starts the agent when the update ends, whatever its outcome, and
the output is logged beside the registry. On POSIX, the updater owns a new
process group. A guard retains group membership after the leader exits, so
cleanup never signals a recycled leader PID. Normal exit, timeout and desktop
teardown stop the group, including installer children that remain in it. Queued
agents wait for both leader exit and the guard's cleanup acknowledgment.
Repeated restarts cannot bypass the queue or create duplicate services.
Output is drained during execution into an 8 KiB tail, with the existing
600-character log limit. Running agents keep their binary.
Codex is not updated: the observer accepts only qualified binary digests, and
an unqualified build loses turn status and requests, so lapis keeps the
qualified build and defaults Codex to `check_for_update_on_startup=false`.
Newly spawned restored Codex agents receive this default too, while existing
explicit config overrides and live reattachment arguments remain unchanged.
Literal prompt words after `--` do not count as config options. The saved-argument
cap remains 64: if there is no room for the default pair, the original arguments
are preserved and the omission is diagnosed instead of breaking registry reload.
Automating Codex requalification (the probes against the fake model rather
than a live one) is the step that would let Codex update too. Restored and
reattached agents are not updated.

Explicit Claude creation through `--new-session` uses the same update queue as
managed new agents. Reconnect and discovery retain their attachment semantics
and never run an updater. Deferred explicit starts retain the normalized socket
endpoint, and their update log lives beside that endpoint. The
`--no-harness-updates` flag disables updates for pinned-binary qualification and
operator-selected launches; fixture `WorkspaceOptions` keep updates off by
default. Codex remains pinned, and arbitrary explicit programs do not gain an
inferred updater command.

Phone access (September 23, requested for use on the go without signing in).
A prototype, deliberately simpler than the SSH design first proposed:

- `apps/remote/lapis_remote.py` is a standard-library gateway on the Mac. It
  reads `runtime/workspace.json` and attaches to an agent's service only while
  the phone shows that agent, with the registry's launch fingerprint (checked
  against the pinned `launch-spec` values). It joins (attach mode 3): the
  service keeps the desktop attached and adds the phone as a view with its own
  attachment tuple, first-screen acknowledgement and input; at most four views,
  which may type, paste, press keys, resize and page history (replies are
  routed to the attachment that asked) but not answer requests or end the
  agent. The phone loads older pages as it scrolls within a screen of the top,
  gathering more than a screen of rows per load since pages can be small, and
  appends newer pages while history is shown so it stays contiguous with the
  live screen. The desktop reattaching never retires a view. Each
  client's last requested size is kept; a resize or typing from a client
  applies it, like tmux's `window-size latest`, so both devices always show the
  same screen at the size of the one in use. When the view whose size applies
  leaves (the phone closes the agent or goes to the background), the service
  applies the desktop's size again. The desktop also claims its size when
  someone comes back to it without typing: the window activating, the agent
  shown on the stage, or the pointer moving over it. It resends its size once
  per size shown, and ignores hover events repeated at a resting pointer, which
  Qt Quick sends as the scene changes, so a busy agent under an idle cursor
  cannot take the size from the phone. A service started before joining existed
  rejects the mode; the gateway then takes the agent over in discover mode (the
  desktop card reads replaced until Reconnect agent) and tells the phone.
  Screens go out as server-sent events of styled runs (text,
  colours, style, starting column and width in cells, so the phone fills whole
  rows without seams), at most one per 50 ms; input comes back as POSTed text, paste, named keys (encoded by the
  service for the terminal's modes) and resize. The phone's grid is applied to
  the PTY; the software keyboard covering the screen does not resize it.
  Width changes, including rotation while composing, also update the row count.
  Input POSTs are serialized, including compound paste/Enter requests. Closing
  a view cancels its pending input; overload is reported after 128 queued requests.
  Brief inactive transitions preserve the stream and loaded history; returning
  from the background reattaches it. Immutable history pages retain their wrapped
  rows and accessibility text until the page set or fitting width changes.
- Admission replaces keys or pairing: the gateway binds only the Mac's
  Tailscale address and serves a request only when `tailscale whois` gives the
  Mac owner's login on an iOS device, or the Mac itself. The Mac's tailnet is
  shared with other people and tagged servers; of its 64 peers on
  September 23 none was admitted, only the Mac itself. Requests with an Origin header,
  without `X-Lapis-Client`, or with an unknown Host are refused, so a web page
  on the phone cannot drive an agent. Plain HTTP relies on WireGuard; the app's
  transport exception is limited to `ts.net` names and local addresses.
  This assumes active Tailscale on both devices and a trusted configured gateway;
  ATS exceptions do not authenticate an arbitrary LAN endpoint. Explicit HTTPS
  URLs retain TLS, and schemes other than HTTP/HTTPS are rejected.
- `apps/ios` is a SwiftUI app (iOS 17+) with categories and agents, an agent
  screen drawing the cell grid, a key bar and a message field that pastes and
  presses Enter. Block elements and box drawing are drawn as cell shapes, as
  terminals draw them, so logos and borders join across rows; rows wider than
  the phone (history archived at a desktop size) wrap instead of scrolling
  sideways. `scripts/check_ios_remote.py` compiles it and its UI tests
  directly and runs them in a headless simulator against disposable services.
  Its loopback fixture supplies synthetic Tailscale status metadata and checks
  gateway readiness; it does not qualify real tailnet admission. It runs
  with a Mac-side client attached the way the desktop is, which must see the
  phone's typing, answer it and never be replaced;
  The agent menu's Send screen to Mac posts a screenshot and the frame it drew
  to `runtime/phone-captures/` (owner-only files); missing-window and encoding
  failures use the existing notice alert. `scripts/install_ios_app.py`
  signs a device build with the development
  profile and installs it with `devicectl`. Both bypass Xcode's build service,
  which deadlocked on this Mac: the kernel's pipe memory was exhausted by
  long-running agent processes, leaving new pipes 512 bytes deep.
- Not yet: structured requests and approvals on the phone (agents' own prompts
  are answered through the key bar), push notifications, and restoring agents
  without the desktop open, which needs a per-user background process; these
  remain proposed.

Observed but not changed: Linux TSan reports frees and mutexes on Qt's uninstrumented
threads in five GUI suites, identically on the pre-merge base, so TSan remains a
macOS qualification. Native Mac selection, wheel and window-manager behavior are
not yet exercised.

### Following milestones

With the two-session workspace assembled, the next qualification stages are
scale, another independent adapter, and platform completion. A later Linux port
still needs actual input, rendering and lifecycle evidence. These stages remain
planned; they are not implied by Milestone 3 passing.

| Milestone | Dependency and owner | Exit evidence |
| --- | --- | --- |
| 4: scale and responsiveness | Working multi-session desktop; verification owner | Controlled 32-session output/TUI workload, p50/p95/p99 input/switch/frame results, memory growth and idle CPU/GPU; distinguish synthetic replay from real agents |
| 5: independent adapter and platform completion | Stable adapter capability contract; separate adapter/platform owners | Second CLI independently exercises observation/response/reconciliation; macOS and named Linux backends have actual lifecycle, native input and rendering evidence |

Dependency notices, a complete bundled inventory/SBOM and redistribution obligations
must be closed before publishing binaries. This release requirement is independent
of a local milestone passing. Keep current implementation status in the README;
the tables here define work order and acceptance only.

## Contracts to preserve

**Session identity and backends.** Each session has a stable lapis ID. Terminal
sessions own PTYs and engines; structured Codex sessions own app-server
processes/connections and conversation state. Structured sessions do not reproduce
the Codex TUI or satisfy terminal acceptance. A separate app-server does not
observe independently launched CLIs by default. The
[Codex investigation](../adapters/codex/README.md) retains the route details.

**Attention core and draft integration semantics.** Keep connection, activity and a set of
pending requests independent. Events carry a contract version, session/adapter ID,
source-connection epoch, sequence, receipt time and kind; preserve source
thread/turn/item IDs and typed request IDs. Kinds cover connection/disconnection,
activity change, attention requested/resolved and reconciled snapshots. Requests
include reason, bounded summary and source confidence. Turn completion is not
process exit or task completion; silence is unknown. Use monotonic time for
scheduling, aging and cooldowns; wall-clock time for presentation/auditing.
The current per-session desktop wire format is v6; extend it only through a
reviewed contract change.

**Two reconciliation boundaries.** Source transport loss makes its requests stale
and disables replies until reconciliation. GUI detachment does not invalidate a
healthy service-owned source connection. A returning GUI refreshes service state
before enabling actions. Deduplicate by source identity/epoch, detect gaps and
retire only the resolved request. Keep attachment identities separate from source
epochs so delayed actions cannot target reused requests. Control-channel overflow
reports loss of synchronization.

**Verified adapters.** Declare observation, response and reconciliation separately.
Prefer supported protocols, then verified hooks; launcher exits and heuristics
supply weaker evidence. Bells/prompt matches are advisory. Hooks use session-bound
local endpoints, stay bounded/nonblocking and cannot silently change execution or
approval policy. Probe dispatch, payload and return semantics before installation.
Structured Codex success does not establish ordinary CLI attention coverage.

**Attention versus focus.** Each service maintains its source request state and
ordering. Desktop policy combines sources for navigation, aging, cooldowns and
presentation snooze without changing their response authority. Desktop session
positions remain stable; support manual
navigation, pinning, snoozing and an opt-in carousel without starving quiet sessions.
Only desktop focus policy assigns keyboard ownership. Typing, held keys, paste,
IME, selection, dragging and modal work defer automatic changes. Automatic
advancement requires the lapis window to be active and interaction idle. Never
split a paste/composition or steal another app's OS focus. Viewing/acknowledging
a request never approves it; explicit decisions use its originating adapter or terminal.

**Bounded rendering and storage.** Service screen state is authoritative; desktop
snapshots are safely replaceable caches, retained while useful. Budget history,
queues and CPU/GPU caches generously per session and globally. Stop drawing
hidden panels while retaining their warm state and consuming output; throttle
previews and let static scenes sleep. Scaled previews do not
resize PTYs. Preserve shaping, wide cells, IME, clipboard, selection and
accessibility. Live objects do not guarantee physical RAM residency.

The later 32-session experiment records workload/output rates, display rate,
p50/p95/p99 input/switch latency and frame times, memory growth and idle CPU/GPU
use. Separate replay from real CLI agents and keep provisional targets distinct
from results. Use [CONTRIBUTING.md](../CONTRIBUTING.md) for commands and evidence rules.
