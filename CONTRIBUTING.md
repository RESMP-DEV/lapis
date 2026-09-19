# Contributing to lapis

This is the shared procedure for human and agent contributors: setup, checks,
profiling and pull requests. Start with [README.md](README.md) for implemented
behavior and [architecture](docs/architecture.md) for the current milestone.
Agents also follow [AGENTS.md](AGENTS.md).

## First contributor baseline

Start from a committed `main` baseline. Before changing code, read the README
status table, architecture's component ownership and current acceptance milestone,
and the checks below. For another checkout alongside an existing one:

```sh
git fetch origin
git worktree add -b feature/my-change ../lapis-my-change origin/main
cd ../lapis-my-change
git status --short
git rev-parse HEAD
```

Choose an unused branch/path; do not overwrite an existing worktree. Record the
starting SHA in the PR. Worktrees share Git history, not uncommitted files,
`build/` or `runtime/`. Install the [toolchain](#setup), then bootstrap the
[terminal dependency](#terminal-dependency) or point `LAPIS_GHOSTTY_PREFIX` at an
existing verified prefix. Keep its adjacent probe receipt and source manifest;
reuse this dependency read-only. Configure fresh build directories in each
checkout; never copy CMake caches or compilation databases between worktrees.

On the currently qualified macOS desktop host, establish the baseline in order:

```sh
python3 scripts/lapis.py doctor    # report which dependencies are ready
python3 scripts/lapis.py check
python3 scripts/lapis.py build
python3 scripts/lapis.py ui-check
python3 scripts/lapis.py cli-check
```

`scripts/lapis.py` resolves the build environment itself, so no `LAPIS_*`
variable needs exporting. It finds the bootstrapped Ghostty prefix, supplies the
socket path, and opens windows on the laptop panel. Equivalent `just` recipes
(`just check`, `just desktop`, `just ui-check`, `just cli-check`) call the same
launcher; run `python3 scripts/lapis.py` with no arguments for the full list.
The underlying scripts still accept the documented variables directly when a
specific prefix or display is required.

The desktop steps require a logged-in graphical session and the exact dependencies
below. Run GUI checks serially, including across worktrees, so windows do not steal
focus from another test. The baseline is not a substitute for the scope-specific
sanitizer, tooling or dependency checks in the [required matrix](#checks).
For the deferred Linux port, record a headless baseline and its platform limits; do not claim desktop
acceptance from a headless pass. Report a pre-existing failure with its command,
SHA and log before mixing repairs into a large change.

### Large changes and parallel contributors

Contributors share ownership and work together on the same feature. Agree on a
compact task brief: objective, milestone, current editing scope, dependencies,
shared interface/version and acceptance commands. Use the
[component boundaries](docs/architecture.md#component-ownership) to preserve
interfaces, not to assign permanent contributor roles. Coordinate overlapping
edits to root CMake files, shared headers, architecture and status docs. Temporary
worker scopes prevent collisions; separate worktrees are useful when independent
changes need isolated builds.

For sweeping changes, open reviewable checkpoints that keep `main` buildable.
Separate mechanical moves from behavior changes where practical. Settle the
smallest shared contract before dependent implementations diverge. Record
architecture decisions in the existing architecture document and implementation
handoffs in the PR; do not create another roadmap. A service/transport or terminal
contract change must state how existing clients, live processes and saved fixtures
behave across the transition. Version incompatible wire changes, test rejection or
migration, and use a new private test endpoint rather than attaching a development
build to someone else's live session. GUI detach must continue to preserve the
service-owned child; focus and acknowledgement must never imply approval.

Each checkpoint hands off changed files, interface effects, exact commands and
results, source SHA, platform/tool versions, evidence paths and remaining limits.
The integration owner reviews the combined diff, reruns the accumulated checks
on the assembled head, and updates README status and architecture acceptance.
Individual branches passing tests do not establish integration acceptance. Use
one build owner per checkout/preset; shared `build/reports/<mode>/` receipts are
overwritten on rerun, so preserve relevant logs before another run.

## Contribution and PR procedure

1. Inspect `git status --short` and choose one coherent change within the current
   milestone. Work on a branch from `main`; preserve unrelated work. Discuss
   architectural changes before expanding implementation, and record decisions
   in `docs/architecture.md` rather than adding another plan.
2. Implement the smallest reviewable result. Keep refactors separate when they
   can be reviewed independently; include necessary tests and documentation with
   the behavior they describe. Use the checks below for the affected scope.
3. Open a ready-for-review PR against `main` (unless a draft was requested) using the
   [PR template](.github/PULL_REQUEST_TEMPLATE.md). Write a concrete title describing
   the result. Explain the problem and resulting behavior, relevant design choices,
   exact checks and outcomes, and remaining risks. Link an issue when one exists;
   an issue, special branch prefix or conventional-commit prefix is not required.
4. Identify the applicable configured reviewers for the actual PR head; invoke
   comment-triggered services and verify delivery. Installation alone does not
   establish review coverage. Collect complete thread-aware feedback and address findings together within
   the review budget; slow optional services must not hold useful work open.
   Fix valid findings, rerun affected checks, reply with evidence and resolve the
   threads. Explain duplicate, stale or inapplicable findings rather than ignoring
   them. Refresh review state after changing the head.
5. Record each service as completed, completed with findings, pending, or
   skipped/unavailable. Explicit auth, quota or provider failures are unavailable.
   For rate/quota limits, stop calls until reset (next local day if none is supplied),
   then retry once only if needed. For silence, use a bounded wait of up to ten minutes, one retrigger and a
   sixty-second acknowledgement check unless the maintainer requests a shorter
   budget. Record a shortened wait as skipped, without claiming unavailability. Optional unavailability is neither approval nor a merge gate.
6. Before merge, confirm checks and reviews apply to the current head, resolve
   valid findings and conflicts, and satisfy required CI, repository protection
   and human approvals. Report unavailable reviewers in the handoff. Templates
   standardize submissions; they do not enforce these gates in GitHub.

Keep the PR description current as scope changes. Use `Change`, `Validation` and
`Risks and follow-up`; a small change needs only a sentence or two plus its checks.
For visual changes include useful screenshots/captures; for performance changes
include matched measurements or state explicitly why a claim is still unproven.

Keep Markdown concentrated in the README, architecture, this guide, agent
instructions and the Codex investigation. Update those documents in place.
Keep reproducible tooling in `scripts/` or `tools/`, sanitized receipts in
`evidence/`, raw builds/logs in ignored `build/`, and local session state in ignored
`runtime/`. Never include credentials or private transcripts. Preserve `.sindexer/`
in `.gitignore` and the relative `CLAUDE.md` symlink to `AGENTS.md`.

### Fast iteration

Start with the changed behavior and reuse verified dependency builds. Once the
relevant checks pass, move on; rerun broader checks only for a new change, failure
or unresolved concern. Do not rebuild the engine comparison for adapter-only work.
Skip optional slow checks or additional reviewer waits when they stop providing
useful evidence, and name what was skipped and why in the handoff. Required CI,
repository protection and unresolved correctness findings still govern merges.

### Long-running work

Keep slow work bounded and observable; a silent command is not necessarily stuck.
Operation timeouts and diagnostic checkpoints are not an overall project time
limit. Continue authorized implementation and verification through those
checkpoints; diagnose, resume or replace stalled operations without treating
elapsed time as a reason to declare incomplete work finished.

1. Before a substantial build, investigation or delegated task, state its scope,
   acceptance command, expected duration when known, and diagnostic checkpoint.
   Distinguish a cold dependency build from a cached run. Set a finite command
   timeout appropriate to the workload and retain logs under `build/`.
2. During interactive work, provide a concise update at least every sixty seconds:
   elapsed time, evidence of progress, remaining work and the next checkpoint.
   Use asynchronous commands so a long build does not prevent updates.
3. Diagnose when a task exceeds twice its estimate, or after ten minutes if no
   estimate exists. Inspect process/child state, CPU activity, log growth, output
   artifacts and provider status. Report whether it is progressing, waiting on a
   known dependency, or stalled; elapsed time alone does not prove a hang.
4. At that checkpoint, freeze scope. Continue a progressing task with a revised
   estimate and another bounded checkpoint. For a stalled task, preserve logs and
   partial work, stop only the owned process group, and verify children stopped
   before retrying or handing its files to another worker. Do not launch duplicate
   builds into the same directories.
5. Retry only after identifying a concrete change to the failing conditions.
   After two attempts fail for the same reason, stop repeating that approach,
   report the evidence and choose a smaller diagnostic or an alternative. Continue
   independent authorized work; do not quietly extend the milestone to chase a fix.
6. If required validation remains blocked, report the completed work, missing check,
   cause, evidence path and next action. Preserve a reviewable checkpoint and keep
   its status incomplete. Never describe a timeout, skipped check or unavailable
   reviewer as a pass. Rate limits still stop reviewer calls. A maintainer-directed shorter review
   budget takes precedence over optional waiting; record it as skipped, not passed.

## Setup

On macOS, run `brew bundle --file Brewfile`. Xcode or its command-line tools must
provide an SDK. Python 3.11+ runs the verification scripts without extra packages.
`just` is an optional command shortcut. Python tooling changes also require Ruff;
install these optional tools with `brew install just ruff` when needed.

The runner locates Homebrew LLVM without editing shell PATH or replacing Apple's
compiler. Set `LAPIS_LLVM_BIN` to another LLVM installation's `bin` directory to
select it explicitly. Keep clang++, clang-tidy, clang-format, and clangd from
the same LLVM installation. A selected installation must provide all four
executables; the runner refuses to fill gaps from PATH.

On Linux, install LLVM, Cppcheck, CMake, Ninja, a C++ standard library, and Python
through the platform's package manager; optionally install ccache. Headless engine
build/replay has been exercised in an Ubuntu 24.04 ARM64 container. Session,
desktop and GPU behavior remain unqualified there. CMake accepts macOS and Linux
targets; sanitizer presets require a Clang/GCC toolchain.

### Terminal dependency

The normal build includes the production terminal adapter. Bootstrap the pinned
Ghostty library once, then run the checks; the launcher finds the resulting
prefix by itself, so nothing needs exporting:

```sh
python3 scripts/lapis.py bootstrap   # builds Ghostty and reports readiness
python3 scripts/lapis.py check
```

`bootstrap` runs the same probe and prints the prefix it produced. To select a
different prefix explicitly, set `LAPIS_GHOSTTY_PREFIX` before running any
launcher command, or pass `-DLAPIS_GHOSTTY_PREFIX=...` to CMake directly.

CMake accepts the same setting as `-DLAPIS_GHOSTTY_PREFIX=...` and retains it in
its cache. It checks the adjacent successful probe receipt against the source
manifest; this is build provenance, not a cryptographic attestation of the local
archive. Reuse that prefix across development and sanitizer builds. Ghostty runs
in Zig ReleaseSafe mode; ASan/UBSan instruments the C++ adapter and tests.
No network download occurs during CMake configuration. Other bootstrap hosts
remain unqualified; the existing pins cover macOS and Linux ARM64.

### Desktop preview

Install the macOS Brewfile dependencies and bootstrap Ghostty above. The desktop
requires **Qt 6.11.2 exactly**, Vulkan headers/loader (exercised at 1.4.357.0), and
MoltenVK (1.4.2). Homebrew formulae move; a newer Qt installation deliberately
fails the CMake version check until that version is evaluated. The headless build
does not depend on Qt. Check the installed version with `qmake -query QT_VERSION`
(expected: `6.11.2`) before configuring. `just desktop` uses an optimized build with symbols;
`just run` opens `build/desktop/apps/desktop/lapis_desktop.app`. The app locates its
service using the build path; copying the bundle alone is not a portable install.

For a repeatable visual/input check:

```sh
python3 scripts/lapis.py smoke    # writes build/window.png

# Equivalent direct invocation, including an explicit new session:
# build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop \
#   --new-session --smoke-input --capture "$PWD/build/window.png"
```

This opens a real window, clears a harmless partial command with Control-U, sends
a unique `printf` marker and `stty size` through Qt key routing (including
Alt-B/Alt-D shell word editing), waits for the
service snapshot, captures the window, and exits. `--compact` tests 980×700 logical
pixels. Record the capture device pixel ratio and distinguish physical image
coordinates from Qt logical coordinates. The shell survives the capture process.
Keep captures private under `build/` unless reviewed for terminal content. This
is functional acceptance, not an input-latency benchmark. Serialize every GUI
check across worktrees; an offscreen or headless result never counts as
native-display qualification. The two CTest GUI suites share `qt_gui` as a
resource lock within one invocation; separate invocations still need explicit
coordination. For renderer changes, run `terminal-render` against the previous
renderer and the proposed change: a valid baseline failure must identify a
pixel/layout assertion after window and snapshot preconditions pass.

Window tests open on the laptop panel by default. `--screen <text>` selects the
QScreen whose Qt name contains that text, and `LAPIS_SCREEN` supplies a default
for direct app invocations. The launcher sets `LAPIS_SCREEN=built-in` and prints
the chosen screen and geometry, so a capture cannot silently land on an external
display. Pass `--screen ""` or `LAPIS_SCREEN=` to keep platform placement.

### Keybindings and layout

`lapis.json` at the project root holds the keybindings, the layout, and the
category names. Edit it and press Ctrl-R in the running window, or choose
"Reload keybindings" from the Preview tools menu; no rebuild is needed. The
defaults are:

| Action | Default | Purpose |
| --- | --- | --- |
| Quit | Ctrl+Q | Exit lapis |
| Detach window | Ctrl+W | Hide the window and leave sessions running |
| Next / previous category | Ctrl+Tab / Ctrl+Shift+Tab | Move between categories |
| Category 1-4 | Ctrl+1 to Ctrl+4 | Jump straight to a category |
| Next / previous window | Ctrl+Shift+] / Ctrl+Shift+[ | Move through sessions |
| Focus left / right | Ctrl+Left / Ctrl+Right | Step one session |
| Cycle layout | Ctrl+L | Switch focus and blocks layouts |
| Appearance | Ctrl+, / Meta+, (Command-comma on macOS) | Open theme, layout and density settings |
| Reload config | Ctrl+R | Re-read `lapis.json` |

Each action takes a string or a list of strings, so several chords can share one
action. Qt names the macOS Command key `Meta`; write `Meta+` (or `Ctrl+` for
portability) rather than `Cmd+`. A missing or malformed file falls back to the
built-in defaults and reports the problem instead of failing to start, and the
window logs the path it read.

Appearance settings offer four layouts: `focus` keeps one large pane and a
preview strip, `columns` places previews beside the pane, `blocks` uses a wrapping
grid, and `stack` shows the selected session alone. Six themes and three card
densities change the window chrome; terminal cell colors remain session-owned.
Appearance choices persist atomically in `lapis.json`, preserving shortcut strings
and other JSON values; malformed files remain untouched and show a diagnostic. Ctrl-L temporarily toggles focus/blocks at runtime;
the Appearance dialog exposes all four choices. The session cards still include
fixtures; navigation does not create additional live service sessions.

Command-Left and Command-Right inside the terminal move to the start and end of
the line, matching macOS editing. The terminal translates them to the Ctrl-A and
Ctrl-E sequences that interactive shells already implement, so line editing stays
with the shell rather than being reimplemented in lapis. Every other Command
combination stays available to the window.

The macOS app sets `QT_MTL_NO_TRANSACTION=1` before Qt initialization. On this
Qt/MoltenVK combination, the default transaction layer emitted five-second display
lock warnings; the plain CAMetalLayer path passed threaded Vulkan capture without
them. Recheck this version-specific workaround on Qt upgrades. The app verifies
Vulkan selection at runtime instead of accepting a silent fallback.

Qt Core/Gui/Network/Qml/Quick/QuickControls2 are dynamically linked under the open
source LGPLv3 option; commercial licensing is an alternative. Redistribution must
retain notices, meet the source/relinking requirements and audit the actual bundled
modules/dependencies. MoltenVK and the Vulkan headers/loader formulae report Apache-2.0;
verify their complete bundled notices before redistribution. Qt, Vulkan and Ghostty packaging/SBOM provenance are unfinished;
no distributable binary is published by this checkpoint.

Use the [desktop sanitizer procedure](#desktop-sanitizers) for PTY, transport,
renderer and UI lifecycle changes. Keep ASan and TSan separate. Vendor Qt/MoltenVK/Ghostty libraries are not instrumented
by these C++ presets; a passing test is not coverage of those implementations.

### CLI integration qualification

The shell launch specification is `$SHELL -i` in the checkout. If `SHELL` is unset,
lapis uses the account login shell; `/bin/sh -i` is the final fallback.
First use requires **Session → Start new session** or `--new-session`.
Subsequent launches without that flag reconnect using the saved identity; they
never create a replacement process. `--discover` explicitly adopts an existing
matching service. Both flags are mutually exclusive and unavailable in fixture mode.
Use `--socket PATH --cwd DIRECTORY -- PROGRAM ARG...` for an explicit launch.
Arguments are literal; use `--` to separate lapis options from the child's options.
Repeat the same launch without `--new-session` to reattach; changing executable/argv/cwd on an occupied
endpoint is rejected. A socket parent must be owned by you and private (0700).
Existing directories/files are not repurposed. Logs are written beside each
socket as `<socket>.log`. The default endpoint is `runtime/desktop-v4.sock`;
old v1/v2/v3 sessions stay untouched. The `<socket>.session` hint is a private 0600
regular file containing session ID, epoch and launch fingerprint. A missing or
corrupt hint disables implicit attachment. Explicit discovery can replace corrupt
contents in a safe file; unsafe modes, symlinks or hardlinks require repair first.
The service identity is always checked live. After the service ends, choose Start
new session explicitly; an occupied endpoint will be rejected, preserving its child.

Run `just cli-check` for isolated service and GUI fixtures. For the optional
installed Codex test:

```sh
python3 scripts/check_cli_launch.py --desktop --codex \
  --output build/cli-launch-check/codex.json
```

The optional test uses the current Codex configuration with no extra launch
flags (Codex 0.154.0 removed the older `--no-daemon`), types
only an unsubmitted test marker, exercises navigation/paste/resize, captures normal
and compact windows, reattaches to the same child, clears the draft with Ctrl-C,
and quits from the empty composer with Ctrl-D.
It does not send Enter or start a model turn. Service IPC drives those Codex inputs;
the separate shell smoke drives Qt key events. No physical-key, IME or attention
claim follows. Logs/captures stay in a unique directory beside the receipt; runtime
sockets use a fresh private directory. The [launch receipt](evidence/cli-launch.json) and
[reconnect receipt](evidence/session-reconnect.json) delimit the exercised scope.

Read-only Codex inventory can be repeated now, without starting a model turn:

```sh
python3 scripts/probe_codex.py --output build/reports/codex-probe.json
codex --help
codex app-server --help
codex features list
```

The script hashes the executable selected from PATH and records schema and live
initialization/list results. Help and feature output advertise interfaces; they
do not verify TUI behavior, hook dispatch, permissions or shared-server delivery.
Record selected CLI options alongside the receipt when qualifying a route.

For additional CLI acceptance runs, use a dedicated service/socket and test
directory. Record the lapis revision, Codex
hash, working directory, launch arguments and whether the backend is local to the
TUI or shared. Exercise text/navigation, literal paste, alternate-screen behavior,
resize, interrupt, GUI detach with continuing output, reattach to the same child
and screen, and explicit exit. Distinguish service death from GUI detachment.
Keep raw output/captures private under `build/` and socket/state under `runtime/`.
Do not use `--smoke-input` while Codex is running: that probe sends shell commands.

The no-prompt check clears its draft with Ctrl-C, verifies the cleared screen,
then uses the empty-composer Ctrl-D quit shortcut. It submits no model prompt.

Start with a no-prompt TUI check. A later real input/approval fixture needs a
declared provider/model, bounded turn deadline and explicit test approval policy.
Check the effective runtime route and actual request/response/continuation;
neither a TUI screenshot nor a schema export passes that acceptance. Keep hooks
scoped to the fixture and leave the user's shared daemon/configuration intact.

Use `just check` and `just desktop` for changes to the launch path. The existing
focused cases can also be rerun with:

```sh
ctest --test-dir build/desktop -R '^(launch-spec|pty-process|local-protocol)$' --output-on-failure
```

Those CTest cases cover launch validation, the PTY primitive and framing.
`check_cli_launch.py` covers the full detached service, and `--codex` adds the
installed TUI.
Run lifetime/parsing and lifecycle cases in separately configured desktop-enabled
ASan/UBSan and TSan builds as described above; check `ctest -N` in each build to
confirm the intended cases exist. A default headless sanitizer pass is insufficient.
Point `check_cli_launch.py --build-dir` at each instrumented desktop build to test
its actual service. Run GUI checks one at a time: focus changes from another test
can pause the attention cue and invalidate a timing assertion.

The PR #2 repair passes all seven desktop CTest cases under ASan/UBSan and
TSan, with `QSG_RENDER_LOOP=threaded`, plus the instrumented service harness.
The earlier `TerminalSurface` construction/render reports are resolved by an
explicit mutex handoff of owned immutable render state. No suppression was added;
the render callback no longer reads a GUI-owned `QPointer`, snapshot or preedit.
The [repair receipt](evidence/pr2-review.json) records the tested source and scope.
Vendor Qt/MoltenVK/Ghostty remain uninstrumented; this does not establish race
freedom inside those libraries or physical input-to-presentation performance.

### UI tuning and debugging

After `just desktop`, use `just ui` for an isolated synthetic workspace. It never
constructs a live service connection or sends shell input. Edit
`apps/desktop/qml/Main.qml`, then select **Preview tools → Reload interface**; no C++
rebuild is needed. A load error retains the working view, marks the preview control
and exposes diagnostics in its tooltip and stderr. Normal launches use bundled
QML; rebuild with `just desktop` to include edits there.

The menu describes each effect beside its action. **Show one alert** highlights
the third terminal; **Show two alerts** highlights the second and third.
**Clear one alert** clears the third terminal, while **Clear all alerts** removes
every alert. New alerts pulse twice and remain marked until cleared. **Repeat the
same alert** verifies that an existing alert does not pulse again. Clear it first
to replay the pulse. **Disable animations** uses steady markers; the macOS Reduce
Motion setting also enables it, sampled at startup and app activation. These are
synthetic events, with no agent response or approval attached. Manual navigation
and configurable shortcuts work over these fixtures and the single live session;
automatic attention-driven navigation remains planned in the
[architecture](docs/architecture.md#ui-refinement-checkpoint).

Run `just ui-check` for five captures and three expected-failure cases. Artifacts
and a receipt go under `build/ui-preview-check/`. For an individual capture:

```sh
build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop \
  --ui-preview --scenario two --capture "$PWD/build/two.png" \
  --trace "$PWD/build/two.json" --capture-delay 2000
```

Use `--compact` for 980×700 logical pixels, `--reduced-motion` for a steady cue,
and a shorter capture delay to sample the pulse. The normal requested size is
1400×960; the window manager may constrain it. Captures wait for a rendered frame
and fail within 15 seconds plus the configured capture delay. Traces include focus ownership, pane/card geometry,
request state and bounded GUI-thread `frameSwapped` observations. Signal delivery
includes scheduling overhead: these are neither physical presentation nor input
latency measurements. The short post-pulse idle observation is not a CPU/GPU load
benchmark. Capture timing targets remain provisional.

`just ui-debug` opens LLDB with the isolated source-QML fixture. For example:

```text
breakpoint set -n lapis::desktop::UiPreview::load
run
bt
continue
```

Launch/break/inspect/resume and separate `xcrun lldb -p <preview-pid>` attach,
`bt`, `detach` were exercised on this Mac. Use the actual preview PID; the service
is a separate process. Capture native stacks and Qt diagnostics separately from
terminal content. `just desktop` already produces symbols. Preview reload/attention
cases also run under the desktop-enabled ASan/UBSan preset; vendor libraries remain
uninstrumented. See the [receipt](evidence/ui-preview.json) for exercised scope.

The older `--smoke-input --capture` command above attaches to the real one-client
service and types into its shell. Use it only with a dedicated test session;
`--ui-preview` deliberately rejects that combination. Debugging starts from a
reproducible symptom; a screenshot alone does not establish an application defect.

## Checks

Run from the repository root:

| Command | What it checks |
| --- | --- |
| `just check` | Compiler warnings as errors, CTest, format, clang-tidy, and Cppcheck |
| `just asan` | CTest with AddressSanitizer and UndefinedBehaviorSanitizer |
| `just tsan` | CTest with ThreadSanitizer in a separate build |
| `just verify-tools` | Known-bad fixtures must produce specific failure diagnostics |
| `just format` | Apply C++ formatting |
| `just profile` | Optimized build with debug symbols and CTest |
| `just desktop` | Optimized desktop/service build, PTY/transport/UI cases and static checks |
| `just run` | Open the previously built live shell window |
| `just ui` / `just ui-debug` | Isolated source-QML fixture, directly or in LLDB |
| `just ui-check` | Bounded isolated captures, attention state and expected failures |
| `just cli-check` | Isolated live service/CLI and shell GUI acceptance |
| `just native-input` | Automated macOS keyboard/clipboard and real Japanese IME through the PTY |

Required checks accumulate when a change touches multiple areas:

| Change | Required validation |
| --- | --- |
| C++ code | `just check` plus meaningful behavioral cases; `just desktop` for desktop/service Qt code |
| Memory/lifetime, parsing or process resources | Relevant cases through `just asan` |
| Threading, queues or session lifecycle | Relevant cases through `just tsan`, separately from ASan |
| PTY, local transport or CLI launch | `just desktop`, `just cli-check`, and desktop-enabled ASan/TSan as applicable below |
| QML, rendering or desktop input | `just desktop` and `just ui-check`; live input changes also need `just cli-check` and `just native-input` on the qualified Mac |
| Build/test tooling | `just verify-tools` plus affected positive check/build paths |
| Disk history | `python3 scripts/check_history.py --disk-full` on macOS, plus desktop-enabled ASan/TSan; the disk-full fixture creates and removes its own 32 MiB disk image |
| Python tooling | `ruff check --isolated scripts` and `ruff format --isolated --check scripts`, plus relevant runtime probes |
| Documentation or symlinks only | Verify paths, links and instruction consistency; no unrelated C++ rebuild |

Without `just`, use `python3 scripts/check_cpp.py dev`, replacing `dev` with
`asan`, `tsan`, `profile`, `desktop`, or `format` as appropriate. The detector check is
`python3 scripts/verify_cpp_tools.py`.

Builds use Ninja and ccache when available. Analysis runs in parallel, up to
eight workers by default; use `--jobs N` on `check_cpp.py` to adjust it. Each
invocation records diagnostics, tool versions, exit codes, and durations under
`build/reports/<mode>/`. Static analysis runs even when compilation is cached. Desktop analysis uses the
Qt Cppcheck library and excludes generated MOC/resource files from source analysis;
the compiler still builds those files with project warnings enabled.

Use these runner commands to select LLVM, the macOS SDK, and ccache together;
raw CMake presets do not perform that tool discovery. The `dev` and `profile`
presets explicitly disable sanitizers, including when reusing a build directory
that previously had instrumentation enabled. ASan/UBSan and TSan remain separate.

The development preset generates `build/dev/compile_commands.json`. Point your
editor's clangd extension at the same LLVM installation. `.clangd` supplies the
database location and limits interactive analysis to fast checks. The full batch
checks still run through `just check`.

### Test suites and failure triage

CTest registers the following suites in the current build. Confirm the inventory
with `ctest --test-dir build/desktop -N`; an empty or accidentally headless build
is not a desktop test pass. These are suites, not counts of individual assertions.

| CTest name | Build | Behavior |
| --- | --- | --- |
| `toolchain-smoke` | Headless and desktop | Compiled toolchain baseline |
| `attention-state` | Headless and desktop | Typed requests, exact retirement, stale decisions, reconciliation watermarks, bounds, aging and snooze/cooldown |
| `session-platform-ownership` | Headless and desktop | POSIX descriptor ownership and moves |
| `terminal-behavior` | Headless and desktop | Ghostty parsing, snapshots, history, resize and mode-aware input |
| `launch-spec` | Desktop-enabled | Literal launch validation and private endpoint rules |
| `local-protocol` | Desktop-enabled | v4 identity/timing/history envelopes, framing, bounds, snapshots and invalid messages |
| `session-descriptor` | Desktop-enabled | Private identity hint, atomic replacement, corruption and unsafe-file rejection |
| `live-connection` | Desktop-enabled | Screen-before-input, explicit reconnect/discovery, lost/stale snapshots and legacy-server rejection |
| `pty-process` | Desktop-enabled | Real launch/I/O/resize, exit, failure and process cleanup |
| `keymap` | Desktop-enabled | Configuration defaults, appearance choices, persistence and invalid input |
| `ui-preview` | Desktop-enabled | Qt reload, screen selection, attention, input and render lifecycle |
| `appearance-input` | Desktop-enabled, native GUI | Configured settings shortcut, modal focus, all theme/layout/density controls, persistence and shortcut reload |
| `history-store` | Desktop-enabled | Styled page round trips, per-session/global quotas, corruption, interrupted-write cleanup and file-size write failure recovery |
| `terminal-input` | Desktop-enabled, native GUI | Qt composition commit/cancel, replacement rejection, paste and focus/document/history/disconnect ownership |
| `terminal-render` | Desktop-enabled | Real Qt Vulkan pixel regressions for cell background grids, wide/combining characters, fallback/RTL text, styles/decorations, actual Ghostty resize, cursor placement and clearing |

`just desktop` runs these fifteen suites plus static checks. The separate Python
GUI harness checks five preview captures and three expected failures. The CLI
harness checks detached service behavior, attachment generations, fragmented
handshakes, synchronization timeout, stale controls, bounded queue failure and
replacement identities; `--desktop` adds Qt-to-shell input and
captures, and optional `--codex` adds the installed no-prompt TUI acceptance.
A screenshot, a headless suite and a real agent approval round trip prove different
things. See [attention qualification](#codex-attention-qualification) for isolated
live round trips and the remaining service/desktop integration gap.

After a failure, retain `build/reports/<mode>/receipt.json`, the named check log,
and CTest's `build/<build-name>/Testing/Temporary/LastTest.log`. Fix the cause,
rerun the failing suite, then rerun the relevant complete check on the final diff.
For a capture watchdog failure, inspect its log and window activation/frame
prerequisites; stop competing GUI checks and reproduce that case in isolation.
Preserve the original failure even if a clean run subsequently passes.
For process-group cleanup, poll for `ESRCH`; a temporary macOS `EPERM` is
not proof that the group disappeared. The test retains PID, errno and elapsed
time in its failure diagnostic.
Do not weaken assertions, add broad suppressions or count an expected-failure
probe as a pass unless its expected diagnostic was observed. Raw CMake/CTest
commands below do not run format, clang-tidy or Cppcheck; `just desktop` supplies
those checks. Save custom build/test output under `build/` and include exact
commands with any sanitized receipt committed to `evidence/`.

### Codex attention qualification

The attention core is a standalone C++20 library, not yet wired into the session
service. Run `just check`, `just asan` and `just tsan` for its normal/static and
separate sanitizer checks. `attention-state` is registered in all build modes;
synthetic reducer events do not establish delivery from Codex.

The original `scripts/probe_codex.py` retains its no-turn behavior. The separate
shared-server probe also sends no model prompt:

```sh
python3 scripts/probe_codex_attention.py --output build/codex-shared-server.json
python3 -m unittest discover -s scripts/tests -v
ruff check --isolated scripts
ruff format --isolated --check scripts
```

It uses a private Codex home and server, tests WebSocket-over-Unix transport, and
reports capability limits independently of transport success. A fresh thread may
not yet have resumable history; successful initialization or live metadata reads
are not proof of pending-request reconciliation.

For **opt-in live model turns**, the current fixture requires the local CCR service
at `127.0.0.1:3456` with the `zai,glm-5.3` route. There is no fallback model:

```sh
python3 scripts/check_codex_attention.py --live-glm --with-tui \
  --output build/codex-attention-live.json
```

This submits one input fixture and one command-approval fixture to a disposable
server, reconnects an observer, and answers exact pending request IDs. The approval
fixture permits only `python3 -c 'print(123456789)'`, optionally wrapped by a known
shell; unexpected commands fail without approval. The input fixture chooses Blue.
`--case input` or `--case approval` limits a diagnostic run. Each RPC/request has
a deadline; the runner does not retry model turns automatically. Failure produces
a nonzero exit and JSON receipt rather than synthetic success.

`--with-tui` starts the ordinary Codex TUI in a private PTY, checks that it displays
the real question/approval, and keeps it attached while the observer responds.
The only automated TUI confirmation is trust for the fixture's empty directory,
stored in its disposable Codex home. No global hooks/configuration, user's daemon,
clipboard or desktop focus is changed. This is not the lapis GPU/Qt input path;
use the existing CLI and native-input procedures when integrating that path.

Receipts distinguish configured/provider-reported model identity, request replay,
explicit response, matching resolution, completed turns and TUI participation.
Corroborate upstream routing from CCR runtime evidence; configuration alone is not
provider proof. Raw fixture data stays under `build/` and disposable runtime state
is removed. The runner additionally tests a pending request and a request resolved
while a second observer is disconnected, capturing replay/resolution events before
a subsequent same-thread `thread/read` reply. This boundary depends on the tested
server's exclusive method serialization, so source inspection plus a new live run
is required for another binary hash. It qualifies only the two exercised blocking
request kinds. See the [Milestone 2 plan](docs/architecture.md#milestone-2-attention-and-codex-plan).

### History and input qualification

Build the current desktop first. Run these checks serially with other GUI work:

```sh
python3 scripts/check_history.py --disk-full
ctest --test-dir build/desktop -R 'terminal-input|live-connection' --output-on-failure
build/desktop/apps/desktop/lapis_terminal_latency_probe \
  --native --samples 100 --output build/terminal-latency.json
```

`check_history.py` runs a controlled Python child under the real service. It covers
1,500 output rows with a one-row viewport, quota eviction, older/newer paging,
resize, same-child reattachment and corrupt-record recovery. `--disk-full` is
macOS-only and fills a newly created 32 MiB HFS+ image until the OS returns ENOSPC;
it checks that live input survives and browsing recovers after freeing space. It
never formats an existing device. Omit the flag for the portable service cases.
For sanitizer binaries, pass `--build-dir build/desktop-asan` or
`build/desktop-tsan`; keep address and thread instrumentation separate.

The native timing command requires a logged-in desktop and `cliclick` with
Accessibility permission. It briefly activates its own controlled window and
sends Return through macOS. It starts and cleans up its own service/child. Run it
without competing GUI checks or builds for a measurement receipt; omit `--native`
for a separately labeled synthetic Qt baseline. Five warmups precede the requested
samples. Record revision, binary hashes, host, tool versions, observed refresh rate
and workload alongside JSON. Results include p50/p95/p99 stage timings, samples
over one refresh interval, separate desktop/service idle CPU, resident memory and
idle submitted frames and 20 retained history-to-live returns (with at least 30 input
samples). The JSON `transport` interval starts before service snapshot extraction, so it
includes snapshot building/encoding, IPC and GUI decoding; it is not pure socket
latency. Frame submission is not pixel presentation, and idle frame
count is not a hardware GPU-utilization counter. These timings remain observations,
not pass/fail performance thresholds. This probe does not measure cross-session
switches or real agent turns.

Native software input acceptance is automated on macOS; no physical typing is
required. After `just desktop`, run this separately from every other GUI test:

```sh
just native-input
# Equivalent command, also usable with desktop-asan or desktop-tsan builds:
build/desktop/apps/desktop/lapis_native_input_probe --output build/native-input.json
```

The opt-in probe needs macOS 14 or later, a logged-in graphical session, installed
Apple US/Japanese input sources, and macOS Accessibility event-posting permission
for the invoking test environment. It fails with a diagnostic if permission is
absent; it does not prompt or change that permission. It is built on macOS but is
not registered in CTest because headless CI lacks these desktop prerequisites.
Run it on the qualified Mac for changes to native input or composition handling.
Avoid interacting with the keyboard/clipboard during this short exclusive test.

The built-in Japanese input method editor (IME) provides a reproducible test of
provisional composition and explicit commit/cancel: for example, Roman `a`
produces provisional `あ`. Ordinary English typing does not exercise that path.
This fixture does not change lapis's UI language or imply all input methods have
been qualified. Printable/Control/Option keys and paste use the US layout.

CoreGraphics posts keys to the probe's own process. AppKit, the actual Apple
Japanese IME, Qt and the real service-owned PTY handle them. The probe checks exact
received bytes for printable/Control/Option keys and Command-V multiline Unicode
bracketed paste; observes native preedit and commit; checks cancellation and fresh
composition after history, document detach, window focus and actual attachment
replacement/reconnect; and verifies commit plus the candidate anchor after resize.
Each native key edge waits for AppKit delivery before the next edge is posted;
keys are never resent after a deadline. `LAPIS_NATIVE_TRACE=1` logs the probe's
AppKit key and Qt key/composition events for diagnosis. A delivery deadline is
distinct from a PTY-byte mismatch. Composition cases await an observed preedit
before testing commit or invalidation, and cancellation awaits an empty preedit.
The Qt input fixture also awaits expected service bytes instead of assuming
asynchronous transport completes in 50 ms. Failed native cases preserve preceding results and
verify clipboard/input-source restoration after the fixture is destroyed.
These checks need exclusive desktop input: foreground interference can invalidate
a run. Intermittent failures must be retained and investigated, not hidden by
automatic retries.
On native focus loss, Apple's IME may commit to the original terminal; the probe
asserts that the new terminal receives no composition bytes. This does not add a
multi-session product UI. The candidate anchor check verifies the rectangle
provided to the IME, not the visual pixels of Apple's candidate window.

It temporarily enables US and Japanese input sources, then restores and verifies
the selected source, enabled-source inventory and clipboard MIME data. Only
controlled fixture bytes enter the JSON receipt; user clipboard contents are not
logged. Private temporary services and files are cleaned up. Preserve the JSON
and stderr log on failure. Qt-injected composition tests remain useful separate
coverage. Physical keyboard hardware and key-to-photon measurements are outside
Milestone 1 software acceptance. Selection/copy from terminal cells and a
screen-reader terminal tree remain unsupported.

History is under `runtime/history` by default. Before starting a service, set
`LAPIS_HISTORY_ROOT` to an absolute private directory and optionally set
`LAPIS_HISTORY_SESSION_BYTES` / `LAPIS_HISTORY_GLOBAL_BYTES` (positive bytes,
session <= global <= 4 GiB). Defaults are 64 MiB / 256 MiB, with a 4,096-page global
cap. The global quota is shared by services using that root, not every arbitrary
root on the machine. Pages preserve their recorded geometry; only in-memory
engine history reflows on resize. History controls never resize or send input to
the child. Return to Live restores its newest retained screen and requested size.

A storage failure pauses recording and reports a gap when history is requested;
live I/O continues within its memory bound. Free space or repair the configured
storage, then use Older to retry. A damaged page is rejected, never rendered as
valid history. Retire a damaged archive directory only after its owning service
has ended; archiving is terminal content, so retain it only as long as needed.
The store removes only its known abandoned `.pending` write under its root lock;
it leaves unknown files alone. Page-byte quotas exclude fixed metadata and bounded
atomic-write overhead. Normal exit and direct service error shutdown allow up to three seconds to drain
queued pages; a forced
service kill can lose its queued tail. Stored pages are not process recovery.

### Desktop sanitizers

The default `just asan` and `just tsan` builds are headless. For Qt/PTY service,
renderer and desktop lifecycle coverage, use separate desktop-enabled directories.
On macOS, with the verified `LAPIS_GHOSTTY_PREFIX` exported, configure and run ASan:

```sh
export LAPIS_LLVM_BIN="${LAPIS_LLVM_BIN:-$(brew --prefix llvm)/bin}"
cmake --preset asan -B build/desktop-asan \
  -DLAPIS_BUILD_DESKTOP=ON \
  -DCMAKE_CXX_COMPILER="$LAPIS_LLVM_BIN/clang++" \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)" \
  -DLAPIS_GHOSTTY_PREFIX="$LAPIS_GHOSTTY_PREFIX"
cmake --build build/desktop-asan --parallel 8
ctest --test-dir build/desktop-asan -N
QSG_RENDER_LOOP=threaded ctest --test-dir build/desktop-asan \
  --output-on-failure --no-tests=error
python3 scripts/check_cli_launch.py --build-dir build/desktop-asan \
  --output build/reports/desktop-asan/cli.json
```

Repeat those configure/build/test/harness commands with preset `tsan` and all
`desktop-asan` paths changed to `desktop-tsan`. Do not combine instrumentation or
use `ctest --preset asan` for the custom directory: that preset targets
`build/asan`. Each desktop-enabled directory must list all fifteen suites above.
Use the same LLVM installation for normal and instrumented builds. Ccache is
optional (`-DCMAKE_CXX_COMPILER_LAUNCHER=...`); raw CMake does not discover it.
Reduce `--parallel` for host resource limits. The CLI command above runs service
fixtures against the instrumented executable; the CTest UI suite exercises the
threaded renderer. Add `--desktop` to the CLI harness when instrumented live GUI
input is needed. Keep every GUI run serial across all builds.

These commands qualify macOS only. On a Linux qualification host, select its LLVM
compiler and omit the macOS SDK option, then record actual results and dependencies.
Vendor Qt/MoltenVK/Ghostty remain uninstrumented. Do not reuse sanitizer timings
as release performance measurements.

### What the checks cover

- Compiler warnings include conversions, shadowing, and virtual-function mistakes.
- clang-tidy enables the Clang static analyzer, bug-prone patterns, performance,
  portability, concurrency checks, and selected modern C++ practices. Reported
  project diagnostics fail the check. System-header diagnostics are excluded.
- A cognitive-complexity threshold of 25 flags functions that need simplification.
- Cppcheck uses the real compilation database, including defines and include paths.
- ASan catches exercised memory-access faults; UBSan catches exercised undefined
  behavior. TSan detects exercised races and is built separately from ASan.

Sanitizers inspect executed paths. They do not prove race freedom or memory
safety, and ASan is not a complete leak checker on macOS. Use Instruments
Allocations/Leaks for retention and leak investigation.

New CMake targets must link `lapis_project_options` so warning and sanitizer
settings apply. Add meaningful CTest cases for ownership, parsing, event ordering,
and input routing as those components arrive. Header-only code needs a compiled
consumer. Current tests cover the toolchain, POSIX descriptor ownership with real pipes,
and 14 terminal adapter cases on macOS/Linux ARM64. The standalone attention
reducer is additionally exercised on macOS in normal and sanitizer builds. Desktop-enabled tests additionally cover PTY/transport and UI reload/attention
behavior; actual captures run through `just ui-check`. The original
[checkpoint receipt](evidence/cpp-verification.json) records its dated scope;
new local check receipts are under `build/reports/<mode>/`.

### Headless engine experiment

Run `python3 scripts/probe_terminal.py` for both pinned consumers, or select
`--engine ghostty` / `--engine contour`. The script downloads hash-verified source
archives and Zig, checks runner-owned source contents on reuse, and builds in ignored
`build/terminal-probe/reproduce/`. It currently bootstraps Zig on macOS ARM64 and
Linux ARM64; other architectures have not been qualified. Upstream sources stay
out of the normal C++ build, and no runtime package is installed system-wide.

Each invocation builds from a fresh verified source copy under `<engine>/runs/`.
Upstream code generation can modify that copy while the reusable archive tree
stays pristine. Copies and build outputs are retained for inspection.
Manifest destination names form part of each experiment's build layout; change
the manifest and its wrapper together and repeat the relevant replay.

Per-engine logs and JSON receipts live in `<engine>/runs/<invocation>/reports/`
under that build root; the runner prints the receipt path. Reruns retain earlier
evidence. `--mode asan` checks the C++ consumers; Ghostty's Zig library remains
ReleaseSafe, while Contour's compiled C++ graph is instrumented. A failed case
must keep a nonzero exit status and its receipt. Contour currently fails the
fragmented-UTF-8 case; the combined comparison therefore returns nonzero.

Use `--jobs N` for the total build-worker budget and `--build-root` for an isolated
run. An unowned, changed or unexpectedly populated source cache is rejected;
preserve it and choose a fresh build root rather than deleting someone else's work.
The exercised Ubuntu 24.04 toolchain uses Clang/libc++18:

```sh
python3 scripts/probe_terminal.py --engine ghostty \
  --cxx /usr/bin/clang++-18 --cc /usr/bin/clang-18 \
  --cxx-flags=-stdlib=libc++
```

For Contour use `--engine contour` and
`--cxx-flags='-stdlib=libc++ -fexperimental-library'`. Its C++23 setting is confined
to the experiment; lapis remains C++20.

Run `python3 -m unittest discover -s scripts/tests -v` for downloader and failure-
receipt cases after runner changes. `just check` compiles/analyzes the shared
replay contract; adapter changes also need their real standalone builds, replay
and clang-tidy using each experiment's compilation database. The
[saved experiment](evidence/terminal-engine-probe.json) records measured scope and
remaining gaps, including dependency audit coverage.

## Measuring responsiveness

Use **Instruments / `xctrace` as the primary macOS profiler**. It can inspect the
Metal work produced by MoltenVK as well as CPU scheduling. The live tool/hardware
inventory is in [performance-tooling.json](evidence/performance-tooling.json).
A tool's presence or a capture smoke is not a lapis rendering benchmark.

| Tool | Purpose |
| --- | --- |
| Instruments Game Performance / Metal System Trace | Frame pacing, GPU execution, drawable waits and CPU/GPU synchronization |
| Instruments Time Profiler plus signposts | Attribute input, parsing, IPC, layout and render preparation stalls |
| Instruments Allocations / Leaks | Cache footprint, allocation churn, retained objects and growth |
| Xcode Metal frame capture | Diagnose a selected frame's shaders, passes and resources; separate from timing acceptance |
| Qt Creator QML Profiler, once available | QML bindings, layout and scene-graph work; pair with the C++ profiler |
| Tracy, if adopted for shared instrumentation | CPU zones, queues, locks and Vulkan GPU timing across macOS/Linux |
| RenderDoc on Linux, once qualified | Inspect Vulkan frame contents and commands; not a latency acceptance tool |

QML/Tracy/RenderDoc are additional options, not installed project dependencies.
The inventory records which tools were exercised; recheck availability on the
benchmark host.

### Provisional targets and endpoints

Reference class: high-end M-series hardware, initially the M4 Max with 128 GB
unified memory and a nominal 120 Hz display mode. Benchmark at the actual
resolution, scale and refresh used by the app. Confirm presentation cadence;
nominal display mode alone does not prove sustained 120 Hz under variable refresh.
Minimum RAM/chip requirements remain unset until measured on representative hosts.

At 120 Hz, one interval R is 8.33 ms. The following are **provisional hypotheses**:

| Metric | Measured interval | Provisional target |
| --- | --- | --- |
| Warm session switch | Native navigation event receipt to first presented frame showing the selected session | p95 <= R; p99 <= 2R |
| Controlled local echo | Native key event receipt through a deterministic PTY echo to presentation of the matching glyph | p95 <= 2R; p99 <= 3R |
| Active animation | Presented frame intervals and deadline misses, with CPU/GPU work attributed separately | Sustain the configured 120 Hz workload; fit critical frame work within R with headroom |

These numbers are not measured guarantees or PR/release acceptance gates.
Keep them in this table only. Establish a baseline on the first real terminal
view and two-session switch workload, then revisit them. Promoting a number to an
acceptance threshold requires a reviewed change here recording the workload,
hardware, measurement endpoint and supporting receipt; until then it remains
provisional. A warm switch uses retained terminal state and already prepared
display resources; report what was warmed and test cold switches separately.

Report p50/p95/p99, worst stalls and missed presentation deadlines; average FPS
alone cannot establish responsiveness. Higher refresh displays require smaller
budgets when qualified. Measure native event
delivery, service/IPC queues and renderer work separately; do not include model
inference delay in the controlled local-echo benchmark.

Instrument native event receipt, input routing, PTY read, parse completion,
snapshot publication, scene-graph work, GPU submission and presentation with
session/frame correlation IDs. Use a common or calibrated monotonic timebase
across service and GUI. Submission or GPU completion is not on-screen presentation:
`vkQueuePresentKHR` return and Qt `frameSwapped` must not be silently labeled as
pixel visibility. Use verified presentation timestamps/trace events; if only a
proxy is available, label it and leave presentation acceptance unproven. Physical
key-to-photon claims additionally need an external camera/photodiode measurement.

### Capture procedure

1. Build the app with `just desktop` (optimized, symbols, sanitizers off).
   `just profile` covers the headless targets. UI frame observations are available;
   the latency probe correlates native-input, service and frame-submission markers;
   actual pixel-presentation timestamps remain unmeasured.
2. Warm the declared caches, then record a repeatable sequence: local typing,
   scroll/resize, session switches and output bursts. First qualify one terminal,
   then switching with two. Extend the same procedure to 32 sessions later.
3. Capture Game Performance/Metal System Trace for GPU and presentation behavior,
   Time Profiler for CPU causes, and Allocations separately for memory behavior.
   For an existing app PID, use the bounded template below. Raw traces remain
   under ignored `build/`; export sanitized timing distributions into `evidence/`.
4. Compare instrumented and uninstrumented runs to quantify capture overhead.
   Disable API validation and intrusive frame capture for acceptance timing; use
   them separately for correctness. If comparing Vulkan/MoltenVK with native
   Metal, keep source, workload, caches, display and power conditions matched.
5. Record source/tool versions, machine, OS, GPU/backend, resolution/scale/refresh,
   power/thermal conditions, competing agent/model workloads, session/output rates,
   run duration, sample counts and warm/cold state. Include a static idle baseline.

```sh
# Replace 12345 with the actual desktop PID and use a fresh path.
xcrun xctrace record --template 'Game Performance' --attach 12345 \
  --time-limit 30s --output build/lapis-game-performance.trace
```

For RAM, record service/GUI footprint, GPU allocations, cache limits/hit rates,
agent-process usage, memory pressure, compression/swap and growth over time.
CPU/GPU allocations share unified memory; report accounting scope rather than
adding overlapping totals. Generous warm caches are intentional. Check that
switching does not trigger avoidable disk reads, parsing replay, texture uploads
or shader compilation. Exercise cold start and controlled memory pressure
separately; verify cold-history/preview eviction preserves the active session and
keeps queues bounded. Spare RAM is not permission to consume idle CPU/GPU: static
scenes should sleep while live sessions continue consuming output.

## Dependency audit scope

Pin new runtime dependencies and record licenses/redistribution constraints.
Manifest or lockfile changes require the applicable ecosystem audit and a C++
inventory/SBOM scan. Use available ecosystem scanners and record their exact
scope/results; maintainers also run the shared `audit-dependencies.sh` where
installed. The engine experiment records a source/license inventory and direct
OSV commit queries because automatic package discovery does not recognize its
C++/Zig graph. A successful query with no matching advisory is not complete
coverage or a vulnerability clearance.
