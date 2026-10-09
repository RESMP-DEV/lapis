# Status

What lapis does today, what has been qualified, and what remains. The user pages
([agents](agents.md), [workspace](workspace.md) and the rest) describe behavior;
this page records how far each part has been exercised. Update the table as
components land, from code and tests, and keep receipts as dated observations.

## Rollout

The next delivery target is a dependable macOS Apple Silicon early-access
release. The [production delivery order](architecture.md#production-delivery-order-september-30)
prioritizes a consolidated candidate, independent process ownership and verified
upgrades, input/account integrity, then workload and support qualification before
more integrations. The first workload target is eight local sessions; it is not
yet a measured capacity claim. Controlled 32-session qualification precedes wider
workload claims and further feature expansion. Source checks, daily use and
the published app are distinct evidence: the latest v0.5.0 release targets
`0ec13cb`, not every fix now on main. Support claims follow the OS, CLI versions
and artifact actually exercised.

## Release reconciliation (2026-10-01)

At the documentation-reconciliation main
`18bf34166d7c10c2bbce577526be894eb7052336` and the later PR70 merge
`d7db964b76c77e056573d19761c9e528f9413010`, v0.5.0 remains the latest release
and still targets `0ec13cb46491a4f63faa372dba7b58959426c7d6`. The
following repairs are therefore not claims about that published package. This
documentation-only reconciliation did not rerun behavior suites.

Implemented means the code is present at the named source. Exercised means a
dated receipt names its source and result. No tracked daily-use or measurement
evidence covers these six batches. Unknown means no tracked receipt establishes
execution.

| PR | Merged source | Evidence and non-claims |
| --- | --- | --- |
| #63 | `90d6abf6c185aeb1e077b465ce64107055a9a122` | Desktop preview and service attention publication now use last-publication pacing. The [pacing receipt](../evidence/update-pacing-review.json) records focused normal, ASan and TSan cases at source `8debe7ae623c5e9750c822feaff18f3e11d75f95`; the later merge changed test fixtures and later main changed pacing-adjacent source, so execution at this reconciliation revision is unknown. It is not a latency, throughput, GPU or native-input measurement. |
| #64 | `6c255c8374704260b59c17b41abf03f5ecc6bfb8` | iOS newer-history catch-up is paced from its last attempt. The production change and its Foundation probe surface are implemented, but no tracked receipt records their execution; this is not an installed-iPhone qualification. |
| #65 | `00474ef3eee3240e6a5fd0308de555f695ce76c4` | The current packaging source captures source/tree/version and clean/dirty state plus exact dependency inputs, atomically rebinds mutable app/DMG bytes, revokes stale approval/verification state, and release preflight requires a clean matching source, artifacts, notarization, notarized-scope qualification and appcast bindings. See the [manifest reconciliation receipt](../evidence/release-manifest.json); the [R4 source audit](../evidence/r4-provenance-source-audit.json) records its focused Python checks passing at `efb17db`. This does not upgrade v0.5.0, produce an artifact, or qualify native install, Sparkle, GPU or reboot recovery. |
| #66 | `31b716b648d2f68f6c8b8390584c6debb835da11` | The remote gateway bounds connection admission, listing fan-out and deadlines, and bounded reads/scans. The bounds are implemented in source with test surfaces, but no tracked receipt records their execution. This is not a measured load or latency qualification. |
| #69 | `230cd2ee9bcebcfe2b3ee3a18d2204b0c5dff95c` | A finished turn that ends on what the user already saw stays quiet, and chime and notification decisions—quiet reasons included—are written to the owner-only `runtime/attention.jsonl`. [Config](config.md) records the private-title and rotation contract. Production and workspace-test code are present, but no tracked receipt records their execution; this is not native Mac or daily-use qualification. |
| #70 | `d7db964b76c77e056573d19761c9e528f9413010` | Remote account preflight, Claude 2.1.286 fixture preparation, focused quality tooling, the opt-in supervisor state primitive and PTY output retention are implemented. The [release reconciliation receipt](../evidence/release-manifest.json) tracks PR65's implementation reconciliation and says that no behavior suite was rerun; it is not execution evidence for PR70. No tracked receipt establishes PR70 test execution. This is not a package, sustained-output, memory, provider-account or native qualification. |

PR65 supersedes the source-provenance portion of the September 30 release
audit: manifest enforcement is implemented on main. Its package, install,
update-transition and reboot gates remain open. PR63, PR64 and PR66 likewise
repair tracked source risks but do not close the broader responsiveness,
remote-host or packaged-workload gates.

The unmerged R4 candidate-gate source further blocks `package_macos.py release`
until a versioned manifest binds six sanitized, digest-bound receipts for
independent downloaded assets, notarized staged verification, fresh-user Finder
launch, installed Sparkle update with live sessions, update-failure recovery,
and registry/history migration rollback. No such receipts exist, so this is
release enforcement only; it neither produces a candidate nor qualifies any of
those behaviors. The
[source receipt](../evidence/r4-candidate-gate-map.json) and its
[binding receipt](../evidence/r4-candidate-gate-map-binding.json) record the
validated review-repair working tree, the focused checks, and explicit
non-claims. Neither is a gate-map instance, neither embeds a self-hash, and
neither grants candidate approval.

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
| PTY and separate session service | Explicit executable/argv/cwd, shell default, resize/paste/exit, failed launch, detached output and same-child reattachment on macOS; after two simulated power losses, the login helper resumes real Codex/Claude conversations and terminal-checkpoint stand-ins for four other CLIs with durable managed resume arguments ([check](../scripts/check_restore.py)) | PR70's output-pressure admission is implemented to pause reads at 64 KiB, retain bounded FIFO output, recover on the same child and separate bounded final teardown; its focused PTY and real-service backpressure test surfaces have no tracked execution receipt. Sustained-output soak and measured memory/headroom, an actual reboot through the login helper, native resume qualification for the four stand-in CLIs, and later Linux qualification also remain; on macOS 27 services share the window's coalition, so surviving a window quit depends on macOS allowing lapis in the background |
| Local transport | Version 6 identity/epoch/generation attachment, correlated history paging and service attention messages, restored-screen input gating, bounded queues and explicit reconnect; stale sockets left by a simulated power loss are replaced | Qualification across an actual reboot |
| Desktop and Vulkan surface | Qt key input through the live PTY, restored state, default/compact captures and cell-grid/font/decoration regression on M4 Max via MoltenVK; mouse selection, copy, wheel history paging, Command-hover destinations and Command-click opening of visible/OSC 8 links and local files; 14-pixel default and direct size controls (macOS background Qt tests, with URL dispatch intercepted) | Cross-cell contextual shaping, rectangular/multi-click selection and accessibility; opening a file at its line in an editor; native Mac selection not yet exercised; Linux GUI port is deferred |
| Responsiveness | Shared publish-to-publish pacing for service snapshots and attention updates, whole-screen local socket buffers with a socket-pair regression, and native input-to-frame p50 13.2 ms / p95 19.9 ms on the M4 Max reference display, gated by `just latency-budget` against the reviewed 20/30/35 ms budget | Frame submission (about 8.5 ms p50) is the next measured target; switch latency and sustained output remain separate unbudgeted workloads |
| History and input lifecycle | Disk quotas, history scrolled by rows as one strip, live-screen retention, same-PID reattach, real disk-full/corruption recovery; Qt and native macOS composition/paste/focus ownership tests | Archived rows keep their original width (cut or padded, not reflowed) |
| UI iteration and attention | Isolated source-QML reload, captures, configurable navigation and appearance; tiles on the stage (drag from the strip, dividers, keys, zoom; spatial tile keys and a next/previous agent walk that visits tiles first, offscreen Qt tests on macOS), dragging cards to reorder and between categories with multi-select, find in the terminal and text size (Qt tests on the Linux test host); live request badges, explicit approval/answer dialog, stale-state gating and draft preservation; live config reload, alert chimes and their repeat rules, Command-K agent search, the usage meter and per-machine dashboard, the keyboard home list, Command-O resume and the side terminal with its machine picker (Qt tests on the Linux test host); usage answers from the installed Codex 0.156.1, Claude Code 2.1.282, Grok 1.0.41, Kimi Code 0.39.1 and OMP 18.2.9, here and on a Linux host over ssh; the attention journal's encoder, bounded replay, recovery, rotation and service-side audit-gate policy have focused C++ execution evidence | Automatic carousel and larger session-count qualification; the chimes and usage view have not been seen and heard on a Mac by a test; PR69's seen-screen quiet rule has no tracked execution receipt, and the journal still needs native live-agent restart and decision-delivery qualification |
| Runtime tool status | Read-only Tools pane over canonical `harness_catalog`; focused catalog/probe, expiry, rerun and production-QML checks use fixture executables, and background shortcut UI passes | Not a real CLI version/auth/attention qualification; native GPU and macOS input evidence and the broader desktop GUI gate remain blocked by this sandbox |
| Optional next-prompt suggestions | Plan-backed CLI stand-ins, transcript identity and parsing, bounded helper/log failures, fair Tab navigation, service paste admission and presented-frame submission checks; normal, ASan/UBSan and TSan background Qt input checks | Native macOS GPU presentation and ongoing acceptance measurements with real predictions; disabled by default |
| Ultra Tab overlay (milestone 1, standalone app) | Reads the registry, the window's `agent_state.json` and composed `ultratab_cards.json`; deck order, eligibility and the four answers through a fake transport; card parsing, staleness, table numeric columns, link schemes and SVG sanitizing; window placement and settings parsing; the join, first-screen acknowledgement and submitted paste against a fake v6 service, with refusal reported and no takeover; the production QML offscreen with software Quick and Qt events to that window (every block type, the fallback, Command-click and Command-O, input during motion), with captures; `scripts/package_ultratab.py` builds a signed `Ultra Tab.app` that launches in `--list` mode | A live agent answered from the overlay; the NSVisualEffectView blur, the Carbon global key, dragging, accessory activation, focus hand-back and the SMAppService login item on a Mac; voice input; persistent history; notarization |
| Ultra Tab composed cards | Trigger on a new finished turn or guess, debounce, two-at-a-time limit, guess patching, held-card rule, fallback and change-only owner-only writes against a fake runner; the helper process plumbing and timeout with a shell stand-in; context assembly, SVG/size/link sanitizing and invalid-output fallback in Python unit tests with fixture transcripts and logs; one live plan-backed composition on this Mac | The local endpoint path has only a mocked request; composition quality across many agents is not measured |
| Ultra Tab for iPhone (prototype) | Separate SwiftUI app; the gateway's read-only `GET /api/deck` (registry without launch details, published state and composed cards, version long-poll) and `POST /api/agents/<id>/submit` (join, one submitted paste and Return, leave) with gateway unit tests and a live-service test that the desktop keeps its attachment and size; deck order, card parsing and SVG sanitizing, swipe mapping and the send path against a fake service in Swift unit tests; UI tests in the iOS 26.5 Simulator against the real gateway and session services with Mac-side clients attached: every block type, swipe right and left, scripted dictation then typing, a refused send coming back, captures | Not installed on an iPhone yet; Apple's speech recognizer has not run (the simulator tests use a scripted transcriber); no push notifications, so the deck updates only while the app is open |
| Saved account limit resets | Selected-plan targeting, durable two-phase admission, exact-credit reconciliation without replay, process locks and failure bounds through the actual helper with provider stand-ins | Real provider consumption, native keychain access and packaged-host qualification; pending outcomes are never automatically retried |
| Runtime diagnostics and support export | Read-only package/runtime shape, version metadata, registry shape/count, explicit 1–8 monotonic connect-only sampling with aggregate/per-classification timings and counters, and aggregate-only redacted-export fixtures; dispatcher help/error wiring and focused quality at dirty baseline `3276b5e` | Installed package/runtime diagnosis, live Unix-socket protocol or health evidence, workload/resource measurement, package release qualification, and packaged support-flow acceptance |
| Custom chime files | Background file loading, descriptor-based cache invalidation, permission recovery, runtime diagnostics, single-cue policy and fallback gain through a recording playback stand-in; both generated WAV cues decoded by native NSSound without playback on macOS | Audible playback, other audio formats and decoded-memory profiling; no claim that every macOS audio format is supported |
| Attention core | C++20 single-source reducer; typed IDs, exact retirement, bounded state, explicit decisions, recovery guards and deterministic ordering | Larger-workload profiling |
| Persistent supervisor state core | Opt-in state library and focused CTest target implement schema-v1 desired state, strict bounded control parsing, fixed-window token checks, singleton locking and owner-only atomic persistence | No daemon, launchd registration, control socket, process launcher, restart/reconciliation, GUI/CLI integration or native qualification; execution is recorded by the PR validation, not a tracked runtime receipt |
| Experimental supervisor runtime | Behind `LAPIS_BUILD_SUPERVISOR_RUNTIME`: state/control lifecycle, validated service launch payload, supervisor-selected service epoch, crash/reconstruction restart rotating epoch and spawn token, startup-only handshake retry, protocol-identity adoption, safe retained-peer admission, explicit control stop and focused build gate. The October 5 repair receipt records affected target compilation, state/runtime CTest passes and focused Python checks in the worker sandbox, then the parent-host rerun passed 3/3 focused supervisor cases including real-service adoption/crash restart and full repository quality | No GUI birth route, persistent daemon, launchd, package, environment admission, reboot, load or memory qualification; the worker sandbox bind skip remains an environment observation, not source acceptance |
| Claude Code hooks | Claude Code 2.1.286 fixture pin; 2.1.280 permission and structured-input hooks, terminal-only notices, same-child reconnect, `/clear` continuation and actual GUI capture | No GUI responses or authoritative hook-history reconciliation |
| iPhone app (prototype) | Gateway on the Mac over Tailscale or ZeroTier, admitting the owner's iOS or Android devices or members of its private ZeroTier networks; SwiftUI app listing categories and agents, drawing the Mac's cell grid and sending text, paste and keys; the phone joins beside the desktop so both stay in sync (services started by this build); starting an agent in a category from the phone through the Mac's lapis; a terminal on the Mac from the phone, and the resume list; renaming, ordering and removing categories, moving and restarting agents, and the Mac's awake, alert and usage settings from the phone; UI tests in the iOS 26.5 Simulator against real services and the real windowless lapis host with a Mac-side client attached, fake agents, and real Codex and Claude Code on a fake model; installed and used on an iPhone 17 Pro | Agents started before sync are taken over instead; no structured requests or push notifications; starting agents needs a lapis window or the login helper running; PR64's paced newer-history catch-up and PR66's bounded gateway/listing work have no tracked execution receipts and do not extend this older qualification |
| Mac app package | Qt 6.11.2 built with Vulkan (arm64, macOS 14 or later) with MoltenVK loaded directly; signed with the hardened runtime and notarized; Sparkle 2.10.0 updates signed with an EdDSA key and fed from the latest release; a restore/start-at-login helper, not an independently supervised process owner; PR65 package-manifest enforcement is implemented on main; earlier release checks cover architecture, minimum macOS, links outside the bundle, identifying strings, the update key, the bundled MoltenVK on an M4 Max, the windowless host and a launchd start taking the login shell's PATH; 0.1.0 opened and used with a live agent by a person | A package built through the manifest path, an installed Sparkle update, independent service birth and actual login/reboot qualification; notifications, the login helper and the Finder and editor actions not yet exercised by a check on a Mac; macOS 14 and 15 untested |
| Codex integration | Managed ordinary TUI, service-owned observer, live desktop approval/input responses, same-child reattachment, source close/restore reconciliation, cancellation and simultaneous live approvals; [installed binary qualification](../evidence/codex-binary-update.json); session-only Claude import protocol parsing with strict expanded-session details and isolated live import qualification ([expanded shape](../evidence/codex-import-expanded-shape.json), [session receipt](../evidence/codex-external-import-session-probe.json)) | Broader binary and request-kind qualification; service/desktop import onboarding, dedicated production server ownership, launch-record creation and duplicate reconciliation |

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
