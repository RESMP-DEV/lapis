# lapis Android client design

Date: 2026-09-27. Status: approved design, awaiting user review; implementation
not started. This file is the working spec for the Android companion client.
When the first milestone lands, this content consolidates into
`docs/architecture.md` and this file is removed, per the workspace rule that
architecture.md is the single implementation plan.

## Purpose and scope

Build an Android app equivalent to the existing iOS companion app
(`apps/ios/Lapis`, SwiftUI): it connects to the `lapis_remote.py` gateway on
the Mac over Tailscale or ZeroTier, lists and starts categories/agents,
joins an agent's live session beside the desktop, sends input, and pages
scrollback history. The Mac owns the terminal engine and the agent processes;
the phone only draws screen frames and sends input.

Two Android-specific features beyond parity:

1. **Foldable support for Galaxy Z Fold devices.** The cover display uses the
   phone layout; the unfolded main display uses the extra space through a
   configurable layout system (side-by-side navigation, terminal-first, and a
   later dual-agent stage), with automatic tabletop-posture docking.
2. **A permanent command bar** of common terminal controls: fixed keys
   (Esc, Tab, arrows, Ctrl chords, Home/End, PgUp/PgDn) plus user-editable
   command snippets sent as paste+Enter.

Exclusions: no local terminal emulation on the device, no background push or
background network, no snippet sync through the gateway, no changes to the iOS
app, no session-service (C++) changes.

## Decisions

- **Stack**: Kotlin + Jetpack Compose, native. First-class foldable APIs
  (`androidx.window`), Canvas-based cell rendering matching the iOS renderer,
  best input-to-presentation latency. Rejected: Kotlin Multiplatform (one
  consumer; iOS stays SwiftUI) and Flutter (weaker posture APIs, more platform
  bridging for IME and cell rendering).
- **Placement**: `apps/android/` in this repository. Single Gradle module
  `:app` with package-level boundaries; split modules only if it grows.
- **Identity**: app name `lapis` (lowercase, one word), applicationId
  `dev.lapis.remote`, matching the iOS bundle id's namespace.
- **Libraries**: OkHttp (HTTP + SSE line reading), kotlinx.serialization,
  kotlinx.coroutines, Jetpack DataStore, `material3-window-size-class`,
  `androidx.window`. No DI framework, no Retrofit. Minimums: Kotlin 2.0+,
  minSdk 29, target/compile at current stable.

## Reference contracts (verified 2026-09-27)

These facts are the port's foundation; cite them in review, not folklore.

- **Transport**: HTTP/1.1 + server-sent events on port 7349
  (`apps/remote/lapis_remote.py`). Endpoints: `api/agents` (GET/POST),
  `api/categories` (POST), `api/agents/{id}/close`, `api/machines`,
  `api/folders`, `api/agents/{id}/screen`, `api/agents/{id}/input` (POST),
  `api/agents/{id}/history?before=N|after=N`, `api/agents/{id}/stream`
  (SSE: `attached`, `frame`, `status` events), `api/captures`.
- **Screen frames**: `ScreenFrame` with cell runs decoded from unkeyed arrays
  `[text, foreground, background, flags, column, width]`; flags bold=1,
  italic=2, faint=4, underline=8, strike=16, cursor=32.
- **Input**: one POST body with optional `resize: [cols, rows]`, `paste`,
  `text`, `key` (+ `modifiers` nibble: shift=1, control=2, alt=4).
  `MAX_INPUT` is 32 KiB per field.
- **TEXT passes through verbatim**: `input_bytes` returns `wire::Kind::text`
  payloads unwritten-to-terminal unchanged
  (`services/session/src/session_service.cpp:1024`). The iOS KeyBar already
  sends `^C`/`^U`/`^D` as `{"text": "\u0003"}`-style control bytes
  (`apps/ios/Lapis/AgentView.swift:239-251`). **The command bar needs no
  session-service or wire changes.** Only `paste` is re-encoded
  (bracketed-paste sanitation) by `encode_paste`.
