# Contributing to lapis

This is the shared procedure for human and agent contributors: setup, checks,
profiling and pull requests. Start with [README.md](README.md) for implemented
behavior and [architecture](docs/architecture.md) for the current milestone.
Agents also follow [AGENTS.md](AGENTS.md).

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
   establish review coverage. After normal review processing reaches terminal
   states, collect complete thread-aware feedback and address findings together.
   Fix valid findings, rerun affected checks, reply with evidence and resolve the
   threads. Explain duplicate, stale or inapplicable findings rather than ignoring
   them. Refresh review state after changing the head.
5. Record each service as completed, completed with findings, pending, or
   skipped/unavailable. Explicit auth, quota or provider failures are unavailable.
   For rate/quota limits, stop calls until reset (next local day if none is supplied),
   then retry once only if needed. For silence, wait up to ten minutes, retrigger
   once and check for acknowledgement after sixty seconds before recording
   unavailability. Optional unavailability is neither approval nor a merge gate.
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

### Long-running work

Keep slow work bounded and observable; a silent command is not necessarily stuck.

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
   reviewer as a pass. Review-service waits and rate limits follow the PR procedure
   above; these general checkpoints do not override that policy.

## Setup

On macOS, run `brew bundle --file Brewfile`. Xcode or its command-line tools must
provide an SDK. Python 3.11+ runs the verification scripts without extra packages.
`just` is an optional command shortcut. Python tooling changes also require Ruff.

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

Required checks accumulate when a change touches multiple areas:

| Change | Required validation |
| --- | --- |
| C++ code | `just check` plus meaningful behavioral cases |
| Memory/lifetime, parsing or process resources | Relevant cases through `just asan` |
| Threading, queues or session lifecycle | Relevant cases through `just tsan`, separately from ASan |
| Build/test tooling | `just verify-tools` plus affected positive check/build paths |
| Python tooling | `ruff check --isolated scripts` and `ruff format --isolated --check scripts`, plus relevant runtime probes |
| Documentation or symlinks only | Verify paths, links and instruction consistency; no unrelated C++ rebuild |

Without `just`, use `python3 scripts/check_cpp.py dev`, replacing `dev` with
`asan`, `tsan`, `profile`, or `format` as appropriate. The detector check is
`python3 scripts/verify_cpp_tools.py`.

Builds use Ninja and ccache when available. Analysis runs in parallel, up to
eight workers by default; use `--jobs N` on `check_cpp.py` to adjust it. Each
invocation records diagnostics, tool versions, exit codes, and durations under
`build/reports/<mode>/`. Static analysis runs even when compilation is cached.

Use these runner commands to select LLVM, the macOS SDK, and ccache together;
raw CMake presets do not perform that tool discovery. The `dev` and `profile`
presets explicitly disable sanitizers, including when reusing a build directory
that previously had instrumentation enabled. ASan/UBSan and TSan remain separate.

The development preset generates `build/dev/compile_commands.json`. Point your
editor's clangd extension at the same LLVM installation. `.clangd` supplies the
database location and limits interactive analysis to fast checks. The full batch
checks still run through `just check`.

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
consumer. Current tests cover the toolchain and POSIX descriptor ownership with
real pipes; they do not cover terminal sessions or rendering. The
[checkpoint receipt](evidence/cpp-verification.json) records the published scope;
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

1. Build with `just profile` (optimized, symbols, sanitizers off). There is no
   desktop target yet; start application traces when the minimal terminal view
   exists. Put signposts in that first view, not only the later UI.
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
# Once the desktop exists: replace 12345 with its actual PID and use a fresh path.
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
