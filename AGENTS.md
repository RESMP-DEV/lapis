# lapis agent instructions

## Purpose and scope

Build lapis into a portable desktop workspace for supervising many live CLI
agents. Sessions stay ready to display; a GPU-accelerated interface provides fast
switching, previews, and an attention-driven carousel. Codex is the first agent
integration. Each additional CLI gets an independent, verified adapter.

The project name is `lapis`, always lowercase and one word. The canonical GitHub
repository is `RESMP-DEV/lapis`. Work from the repository root and keep these names
consistent in documentation and UI text.

`AGENTS.md` is the canonical shared instruction file. `CLAUDE.md` must remain a
relative symlink to `AGENTS.md`; edit this file to update both tools' instructions.
Follow higher-priority session instructions and applicable parent workspace rules.

Act on the assigned task through implementation and verification. Make reasonable
reversible choices without repeatedly asking permission. Stay within the requested
milestone; a task to investigate or document is not authorization to build every
component or launch an unbounded set of agents.

## Establish context

1. Read [README.md](README.md), inspect the actual tree and `git status --short`,
   and identify the assigned scope before editing. Preserve other agents' work.
2. Read [architecture and near-term plan](docs/architecture.md) when touching
   shared behavior; it includes the attention and ownership contracts. Read the
   [Codex investigation](adapters/codex/README.md) for Codex-specific work.
3. Follow [CONTRIBUTING.md](CONTRIBUTING.md) for checks, profiling and PR procedure.
   Use existing scripts before writing replacements.
4. Verify live interfaces. Documentation, source definitions, exported schemas,
   replay fixtures, and actual runtime events are different kinds of evidence.