- **Named keys** are a navigation subset only (up, down, left, right, home,
  end, pageUp, pageDown, delete, enter, tab, backspace, escape), validated
  `<= TerminalKey::escape` (`session_service.cpp:1034`). Letters are not in
  the key table; ctrl chords go through TEXT.
- **Admission is OS-gated**: `TailnetAuth.allowed` requires
  `tailscale whois` login == Mac owner **and** `Hostinfo.OS == "iOS"`
  (`lapis_remote.py`, class `TailnetAuth`). Android devices are refused
  today. The `X-Lapis-Client` header value is not validated; its presence is.
- **SSE keepalive**: the gateway pings every 10 seconds, so a 40-second
  read timeout distinguishes quiet from dead (iOS: `Gateway.streams`).
- **ZeroTier CLI (verified empirically on this Mac, 2026-09-27)**:
  `zerotier-cli -j listnetworks` returns per network `id`, `name`,
  `status` (`"OK"` when joined), `type` (`"PRIVATE"`/`"PUBLIC"`),
  `assignedAddresses` (CIDR strings), and `routes` with
  `target`/`via` (`{"target": "10.243.0.0/16", "via": null}`). There is no
  `whois` equivalent: a peer's owner login and device OS are not knowable.

Open verification item: Tailscale is expected to report OS `"android"` for
Android nodes in `whois`. Verify against the real device at milestone A and
make the allowlist comparison case-insensitive.

## Gateway changes (Android admission + ZeroTier overlay)

The gateway becomes overlay-agnostic: it admits peers from the Mac's tailnet
(unchanged rules, plus Android) or from its ZeroTier networks. Still no
session-service or wire-protocol changes; the iOS app's behavior on a
tailnet is untouched.

### Android admission

`TailnetAuth.allowed` accepts `system in ("iOS", "android")` compared
case-insensitively; the module docstring says "an iOS or Android device of
the Mac owner".

### ZeroTier admission

ZeroTier has no identity lookup, so authorization is network membership:
joining a network requires the controller's approval, and membership is the
trust the gateway relies on. Documented semantic difference from the tailnet
rule: a device the owner did not authorize onto the network cannot reach the
gateway at all, but the gateway cannot further distinguish devices the owner
did authorize.

- New `ZeroTierAuth` beside `TailnetAuth`. The peer is admitted when its
  address lies inside a managed route (`routes[].target`, checked with the
  `ipaddress` module) of a network with `status == "OK"` and
  `type == "PRIVATE"`. Public networks never admit anyone. A bridged network
  whose managed routes cover LAN ranges admits those ranges too; that is the
  owner's network configuration, documented rather than second-guessed.
- The network list is cached (30-second refresh under a lock); admission is
  pure address math, so no per-peer subprocess runs (cheaper than tailnet
  `whois`, which shells out per unknown peer).
- The Mac's own assigned ZeroTier addresses are admitted (the simulator on
  this Mac).
- `hosts()` gains the Mac's ZeroTier assigned addresses (bracketed IPv6),
  so clients may connect by raw ZeroTier address.

### Startup and binding

- `main()` builds admission from whichever overlays exist: the tailnet when
  the `tailscale` CLI answers, ZeroTier when `zerotier-cli` lists an OK
  private network. At least one is required; tailscale is no longer
  mandatory.
- Default bind stays the Mac's tailscale address for tailnet-only Macs. With
  ZeroTier active (alone or beside tailnet) the gateway binds all
  interfaces, since per-peer admission and the Host header check — not the
  bind address — are the security boundary. `--bind` still overrides.
- Docstrings and `--help` copy say iOS or Android over Tailscale or
  ZeroTier.

### Tests

