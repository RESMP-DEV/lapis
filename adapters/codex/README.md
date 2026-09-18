# Codex adapter investigation

## Evidence boundary

The ordinary Codex TUI now runs through lapis's explicit PTY launch path. The
structured attention adapter remains an investigation: there are no lapis hooks
or approval routing. Source was inspected on 2026-09-17 at revision
`256a74f942a2fae1d4c198797d6d5246063f06e3` of a local Codex checkout. The checkout
had an unrelated modified build script. Its locally built binary reported
`codex-cli 0.0.0`; that string does not prove it was built from the inspected HEAD.

The [runtime receipt](../../evidence/codex-probe.json) records the actual binary
hash, exported schema methods, and live initialization/list result. Schema
presence establishes an advertised shape, not successful delivery of real agent
attention. Approval/input events and hooks still require end-to-end qualification.

On 2026-09-18 the read-only probe was repeated against the installed executable:
436 schema files, all ten tracked methods advertised, successful initialization
and an empty loaded-thread page on the newly started private server. No model
turn was started. The same executable's help advertises `--remote`, `--no-daemon`,
`app-server daemon` and `app-server proxy`; its effective feature listing reports
`hooks` and `daemon_auto_start` enabled. These observations establish available
options, not hook delivery or shared-thread observation.

The [session reconnect receipt](../../evidence/session-reconnect.json) extends the
no-prompt TUI check through wire v3: the desktop restores a verified session and
enables input only after the first screen. Use `--new-session` for initial launch,
then omit it to reconnect; `--discover` explicitly adopts an existing matching
service. These transport identities are separate from Codex thread/turn/request IDs.

## Integration route comparison

The first wiring slice preserves the ordinary CLI terminal interface. The
[architecture plan](../../docs/architecture.md#first-wiring-slice-explicit-cli-launch)
owns implementation order; this document owns Codex-specific qualification.

| Route | Current evidence | Observation / response / reconciliation |
| --- | --- | --- |
| Ordinary Codex TUI in a lapis-owned PTY | No-prompt editing/navigation/paste/resize, interrupt/exit, same-child reattachment and macOS GPU captures exercised | Terminal interaction works for the exercised cases; all three structured capabilities remain unqualified |
| Native hooks on that CLI | Source event names plus an enabled runtime feature | Dispatch, payload identity, trust, loss/reconnect and return semantics all require live probes; no hooks installed by lapis |
| TUI and lapis connected to the same app-server | Installed help advertises remote, daemon and proxy options | Candidate for retaining the TUI with structured attention; multi-client event delivery, request ownership and reconnect remain unqualified |
| Separate lapis-owned stdio app-server | Live initialization and loaded-thread listing pass | Structured request/response/reconciliation remain unqualified; does not render the ordinary TUI or observe a different server's threads |

The empty loaded-thread page describes the private probe process only. It does
not establish whether existing CLI sessions use a shared daemon or whether its
threads can be observed. Probe a dedicated server and disposable session before
selecting shared attachment. Preserve typed IDs and connection boundaries even
if two clients see the same thread.

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

The installed CLI advertises `--remote` endpoints including Unix sockets and
WebSockets, and `--no-daemon` for a backend independent of the shared daemon.
Use an explicitly selected mode in qualification and record it. Killing a TUI
process or retaining its PID says nothing by itself about daemon-owned work.
Do not start/stop the user's shared daemon or install global hooks for a probe.

The inspected `codex-rs/hooks/src/lib.rs` lists `PermissionRequest`,
`UserPromptSubmit`, `Stop`, `Interrupt`, `SessionStart`, `SessionEnd`, and tool,
compaction, and subagent hooks. This is source evidence only. Hook dispatch,
configuration, payloads, nonblocking delivery, and response semantics are not
yet verified against the active binary. A Stop hook may participate in control
flow; it must not blindly be mapped to definitive task completion.

## Next qualification

The [launch receipt](../../evidence/cli-launch.json) records the installed TUI
exercise with `--no-daemon`, normal/compact GUI captures and no submitted prompt.
Input/navigation/paste were driven through service IPC; the shell smoke separately
exercises Qt key routing. This does not qualify real IME or physical input latency.

1. Extend this baseline to actual native Codex keyboard/paste/IME workflows and
   controlled model turns. Keep the launch hash, backend ownership mode and
   startup-versus-model-turn distinction in every receipt.
2. On a disposable app-server/session, probe shared attachment and native hooks
   independently. Verify which connection receives each event and which client is
   permitted to answer it; do not infer this from method names or help text.
3. Capture real input/approval requests with a controlled local fixture and
   explicitly chosen approval settings. Record the effective provider/model and
   any trust or policy prerequisite; do not change normal launch defaults.
4. Reply using exact source request identities; verify resolution and continuation.
   Verify cancellation, duplicate delivery, reconnect, sequence gaps and multiple
   pending requests. Disable stale replies until the source is reconciled.
5. Publish the observed capability matrix and select the attention route. If a
   hook route can only notify, keep responses in the originating terminal; a
   structured-only session must remain distinguishable from the ordinary TUI.
