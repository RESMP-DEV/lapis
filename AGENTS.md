# lapis agent instructions

## Purpose and scope

Build lapis into a portable desktop workspace for supervising many live CLI
agents. Categories own ordered agent tabs and remember their selections. One
GPU-accelerated terminal stage shows the selected agent. A strip of throttled
live previews under the stage is the category's navigation; previews never take
input or resize a terminal. There is no tab row. Agents dragged from the strip
onto the stage tile it, as iTerm2 splits a tab; each category keeps its tiles.
Normal use has no sample cards; the only plain shells are the side terminal's
(Command-`), one per machine for a quick command and never an agent. Codex is
the first agent integration.
Each additional CLI gets an independent, verified adapter.

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
3. Follow the [shared code standards](CONTRIBUTING.md#code-standards) and
   [CONTRIBUTING.md](CONTRIBUTING.md) for checks, profiling and PR procedure.
   Use existing scripts before writing replacements.
4. Verify live interfaces. Documentation, source definitions, exported schemas,
   replay fixtures, and actual runtime events are different kinds of evidence.

[README.md](README.md#current-status) owns the current implementation status.
Determine status from code and tests, update that table as components land, and
treat saved receipts as dated observations. Keep milestone acceptance in the
architecture document instead of creating another status or roadmap file.

## Contributor workflow

<contributor_workflow>

Use the shared [work record and review checkpoints](CONTRIBUTING.md#work-records-and-review-checkpoints)
for substantial implementation, maintenance and delegated work. Keep one current
record in the task context or ignored `build/<task>/work.md`; update it as findings
change. Incoming agents continue that record and verify its baseline before editing.

1. Establish the objective, exclusions, checkout/HEAD, dirty work, and relevant
   open PR heads or overlapping files. Preserve work already in progress.
2. Assign temporary write scopes and one build owner per checkout/preset. Agree
   on the data owner, identity/lifetime and compatibility contract before dependent
   edits. Contributors continue to share feature ownership.
3. Classify findings as defects, contract gaps, acceptance gaps or preferences.
   Choose a bounded batch with an observable finish line and the smallest relevant
   checks. Verify documented commands against the current CLI before using them.
4. Review the combined diff against the
   [quality review checkpoints](CONTRIBUTING.md#quality-review-checkpoints).
   Inspect worker changes and receipts; a completed worker or green provider status
   alone is not verification. Apply the existing required-check and reuse rules.
5. Hand off the actual source state, changed contracts, checks/results, evidence,
   unresolved findings and next action. Keep implementation and qualification
   status distinct. Update canonical documents when their contracts change.

These are manual contributor/reviewer obligations. `just quality` enforces its
documented mechanical checks; it does not prove architectural compliance or native
platform qualification.

</contributor_workflow>

## Architecture boundaries

- Keep process/session ownership, terminal state, agent protocols, attention
  policy, and rendering separate. The session service owns running agents; a GUI
  restart must not kill service-owned processes. Service failure and machine
  reboot require their own recovery behavior.
- macOS is the active target. Keep GPU integration, POSIX PTYs, native input,
  credential storage and OS integration behind sensible platform boundaries so
  Linux can be ported and qualified later. Claim support only on platforms
  actually exercised.
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

Use a tactical command-room interface with restrained spacecraft styling: dark
opaque surfaces, clear category/tab hierarchy, quiet luminous selection edges,
and readable terminal typography. Theme treatments coordinate color, typography,
border shape and motion; do not add decorative telemetry or animate glyphs. Motion
should make feedback immediate and changes easy to follow. Keep transitions short,
interruptible and consistent; do not defer input until an animation finishes or
animate terminal glyphs. Prefer position/color feedback over blur, transparency or
ornamental motion. Respect reduced-motion preferences when expanding navigation.
Keep detailed decisions in the architecture document, not another design file.

## Shared quality standard

[CONTRIBUTING.md#code-standards](CONTRIBUTING.md#code-standards) is the canonical
coding and review standard for humans and agents. Follow its language/tool
configuration, ownership, failure-handling and evidence rules. Run `just quality`
for the common checks, then apply the affected rows of the required-check matrix.
Use its [selection and result-reuse rules](CONTRIBUTING.md#selecting-checks-and-reusing-results)
to avoid duplicate runs; retain the original revision and scope of reused evidence.
Quality/maintenance tasks do not authorize feature wiring or resuming a tabled
milestone. Review findings must distinguish defects from optional preferences;
keep cleanup scoped and preserve other contributors' changes.

## Test host selection

<test_host_selection>

macOS is the active target; Linux UI work is deferred. Use the current checkout
for local builds, unit tests and background checks when it has the required
capabilities. Never require a particular hostname, SSH alias, account or absolute
checkout path. A remote machine is optional: use it only when selected by the
operator or current session, and verify its checkout and dependencies first.
An unavailable remote does not block suitable local checks.

Use `python3 scripts/lapis.py ui-review` (or `just ui-review`) for routine UI
reviews. It builds the required targets and runs workspace UI, shortcuts and
terminal input with explicit offscreen/software modes, preserving the user's
cursor, OS focus and system clipboard. It does not open the product application.
Use `--json` for a machine-readable result; logs, software-rendered PNG captures
and the receipt live under ignored `build/reports/ui-review/`. Inspect the saved
captures for visual changes. This is the default review path during edits.

Native desktop input, clipboard/input-source changes and foreground test windows
require authorization for that interaction; reuse authorization already given in
the session. Run those GUI checks serially. Background Qt/software results do not
replace native macOS input/IME or GPU acceptance. Keep `lapis.py build`, native
CTest entries and `lapis.py ui-check` for their documented integration and native
qualification scope. Report the specific missing capability when a check cannot
run; never substitute a background pass for failed native evidence.

For other compiled checks, build the affected CMake target and run focused CTest
expressions with `--no-tests=error`. Reserve the full static-analysis gate for
integration and handoff. If Linux is explicitly selected, `lapis.py linux-gui`
uses its own virtual display/window manager; keep that software-rendered evidence
platform-scoped.

</test_host_selection>

## Responsiveness and resource policy

- Product priority is responsiveness first, ergonomics second, visuals third.
  Target high-end M-series hardware with a 120 Hz reference workload. Measure
  input-to-presentation and warm switching tails, not just average FPS. Preserve
  correctness and keyboard ownership while optimizing.
- Spend RAM deliberately to keep session screens, recent history and useful
  render caches warm. Do not unload a session just because focus moved. Use
  generous configurable budgets, account for unified CPU/GPU memory and other
  agent processes, and evict cold data under pressure before interactive state.
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

Before work begins, complete the shared
[work record](CONTRIBUTING.md#work-records-and-review-checkpoints). Start with independent
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
`runtime/`. Never commit credentials, private transcripts, or raw user prompts,
nor machine identities: host and device names, network addresses, account IDs
and usernames in paths. Sweep each diff, commit message and PR text for them.
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