Extend the committed `scripts/tests/test_lapis_remote.py` (the untracked
`test_lapis_remote_bounds.py` belongs to the quality branch and is out of
scope): a `fake_zerotier` runner mirroring `fake_tailscale`; Android OS
admission; private-network peer admitted; public network or non-member
refused; own address admitted; `hosts()` includes the ZeroTier address;
tailnet absent but ZeroTier present starts ZT-only; both absent fails
cleanly.

## Client transport (Tailnet or ZeroTier)

Clients embed no overlay SDK; the OS-level Tailscale/ZeroTier apps provide
connectivity, and the host setting accepts a tailnet name or a raw ZeroTier
address (port 7349 default applies).

- **iOS**: cleartext HTTP to raw private-range IPs must pass ATS.
  `NSAllowsLocalNetworking` is expected to cover IP literals; verify
  empirically with an app-hosted unit test hitting an `http://` stub bound
  to this Mac's LAN or ZeroTier address, and if it fails, add
  `NSAllowsArbitraryLoads` with a justification comment (every request goes
  to the user-configured gateway only; browsers are refused server-side).
  Settings hint and `describe()` error copy mention ZeroTier.
- **Android**: `targetSdk` current disables cleartext by default; the
  network security config permits it, justified the same way.

## Architecture

Four packages under `dev.lapis.android`, each independently testable:

```
apps/android/
  settings.gradle.kts, build.gradle.kts, gradle/libs.versions.toml
  app/src/main/kotlin/dev/lapis/android/
    gateway/      LapisGateway, DTOs, StreamEvent, GatewayError
    session/      WorkspaceModel, AgentSession, ScreenCache, DiskCache
    terminal/     TerminalMetrics, TerminalRow painter, CellGlyphs, wrap
    ui/
      list/       categories, agent list, new-agent sheet, folder search
      stage/      terminal screen, banner, composer
      commandbar/ key row, snippet row, SnippetStore
      adaptive/   AdaptiveScaffold, LayoutMode, posture
    platform/     Command theme, DataStore wiring
  app/src/test/   JVM unit tests + sanitized gateway JSON fixtures
```

### gateway/ (port of Gateway.swift)

Same URL normalization (`http://` default scheme, port 7349, http/https only),
header `X-Lapis-Client: android`, `Cache-Control: no-cache`. DTOs mirror the
Swift models exactly, including `Run`'s positional-array decoding and the
optional trailing `column`/`width` fields. The SSE stream is a
`callbackFlow`/`flow` reading lines from the OkHttp source with a 40 s read
timeout and no overall call timeout. Input sending serializes through a
128-deep queue; overflow raises the same visible notice as iOS rather than
dropping keys silently. Paste+Enter is one compound body (`paste` + `key:
enter`), matching `submit(_:)`.

### session/ (port of Models.swift)

- `WorkspaceModel`: host-generation identity; any host change invalidates
  in-flight work and restores the disk cache for the new host (cache files
  keyed by SHA-256 of the host string). Prefetch cycle mirrors iOS: harnesses
  (60 s), machines (120 s), folder catalogs (this Mac 300 s, first three
  available ssh machines 600 s), then screens of up to eight running agents
  (20 s staleness). All parsing stays off the main dispatcher.
- `AgentSession`: states connecting/live/closed(reason, reopen); a connection
  generation UUID retires stale streams, history loads, and input queues so an
  old attachment can never close or interleave with a newer one; first-frame
  flushes a resize queued during connecting; history paging gathers up to 80
  rows per load, retries an empty archive at most once per second, follows
  newer pages while scrolled up, and distinguishes "nothing archived yet"
  from "start of history". `ScreenCache` shows the last frame at once on
  reopen.
- `DiskCache`: atomic JSON files under `filesDir/cache`, same per-host naming;
  cold launch shows the last listing before the network answers.

### terminal/ (port of TerminalScreen.swift + CellGlyphs.swift)

