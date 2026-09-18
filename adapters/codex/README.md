# Codex adapter investigation

## Evidence boundary

This component remains an investigation; the current checkpoint adds no Codex
adapter, hooks or approval routing. Source was inspected on 2026-09-17 at revision
`256a74f942a2fae1d4c198797d6d5246063f06e3` of a local Codex checkout. The checkout
had an unrelated modified build script. Its locally built binary reported
`codex-cli 0.0.0`; that string does not prove it was built from the inspected HEAD.

The [runtime receipt](../../evidence/codex-probe.json) records the actual binary
hash, exported schema methods, and live initialization/list result. Schema
presence establishes an advertised shape, not successful delivery of real agent
attention. Approval/input events and hooks still require end-to-end qualification.

## Structured app-server route

Candidate for sessions owned by lapis: launch `codex app-server --listen stdio://`,
send newline-delimited JSON messages, complete `initialize` / `initialized`,
and consume server requests as well as notifications. Probe experimental opt-in
requirements against each installed build; the app-server CLI is experimental.

| Source method | Proposed lapis behavior |
| --- | --- |
| `item/commandExecution/requestApproval` | Track a command approval request |
| `item/fileChange/requestApproval` | Track a file-change approval request |
| `item/permissions/requestApproval` | Track a permissions request |
| `item/tool/requestUserInput` | Track questions requiring user input |
| `mcpServer/elicitation/request` | Track MCP-provided user interaction |
| `thread/status/changed` | Reconcile activity and waiting flags |
| `turn/started` | Mark a turn working |
| `turn/completed` | Inspect completed/interrupted/failed status |
| `serverRequest/resolved` | Retire the identified pending request |
| `error` | Inspect retryability; a retry is not terminal failure |

`ThreadStatus::Active` carries `waitingOnApproval` and `waitingOnUserInput`
flags in the inspected source. These summarize attention but do not replace the
original request payload and ID needed to submit a response.

Keep request IDs scoped to the connection and preserve the original response
shape. `item/tool/call` is a client tool execution request, not automatically a
human attention request. Account/authentication events also need separate handling.

Source pointers relative to the inspected Codex checkout:

- `codex-rs/app-server-protocol/src/protocol/common.rs`: method registry.
- `codex-rs/app-server-protocol/src/protocol/v1.rs`: initialization.
- `codex-rs/app-server-protocol/src/protocol/v2/thread.rs`: thread status/flags.
- `codex-rs/app-server-protocol/src/protocol/v2/turn.rs`: turn status.
- `codex-rs/app-server-protocol/src/protocol/v2/notification.rs`: request resolution.

## Ordinary Codex CLI route

Run the CLI under a PTY to retain its existing terminal interface. A separate
app-server process is not an observer of that TUI by default. Qualify native
hooks or a supported shared-server attachment before claiming CLI attention
coverage.

The inspected `codex-rs/hooks/src/lib.rs` lists `PermissionRequest`,
`UserPromptSubmit`, `Stop`, `Interrupt`, `SessionStart`, `SessionEnd`, and tool,
compaction, and subagent hooks. This is source evidence only. Hook dispatch,
configuration, payloads, nonblocking delivery, and response semantics are not
yet verified against the active binary. A Stop hook may participate in control
flow; it must not blindly be mapped to definitive task completion.

## Next qualification

1. Capture real input/approval requests with a controlled local fixture and
   explicitly chosen approval settings.
2. Reply using exact source request identities; verify resolution and continuation.
3. Verify cancellation, duplicate delivery, reconnect, and multiple pending requests.
4. Probe native CLI hooks independently; publish their actual capability matrix.
5. Compare structured sessions and PTY sessions before selecting the default UI.
