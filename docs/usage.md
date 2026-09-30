# Usage

## The meter

`usage.show` in [config](config.md) puts plan usage under the categories: every
plan a CLI here is signed in to (Codex, Claude, Grok and Kimi, plus every account
OMP's logins hold), each showing what is left of its tightest window. The bar is
green with plenty left, orange under 30% and red under 10%. `usage.meter` picks
which appear and in what order; one that is not signed in is left out.

Clicking the meter opens a dashboard per machine (this Mac and each ssh host in
`usage.machines`): every account's windows with what is left, reset times, and
when the current pace runs out or how much it leaves at the reset, plus the
Codex and Claude tokens used on that machine today, this month and per day for
30 days, by model. Each CLI is asked through its own interface every five
minutes, without a prompt, a hook or a saved session. Another machine's CLIs are
asked over ssh, and its transcripts are counted there by its own python3. There
are no prices.

## Shared plans

Claude Code and Codex sessions can share several plans. List them under
`accounts` in `~/.lapis/lapis.json`: each plan's name, its email, its `home`
(the machine that signs in as it, `local` for this Mac) and the `machines`
where lapis keeps a credential for it.

A session takes its machine's own sign-in. When that plan reaches `switchAt`
percent (95 by default), a new session takes the plan with the most room that
its machine can use, as OMP ranks its accounts. A running session moves only
when a ready observer reports it idle or finished with a turn, restarting on the
other plan and resuming its conversation; output going quiet alone never
switches it. **Switch plan** in Commands moves an agent by hand.

**Add a Claude Code plan** in Commands signs in any account from inside lapis.
It starts Claude Code's own sign-in and opens its link in your default browser,
copies it, and keeps it shown with **Open again** in case a try goes wrong. Sign
in there with the account to add, then type that account's email and press
Return. lapis keeps the token Claude Code prints (owner-only, never shown),
and records the plan only after that token is stored. New plans start on this
Mac. Refreshing an existing plan also copies it to that plan's explicitly listed
`machines`, with at most 64 destinations per attempt. An unrelated ssh-config
entry never receives the credential. Each replacement is atomic; incomplete
remote copies preserve the previous token. The panel distinguishes a failed copy
from a token that arrived but could not be recorded in the plan configuration.
If saving the local configuration fails after storing the token, it says so and
retains that credential for recovery. Existing credential directories must be
private and owned by the current user; lapis reports incompatible permissions
without changing them. Escape
ends an unfinished sign-in, and a new attempt retires older copies. Codex plans are
still added with the script below.

`scripts/lapis_accounts.py` fills the section too:

- `homes` records each machine's own sign-ins.
- `sign-in` offers Claude plans missing a local setup token. Each token goes to
  that plan's destinations and the usage hosts. `--to HOST ...` overrides them;
  `--to` alone keeps it local. Invalid plans are skipped before authentication.
- `add-claude NAME --email E` runs `claude setup-token` and keeps the token
  (owner-only) on this Mac and the usage machines.
- `add-codex NAME --email E --on local devbox` signs Codex in to a home kept for
  that plan, which shares its sessions and settings with `~/.codex`.

Credentials never appear on a command line: a remote session reads its plan's
credential on its own machine.

## Saved limit resets

lapis can spend saved Claude Code and Codex resets for the plan selected by an
agent. Automatic spending is on by default: checks start after one minute and
repeat every five minutes. **Use a saved limit reset for this agent's plan** in
Commands asks for one immediately, only for that CLI and plan.

`limitResets` configures `auto` (true), `minBlockedMinutes` (60), `keepCredits`
(0) and `salvageHours` (12). Set `{"limitResets": {"auto": false}}` for manual
use only. Automatic restores require a sufficiently long block and coverage of
every exhausted window; expiring credits can be salvaged when a covered weekly
window is at least one-quarter used. A weekly session reset cannot clear an
Opus/Sonnet weekly cap.

Missing plan credentials or provider IDs, or an account mismatch, stop the
operation; email alone is not an account identity. A definite provider refusal
is reported with a bounded reason. If its reply
is lost, lapis reports an unknown outcome and checks the recorded credit without
spending again, even from another machine or plan alias for that account. An expired or absent credit can settle that uncertainty; this is
not reported as a newly spent reset. Expired, settled journals are reclaimed under capacity pressure; pending or
unrecognized records are never evicted. Corrupt, full or unwritable journals
refuse new operations. Keep the private runtime `limit-resets/` records when recovering
or reinstalling; deleting a pending record discards the protection against replay.
The [architecture contract](architecture.md#saved-limit-resets-september-29)
describes these bounds and the remaining live qualification.
