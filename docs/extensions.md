# Extensions

This page records the proposed extension architecture. Nothing here is
implemented or advertised as a product capability yet; see
[status](status.md). The contract exists so a contributor can build a
Lapis Studio-style companion without forking the desktop, linking private
C++ objects, loading third-party QML into the app, or speaking the internal
session wire. The X1-X4 implementation gates live only in
[architecture](architecture.md); this page explains the contributor contract
without maintaining a second roadmap.

## Goal

Lapis Studio is treated as a second client of a live workspace, never a second
owner of that workspace. Its extensions can inspect workspace state, present
declarative panels and commands, and eventually ask Lapis to perform actions
that Lapis already owns. They cannot replace the renderer, own a PTY, route
approvals, or move keyboard focus.

The first supported package is therefore an explicitly installed extension:

- a versioned manifest and capability list;
- an optional data-only CLI descriptor for the harness catalog;
- an optional out-of-process Studio entry point;
- host-rendered declarative UI slots;
- extension-scoped private state;
- fixture-based protocol and manifest tests.

## Trust and package identity

An extension package is installed by an explicit user action. Installation
presents its identity, developer, resolved absolute executable path, executable
digest, bundle inventory digest when applicable, manifest digest, requested
capabilities, and protocol range before enablement. Presence on disk does
nothing. Lapis re-verifies the executable and manifest digests before every
launch; a changed path or byte sequence requires fresh consent. A manifest
digest alone never authorizes replacement executable bytes, and extension
commands are never resolved afresh through `PATH`.

For X2 development, an enabled sidecar runs with the user's normal privileges.
It is not OS-confined. The capability contract governs the supported protocol
and Lapis-owned UI, but it does not make an untrusted executable safe. Consent
must say this plainly: installing a Studio package is trusting that package
with the user's local privileges, just as installing a CLI is. Production
automatic start requires a separate reviewed decision to retain that explicit
trust or add a named, enforced restriction before sidecars are supervised.

The manifest contract is `lapis.extension/1`. A major mismatch fails the whole
package, with no partial catalog entries or sidecar start. Unknown fields may
be ignored only when their capability is known; unknown capability names fail.
Identifiers are unique and cannot shadow a built-in harness such as `codex` or
`claude`.

Executable fields are literal argv. They accept no shell string, environment
expansion, command substitution, caller script, dynamic library, QML module, or
debugger attachment. This is a Studio package identity, not a browser-content
capability and not a mechanism for a web page to obtain host authority.

## Capabilities

Version 1 has three capability groups:

| Capability | Grants | Does not grant |
| --- | --- | --- |
| `harness.descriptor` | A terminal-adapter CLI descriptor with only `id`, label, resolved absolute command, literal default argv, model-argument shape, and resume option | A Codex or Claude adapter, update command, probe behavior, response routing, or a process owned by the extension |
| `workspace.observe` | Versioned read-only workspace DTOs and coalesced state events | Direct reads of private workspace files, the session wire, transcripts, credentials, or workspace-control |
| `ui.slot` | Declarative models for named Lapis-owned UI slots | QML, JavaScript, pixels, native views, focus, keyboard, paste, or arbitrary layout |

Actions are deliberately absent from v1. A later major version may add a
separate `workspace.command` capability for operations Lapis already owns. It
must use per-command consent, explicit confirmation for destructive work, and
the desktop's existing error and audit paths. Even then, an extension cannot
compose raw CLI invocations, bypass the harness catalog, or obtain a direct
PTY, session, adapter, browser, or focus handle.

## Sidecar process and protocol

When an enabled package declares a Studio entry point, Lapis starts one
sidecar process for that package. The sidecar is not a session service and is
not loaded into the GUI. It may be manually launched during development;
production automatic start and supervision wait for the process-lifetime R1
result.

The transport is newline-delimited JSON-RPC 2.0 over stdio for the development
slice. An owner-only local socket is the later alternative when a second
Lapis-owned process needs to reach the same sidecar. The host is the sole
handshake initiator: it sends `initialize`, the sidecar replies with its
supported protocol range and selected capabilities, and neither side may wait
for later methods while that request is outstanding. Payloads are bounded,
schema-validated, and versioned. Exact byte and timeout limits must be selected
with a resource-safety rationale and regression tests; they are not part of
this architecture decision.

The protocol exchanges stable data-transfer objects, never serialized
`Workspace`, `Agent`, `Config`, transcript, attention, browser, or adapter
objects. Initial DTOs cover only workspace summary, categories, agents,
attention summaries, and extension health. Conversation and artifact reads are
a separate future capability with their own privacy review.

## Events and UI

Host-to-extension events are observations from the workspace DTOs. They are
published publish-to-publish: the first change after quiet is delivered
immediately, sustained changes coalesce to a deadline, and the deadline
publishes the newest state rather than replaying stale intermediate states.
Queues are bounded and report lag or explicit loss. Events never run on the
terminal input path.

Extension UI is a model, not code or pixels. A v1 slot registry may provide a
sidebar section, stage-adjacent inspector, Commands entry, Settings section, or
status segment. Models are limited to labels, details, badges, rows, bounded
action references, and simple declared controls. Lapis owns theme, layout,
accessibility, keyboard navigation, focus, IME, selection, paste, and reduced
motion. A malformed model makes that slot explicitly unavailable; it never
falls back to executable content.

## State ownership

Each extension owns private state under an ignored, owner-only,
extension-scoped runtime directory. Lapis records enablement, package digest,
protocol range, health, and user-visible diagnostics separately.

Extensions never write `workspace.json`, `gui_state.json`, `agent_state.json`,
`factory_state.json`, `attention.jsonl`, or Lapis configuration sections. They
do not read these files directly. If a future judge or artifact capability is
added, it goes through a versioned protocol sink that preserves the existing
rule that live workspace state outranks external judgment.

## Failure and performance

A missing, stale, malformed, hung, or crashed extension is explicit
`unknown` or `unavailable`. It cannot block terminal input, answer a request,
close an agent, or change focus. A sidecar restart gets a new epoch and must
re-negotiate before its prior UI or state is trusted.

The only reviewed performance budget today is the existing 20/30/35 ms input
budget. No synchronous extension work may occur in key handling, paste, IME,
terminal publication, or renderer submission. Extension dispatch happens after
input publication and visible frame presentation. Idle extension UI must not
draw or wake the renderer. A hidden CEF view already costs six processes and
roughly 570 MiB RSS in the recorded W0 probe, so a web view is not the Studio
logic host; one may later display selected rich content only under the separate
native-browser ladder.

Startup, IPC, resource, crash-containment, and contributor-iteration numbers
must come from named extension-host workloads before any is promoted to a
release budget. The absence of those measurements is a gate, not permission to
infer a budget from the session service.

Follow the X1-X4 delivery ladder in
[architecture](architecture.md#extension-system). No phase may displace the
current release order. In particular, production sidecar supervision depends on
the unresolved R1 process-lifetime result, and browser-backed UI remains behind
the native-browser W checkpoints.