Compose `LazyColumn` of row painters on Canvas. Whole-row background fills
full row height so shaded blocks join seamlessly; runs draw at their own
column with pixel-snapped rects (`CellGlyphs.snapped` equivalent against
device density); box/block characters render as cell shapes via ported
`CellGlyphs` path drawing; text stretches use a monospaced font with
`Paint.measureText("M")` metrics and font-size setting 8–20. The
`wrap(line, columns, limit)` reflow ports verbatim, including wide-character
runs kept whole on their starting row and trailing blank-cell dropping.
History rows are immutable and cached by page-id set + width; only changed
rows recompose. Follow-bottom anchoring keeps the newest output in view across
history loads and size changes; scrolling near the top loads the previous
page. Accessibility: the terminal is one semantic element whose value is the
history + live text, as iOS does.

### ui/adaptive/ (foldable layout)

Reads `WindowSizeClass` and `WindowInfoTracker` folding posture.

- **Compact (cover display, ~6.2" 23.1:9)**: the iOS phone layout exactly:
  categories → agent list → stage.
- **Expanded** (window width class `Expanded`, ≥840dp; every current Z Fold
  main display qualifies, the cover display never does): persisted
  `LayoutMode` setting:
  - `sideBySide` (default): navigation pane left, terminal filling the rest —
    the desktop's strip-plus-stage model.
  - `terminalFirst`: full-bleed terminal, list behind a navigation rail.
  - `dualStage`: two agent tiles side by side. Not part of milestones A–E:
    the setting appears but is disabled with a "after parity" label until a
    later milestone adds it.
- **Tabletop posture** (`FoldingFeature` half-opened, horizontal hinge):
  terminal above the hinge, command bar + composer below it. Automatic, not a
  mode.
- **Resize discipline** (ported from `AgentView.fit`): keyboard-only height
  changes never resize the agent; width changes do; sends deduplicate to real
  grid-dimension changes so fold/unfold never thrashes the Mac's terminal
  size. Fold transitions recompute the grid from the new window metrics.

### ui/commandbar/ (permanent bar)

Persistent below the stage in every layout, both folded and unfolded;
hide/show lives in settings, default visible.

- **Fixed key row**: esc, tab, shift-tab, ↑ ↓ ← →, ⏎, ⌫, home, end, pgUp,
  pgDn, and Ctrl chords `^C ^D ^U ^L ^R ^Z ^W` sent as raw control bytes via
  TEXT (`\u{03}`, `\u{04}`, `\u{15}`, `\u{0C}`, `\u{12}`, `\u{1A}`,
  `\u{17}`), matching the shipped iOS KeyBar mechanism. Navigation keys use
  the named-key path with the shift modifier for shift-tab.
- **Snippet row**: user-edited commands persisted per-device in DataStore
  (JSON list of strings), sent as paste+Enter. Long-press edits or reorders;
  `+` adds. Not synced through the gateway.
- Width-adaptive density: horizontally scrollable strip on compact, fuller
  rows on expanded.

### ui/list/ and ui/stage/

Ports of `AgentListView`, `NewAgentView` (harness/model/mode/machine/folder
selection with cached catalogs and folder search), `SettingsView` (host,
font size, reset cache, plus the new layout-mode and command-bar settings),
`LapisApp` navigation, and `AgentView`'s stage: banner states (connecting /
closed with reopen / not-shared notice), composer (draft field, send-without-
Enter, send-with-Enter), font-size menu, and "Send screen to Mac" capture
(POST `api/captures` with PNG + frame JSON for rendering debugging).

### platform/

