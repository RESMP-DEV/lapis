# Attention contract draft

Adapters translate tool-specific signals into this common model. This is a
design contract, not a finalized wire protocol.

## Independent state

- Connection: connected, disconnected, or unknown.
- Activity: idle, working, completed, interrupted, failed, or unknown.
- Pending attention: zero or more requests, each with its own identity and reason.

A single status enum would lose information: one session can be working while
also waiting on a user decision. A completed turn does not mean the process
exited or the overall project is complete. Silence is not a lifecycle event.

## Event envelope

Every normalized event carries a contract version, lapis session ID, adapter ID,
connection epoch, monotonically increasing sequence, receipt time, and kind.
Source thread/turn/item IDs are included when available. Use a monotonic clock
for in-process scheduling and wall-clock time only for presentation/auditing.

Event kinds:

| Kind | Meaning |
| --- | --- |
| `session.connected` / `session.disconnected` | Transport lifecycle |
| `activity.changed` | Explicit activity observation |
| `attention.requested` | Input, approval, completion review, failure, or notice |
| `attention.resolved` | A specific pending request ended |
| `session.snapshot` | Reconciled activity and pending attention |

Attention includes an ID, reason, bounded summary, source confidence, and source
request identity when applicable. Preserve JSON-RPC request ID types. Associate
IDs with the owning transport epoch to avoid replying to a reused ID after
reconnect. Do not conflate a request ID with a tool item ID.

Capabilities are declared per adapter and version: activity observation,
input/approval observation, resolution observation, response support, and
reconciliation. Unsupported observations remain unknown, not inferred successes.

## Attention queue and focus

De-duplicate requests by session, epoch, and source request identity. Retain all
distinct pending decisions; resolving one must not clear the entire session.
Order by configured priority and arrival time, with aging and a cooldown to
prevent one noisy agent from monopolizing focus. Session positions remain stable.

Manual mode highlights and queues requests. Automatic mode advances only while
the lapis window is active and interaction is idle. Typing, held keys, IME
composition, paste, dragging, selection, and modal dialogs defer automatic input
ownership changes. Provide pin, snooze, next-waiting, and return-to-previous actions.

Latch input to an explicit session owner; never split a paste or composition
between sessions. Background applications receive a notification, not unexpected
OS focus theft. Displaying an approval request never accepts it.

Distinguish viewing/acknowledging attention from replying to the underlying tool.
Submit decisions only through the originating adapter's supported response path;
hook-only integrations may require the user to answer in the terminal.

## Failure and recovery

Ignore duplicate sequences and detect gaps. On disconnect, retain pending
requests as stale and disable their response actions until reconciliation.
Reconcile against supported source snapshots; do not assume requests survive a
new connection. Explicit cancellation/resolution removes the corresponding
request. Unknown events cannot become automatic decisions or successful completion.

Behavioral acceptance scenarios: simultaneous approvals, duplicate delivery,
resolution arriving before a queued display update, reconnect with reused IDs,
output flooding during user input, and an attention request during paste or IME.
