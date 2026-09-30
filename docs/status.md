# Status

What lapis does today, what has been qualified, and what remains. The user pages
([agents](agents.md), [workspace](workspace.md) and the rest) describe behavior;
this page records how far each part has been exercised. Update the table as
components land, from code and tests, and keep receipts as dated observations.

## Rollout

The next delivery target is a dependable macOS Apple Silicon early-access
release. The [rollout gates](architecture.md#broader-macos-rollout-readiness-september-29)
prioritize process survival, input integrity, account identity, verified upgrades
and operational bounds before more integrations. Source checks, daily use and
the published app are distinct evidence: the latest v0.5.0 release targets
`0ec13cb`, not every fix now on main. Support claims follow the OS, CLI versions
and artifact actually exercised.

## Components

The underlying terminal and managed-attention milestones retain their earlier
qualification receipts. The two-session Milestone 3 workspace (flat manifest,
shared attention queue and guarded carousel) was
[qualified on macOS](../evidence/milestone-three-workspace.json) and then replaced
by this category workspace; its service, Codex and Claude Code work carries over. The category workspace's current checks and remaining
acceptance are recorded in [the evidence](../evidence/agent-workspace.json) and
[architecture](architecture.md#daily-use-agent-workspace-direction-september-21-review).

| Component | Exercised | Remaining |
| --- | --- | --- |
| POSIX resources and terminal adapter | Descriptor ownership and 14 Ghostty adapter cases on macOS and Linux ARM64 | Broader terminal compatibility |
| PTY and separate session service | Explicit executable/argv/cwd, shell default, resize/paste/exit, failed launch, detached output and same-child reattachment on macOS; after two simulated power losses, the login helper resumes real Codex/Claude conversations and terminal-checkpoint stand-ins for four other CLIs with durable managed resume arguments ([check](../scripts/check_restore.py)) | An actual reboot through the login helper, native resume qualification for the four stand-in CLIs, and later Linux qualification; on macOS 27 services share the window's coalition, so surviving a window quit depends on macOS allowing lapis in the background |
| Local transport | Version 6 identity/epoch/generation attachment, correlated history paging and service attention messages, restored-screen input gating, bounded queues and explicit reconnect; stale sockets left by a simulated power loss are replaced | Qualification across an actual reboot |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, default/compact captures and cell-grid/font/decoration regression on M4 Max via MoltenVK; mouse selection, copy, wheel history paging, Command-hover destinations and Command-click opening of visible/OSC 8 links and local files; 14-pixel default and direct size controls (macOS background Qt tests, with URL dispatch intercepted) | Cross-cell contextual shaping, rectangular/multi-click selection and accessibility; opening a file at its line in an editor; native Mac selection not yet exercised; Linux GUI port is deferred |
| History and input lifecycle | Disk quotas, history scrolled by rows as one strip, live-screen retention, same-PID reattach, real disk-full/corruption recovery; Qt and native macOS composition/paste/focus ownership tests | Archived rows keep their original width (cut or padded, not reflowed) |
| UI iteration and attention | Isolated source-QML reload, captures, configurable navigation and appearance; tiles on the stage (drag from the strip, dividers, keys, zoom), dragging cards to reorder and between categories with multi-select, find in the terminal and text size (Qt tests on the Linux test host); live request badges, explicit approval/answer dialog, stale-state gating and draft preservation; live config reload, alert chimes and their repeat rules, Command-K agent search, the usage meter and per-machine dashboard, the keyboard home list, Command-O resume and the side terminal with its machine picker (Qt tests on the Linux test host); usage answers from the installed Codex 0.156.1, Claude Code 2.1.282, Grok 1.0.41, Kimi Code 0.39.1 and OMP 18.2.9, here and on a Linux host over ssh | Automatic carousel and larger session-count qualification; the chimes and usage view have not been seen and heard on a Mac by a test |
| Attention core | C++20 single-source reducer; typed IDs, exact retirement, bounded state, explicit decisions, recovery guards and deterministic ordering | Larger-workload profiling |
| Claude Code hooks | Claude Code 2.1.280 permission and structured-input hooks, terminal-only notices, same-child reconnect, `/clear` continuation and actual GUI capture | No GUI responses or authoritative hook-history reconciliation |
| iPhone app (prototype) | Gateway on the Mac over Tailscale or ZeroTier, admitting the owner's iOS or Android devices or members of its private ZeroTier networks; SwiftUI app listing categories and agents, drawing the Mac's cell grid and sending text, paste and keys; the phone joins beside the desktop so both stay in sync (services started by this build); starting an agent in a category from the phone through the Mac's lapis; a terminal on the Mac from the phone, and the resume list; renaming, ordering and removing categories, moving and restarting agents, and the Mac's awake, alert and usage settings from the phone; UI tests in the iOS 26.5 Simulator against real services and the real windowless lapis host with a Mac-side client attached, fake agents, and real Codex and Claude Code on a fake model; installed and used on an iPhone 17 Pro | Agents started before sync are taken over instead; no structured requests or push notifications; starting agents needs a lapis window or the login helper running |
| Mac app package | Qt 6.11.2 built with Vulkan (arm64, macOS 14 or later) with MoltenVK loaded directly; signed with the hardened runtime and notarized; Sparkle 2.10.0 updates signed with an EdDSA key and fed from the latest release; a login item for keeping agents running; release checks for architecture, minimum macOS, links outside the bundle, identifying strings, the update key, the bundled MoltenVK on an M4 Max, the windowless host and a launchd start taking the login shell's PATH; 0.1.0 opened and used with a live agent by a person | An update installed through Sparkle (the first comes with the release after 0.2.0); notifications, the login item and the Finder and editor actions not yet exercised by a check on a Mac; macOS 14 and 15 untested |
| Codex integration | Managed ordinary TUI, service-owned observer, live desktop approval/input responses, same-child reattachment, source close/restore reconciliation, cancellation and simultaneous live approvals; [installed binary qualification](../evidence/codex-binary-update.json) | Broader binary and request-kind qualification |

## Evidence

The [first quality repair batch](../evidence/quality-repairs.json) exercises iPhone
history retirement across reconnects, atomic managed-session reopening, and the
expanded verification gates. Remaining repairs have bounded acceptance in the
[quality goals](architecture.md#quality-repair-goals-september-25-audit).
The [follow-up audit and repairs](../evidence/quality-followup.json) cover paste and
gateway ownership, bounded remote discovery, explicit build dependencies, and
focused CLI checks. The receipt distinguishes exercised behavior from remaining
native-input, deferred-history and TSan qualification gaps.
The [adapter-boundary follow-up](../evidence/adapter-boundaries.json) carries typed
observation phases over negotiated v6 IPC, centralizes CLI launch/resume
configuration, persists restart plans before creating connections and cancels
superseded descriptor writers. Legacy peers retain their existing protocol;
the receipt records background UI, compatibility and scoped sanitizer evidence.
The [local consolidation receipt](../evidence/local-consolidation.json) records the
rebased repairs, fresh checks and source hashes after integration with PRs 18–19.

[Desktop evidence](../evidence/desktop-preview.json),
[UI refinement evidence](../evidence/ui-preview.json),
[reconciled UI and test evidence](../evidence/reconciliation.json) and
[adapter evidence](../evidence/terminal-adapter.json) delimit these observations.
The [downloadable Mac app](install.md) carries its own Qt, MoltenVK
and dependency notices; a build from the repository is a developer build.
The old multi-layout preview is no longer the product surface. The explicit
`--ui-preview` developer fixture exercises the same category UI with synthetic
data; it is never added to a normal workspace. See the
[Milestone 2 plan](architecture.md#milestone-2-attention-and-codex-plan)
for the underlying attention route and its acceptance evidence.
The first checkpoint implemented the standalone attention core and exercised real
Codex request round trips. [Milestone 2 evidence](../evidence/milestone-two.json) now
records assembled service/desktop acceptance on macOS. The
[PR #7 review receipt](../evidence/pr7-review.json) covers subsequent lifecycle,
startup, queue-boundary and multi-question fixes. Its
[follow-up receipt](../evidence/pr7-review-followup.json) records request-ID and
reconciliation fixes, order-independent question checks and refreshed validation.
The [thread-repair receipt](../evidence/pr7-thread-repairs.json) records initial-thread
classification, config separator validation, preserved cleanup diagnostics and
the disposition of the remaining review threads.
The [latest PR #7 receipt](../evidence/pr7-classification-repairs.json) records
post-binding thread isolation, transport and fixture cleanup, backend exit
diagnostics, and the evidence-based disposition of the new review batch.
Following the quality cleanup in PR #6, Milestone 2
now includes the production Codex adapter, session-service integration and explicit
desktop response controls. Request arrival never moves keyboard focus. See the
[attention test procedure](../CONTRIBUTING.md#codex-attention-qualification).
Latency and warm-switch targets remain provisional.

Qualification history remains in [Milestone 1](../evidence/milestone-one.json),
[Milestone 2](../evidence/milestone-two.json), the
[PR #7 repairs](../evidence/pr7-classification-repairs.json), and the dated receipts
under [evidence/](../evidence/). These distinguish terminal behavior, native input,
GPU pixel checks, real adapter traffic, and timing measurements. The
[Codex route comparison](../adapters/codex/README.md#integration-route-comparison)
separates terminal operation from attention delivery.

## Claude Code

Claude Code's permission and input notices appear under **Requests**; answer
them in Claude's terminal. Lapis does not change Claude's approval policy or
install global hooks. The [Claude hook receipt](../evidence/claude-code-hooks.json)
records runtime qualification. See the
[hook contract and limitations](architecture.md#claude-code-hooks-an-observation-only-extension).