Command theme constants (dark opaque surfaces, teal accents, panel/edge
colors mirroring the iOS `Theme`), DataStore keys (`host`, `terminalFontSize`,
`layoutMode`, `commandBarEnabled`, snippets, `resetCache`), and connectivity
error mapping in `describe()` style ("The Mac did not answer. Check that
Tailscale is on here and the Mac is awake.").

## Lifecycle and error handling

Port of the iOS semantics: backgrounding closes the stream and returning
reopens it (agents never die with the app); closed reasons carry the reopen
flag that drives the "Open here again" button; host changes invalidate all
in-flight work via generation counters; stale SSE frames cannot clobber a
newer connection; input backpressure notifies instead of dropping. Android
additions: all gateway I/O off the main thread; process death is survivable
because the disk cache restores the last listing instantly. No background
network, no push: foreground-refresh only, as iOS.

## Testing and verification

- **JVM unit tests**: decode sanitized fixture JSON captured from a real
  gateway (stored under `app/src/test/resources/fixtures/`, scrubbed of
  hostnames, paths and usernames) into every DTO; `AgentSession` transitions
  against a fake gateway flow (attach, frame, status, stale-generation
  retirement, history paging including the empty-archive retry);
  `wrap()`/metrics parity cases mirroring the Swift logic; snippet store.
- **Compose UI tests**: command bar emits exact `Input` payloads; scaffold
  selects layout per width class; posture docking with a fake
  `WindowLayoutInfo`.
- **Gateway tests**: android admission case in
  `scripts/tests/test_lapis_remote_bounds.py`; full existing gateway suite
  still green.
- **Script**: `scripts/check_android.py` following repo conventions —
  Gradle assemble + unit tests; optional `--device` installs and launches on
  an attached adb device for live checks.
- **Device evidence** (milestone D): fold/unfold mid-session with clean
  re-grid, tabletop docking, and command-bar use on a real Z Fold or the
  foldable emulator (hinge simulation), recorded as sanitized screenshots in
  `evidence/`. Background/software results do not substitute for device
  evidence of fold behavior.

## Milestones

- **A — Slice**: gateway Android admission + ZeroTier overlay + Gradle
  scaffold + gateway client + workspace list + host settings. Finish line:
  the real Mac's categories are visible on the device over Tailscale or
  ZeroTier; unit and gateway tests green; `whois` OS string for Android
  verified and recorded (stub-runner tests cover both overlays meanwhile).
- **B — Stage**: `AgentSession` + terminal renderer + composer + history
  paging. Finish line: a live Codex agent on the cover screen; typing, resize
  and scrollback work; Mac and phone stay in sync.
- **C — Command bar**: fixed keys + snippet store/editor, permanent in both
  layouts. Finish line: `^C` verifiably interrupts a running agent (exercised
  live, not assumed), snippets round-trip across restarts.
- **D — Fold**: adaptive scaffold, layout modes, posture docking,
  persistence. Finish line: fold/unfold mid-session with correct re-grid on
  real hardware; mode and posture evidence stored.
- **E — Parity + consolidation**: new-agent flow (harnesses/models/modes/
  machines/folders), start/close, prefetch, capture; README status row;
  this spec's content merges into `docs/architecture.md` and this file is
  removed; profiling per the responsiveness policy (input-to-presentation and
  warm-switch tails at 120 Hz on the unfolded display).

## Risks

- `whois` OS string for Android nodes is presumed `"android"`; verified at
  milestone A before shipping the allowlist change.
- ZeroTier admission cannot check device OS or owner login (no `whois`
  equivalent); it trusts private-network membership. Bridged networks that
  advertise LAN routes widen admission accordingly — an owner configuration
  choice, documented in the gateway's help text.
- iOS ATS treatment of raw IP cleartext under `NSAllowsLocalNetworking` is
  presumed permissive; verified empirically in the iOS work, with
  `NSAllowsArbitraryLoads` as the documented fallback.
- Foldable emulator hinge simulation approximates but does not replace real
  Z Fold evidence; real-device qualification is milestone D's gate.
- Compose recomposition cost on 120 Hz frame updates is unmeasured; milestone
  E profiles it and, if needed, narrows invalidation to changed rows before
  any renderer redesign.