[README.md](README.md#current-status) owns the current implementation status.
Determine status from code and tests, update that table as components land, and
treat saved receipts as dated observations. Keep milestone acceptance in the
architecture document instead of creating another status or roadmap file.

## Architecture boundaries

- Keep process/session ownership, terminal state, agent protocols, attention
  policy, and rendering separate. The session service owns running agents; a GUI
  restart must not kill service-owned processes. Service failure and machine
  reboot require their own recovery behavior.
- Target macOS and Linux. Start on macOS, check Linux during engine selection,
  and qualify the minimal Linux terminal before expanding the desktop. Put GPU
  integration, POSIX PTYs, native input, credential storage and OS integration
  behind platform boundaries. Claim support only on platforms actually exercised.
- C++20 is the compiled baseline and service direction; pinned Ghostty VT is the
  selected candidate for the first production adapter. Keep its unstable C API
  isolated and close the recorded dependency-notice gaps before redistribution.
  Contour's C++23 requirement is confined to the experiment. Qt 6 Quick with a
  custom GPU terminal surface is the macOS visual-checkpoint implementation. Record decisions and
  evidence in architecture before dependent work branches out.
- Codex's Rust implementation does not require C++/Rust linkage. Prefer a
  supported external protocol; do not assume internal Rust crates are a stable
  embedding API. The [Codex notes](adapters/codex/README.md) describe two distinct
  routes: a lapis-owned app-server session and a normal CLI running under a PTY.
  A new app-server is not automatically an observer of independently launched CLIs.
- Preserve actual terminal behavior: Unicode, shaping, wide cells, escape
  sequences, resize, IME, selection, clipboard, and accessibility. Prefer an
  evaluated terminal engine over casually rebuilding terminal compatibility.

## Sessions, attention, and keyboard ownership

- Give each session a stable lapis identity. Keep connection state, activity,
  and a set of pending attention requests independent. A completed turn is not
  necessarily a completed task or an exited process. Silence means unknown.
- Normalize tool events through versioned adapters. Declare observation,
  response, and reconciliation capabilities separately. Hooks, protocols, and
  terminal bells have different reliability; heuristics are advisory only.
- Preserve source request ID types, thread/turn/item identity, and connection
  epochs. Deduplicate events, detect sequence gaps, and retire only the request
  actually resolved. Reconcile after reconnect before enabling stale responses.
- Keep session positions stable; prioritize a separate attention queue with
  aging and cooldowns. Support manual navigation, pinning, snoozing, and an
  opt-in automatic carousel without starving quieter sessions.
- Only lapis focus policy assigns keyboard ownership. Agent output can request
  attention but cannot redirect input. Defer automatic focus changes during
  typing, held keys, IME composition, paste, selection, dragging, or modal work.
  Never split a paste or composition between sessions or steal OS focus while
  the user is in another application.
- Surfacing, focusing, or acknowledging a request never approves it. Route an
  explicit user decision through the originating adapter. Hook-only adapters may
  require the user to answer in the original terminal. Attention hooks must not
  silently change the agent's execution or approval policy.

## Visual and motion direction

Use a minimal blend of Apple and Material design: mostly opaque surfaces, clear
hierarchy, restrained borders and color, and readable terminal typography. Motion
should make feedback immediate and changes easy to follow. Keep transitions short,
interruptible and consistent; do not defer input until an animation finishes or
animate terminal glyphs. Prefer position/color feedback over blur, transparency or
ornamental motion. Respect reduced-motion preferences when expanding navigation.
Keep detailed decisions in the architecture document, not another design file.

## C++ and resource discipline

- Product priority is responsiveness first, ergonomics second, visuals third.
  Target high-end M-series hardware with a 120 Hz reference workload. Measure
  input-to-presentation and warm switching tails, not just average FPS. Preserve
  correctness and keyboard ownership while optimizing.
- Spend RAM deliberately to keep session screens, recent history and useful
  render caches warm. Do not unload a session just because focus moved. Use
  generous configurable budgets, account for unified CPU/GPU memory and other
  agent processes, and evict cold data under pressure before interactive state.
- Prefer small components, explicit ownership, RAII, standard containers, and
  move-only wrappers for native handles. Avoid owning raw pointers, unnecessary
  inheritance, and custom allocators or lock-free structures without evidence.
- Keep blocking I/O, terminal parsing bursts, and agent work off the GUI thread.
  Define thread ownership and message boundaries; use bounded queues and explicit
  backpressure. Handle errors at process, worker, and UI boundaries rather than
  swallowing them or allowing exceptions to escape unexpectedly.
- Bound history, event queues, and CPU/GPU caches per session and globally. Keep
  current screens/recent history warm and back older history with disk. Live
  objects do not guarantee physical RAM residency; the OS can compress or swap.
- Coalesce display updates while preserving approval, input, and lifecycle
  events. Report loss of synchronization on control-channel overflow. Budget
  per-session processing so one noisy agent cannot monopolize the service.
- Cache glyphs, redraw changed regions, throttle previews, and stop rendering
  hidden panels while still consuming their output. Animate at display refresh
  rate and let static scenes sleep. Scaled previews must not trigger continuous
  terminal resizes.
- Link every first-party CMake target to `lapis_project_options`. Keep compiler
  warnings, formatting, and analyzer checks enabled; fix findings at their source.
  Any narrow suppression needs a reason tied to the actual code.

## Coordinating multiple agents

Parallelize independent work within the user's scope and the session's delegation
rules. Assign one coordinator to integrate results and own shared contracts.
Follow the [long-running work procedure](CONTRIBUTING.md#long-running-work): give
regular progress updates, diagnose overruns, bound retries and preserve a handoff.

| Work area | Primary scope | Required handoff |
| --- | --- | --- |
| Sessions and terminal | `services/session/` | Process/I/O lifecycle and replayable terminal behavior |
| Codex and other adapters | `adapters/` | Live event mapping, responses, capabilities, and recovery |
| Desktop and rendering | `apps/desktop/` | Input routing, terminal presentation, previews, and GPU evidence |
| Verification and profiling | `tools/`, `scripts/` | Behavioral cases, repeatable workloads, and measured results |
| Integration | Root build files and shared `docs/` | Compatible interfaces, assembled application, and milestone acceptance |

Before work begins, specify the objective, owner, allowed files, dependencies,
shared interface version, and acceptance commands. Start with independent
investigation where interfaces are unsettled, then agree on the smallest common
contract before parallel implementation. Coordinate shared-file edits explicitly.

Prefer a separate worktree/branch per implementation agent. Verify a common
committed baseline exists first: worktrees do not inherit uncommitted or untracked
files from another checkout. The coordinator must include the intended scaffold
in the baseline before creating worktrees.
Do not reset, clean, overwrite, or indiscriminately stage unrelated work.

When sharing a checkout, assign disjoint file scopes and one build owner. C++ check
scripts share `build/<preset>` and `build/reports/<mode>`; concurrent runs of the
same mode can overwrite each other's evidence. Use isolated worktrees for those
runs. Share caches where safe and bound concurrency by available CPU/RAM.

Workers report changed files, interface effects, commands and outcomes, evidence
paths, and remaining limitations. Communicate dependency conflicts promptly while
continuing independent work. The coordinator reviews the combined change and runs
integration checks; individual workers passing their tests is not integration
acceptance.

## Build in verifiable milestones

1. Prove one persistent terminal session: launch, input/output, resize, exit,
   bounded history, and GUI detach/reattach without killing the process.
2. Deliver the attention state machine with deterministic replay, then qualify
   real Codex input/approval requests, response routing, and resolution. Test
   duplicates, cancellation, reconnect, and simultaneous pending requests.
3. Connect the desktop to those working components. Verify keyboard ownership
   during typing/paste/IME and carousel transitions, using actual GUI evidence.
4. Exercise 32 sessions under controlled output load, including interactive TUIs.
   Measure latency and frame behavior; distinguish replay from real CLI agents.
5. Qualify a second independent CLI adapter and exercise platform backends before
   advertising tool independence or cross-platform support.

Build the smallest complete vertical slice for the assigned milestone. A mock
may unblock an interface experiment, but must remain labeled and be replaced
before claiming end-to-end behavior.

## Checks and evidence

The [required-check matrix](CONTRIBUTING.md#checks) is the canonical procedure for
both human and agent contributors. Apply every relevant row for the change.

Test behavior and failure recovery, not implementation details. The toolchain
smoke is not application coverage. Probe each CLI adapter against its installed
binary and record version/hash and capabilities; source or schema presence alone
does not prove real attention delivery.

Use `just profile` for optimized builds with symbols. Profile actual CPU work,
allocations, and GPU submission/presentation using the available platform tools.
Record workload, source revision, tool versions, display rate, session count,
output rates, and p50/p95/p99 latency/frame times. Report memory growth and idle
CPU/GPU use. Timing targets in CONTRIBUTING.md remain provisional hypotheses,
not PR/release gates, until promoted through a reviewed measurement decision.
Sanitizer timings do not represent release performance.

Pin new runtime dependencies and record their license/redistribution constraints.
Run the applicable dependency audit after manifest/lockfile changes. Follow the
[dependency procedure](CONTRIBUTING.md#dependency-audit-scope), including a C++
inventory/SBOM scan. A scanner finding no supported package sources is not a
vulnerability clearance.

Keep reproducible scripts and sanitized receipts in the repository, generated
builds/logs under ignored `build/`, and local runtime state under ignored
`runtime/`. Never commit credentials, private transcripts, or raw user prompts.
Keep `.sindexer/` in the root `.gitignore`. Preserve the instruction symlink.

## Completion and review

Keep `README.md` as the entry point and `docs/architecture.md` as the single
implementation plan. Consolidate decisions there; avoid parallel roadmap,
component-summary or research Markdown files. Keep specialized documentation only
when it supplies distinct operational detail, as CONTRIBUTING.md and the Codex
investigation do. Use the single `.github/PULL_REQUEST_TEMPLATE.md` for PRs.
Store sanitized evidence in `evidence/`.

Finish with the concrete outcome, relevant checks, evidence paths, and material
limitations. Update documentation when behavior or an architectural choice changes.
Distinguish implemented, exercised, measured, and still proposed work. Do not mark
a milestone complete while a required integration or acceptance check remains.

For PR work, follow the [contribution procedure](CONTRIBUTING.md#contribution-and-pr-procedure)
and applicable workspace rules. Where installed, the cross-tool canonical policy
is `~/.claude/memory-sync/review_policy_index.md`.
Use ready-for-review PRs by default, inspect complete thread-aware state, invoke
applicable reviewers, and address valid unresolved findings. Explicit optional
reviewer unavailability is reported, not treated as approval or a merge gate.
Required checks, human approvals, and repository protection remain gates.
