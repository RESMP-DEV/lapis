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

`scripts/lapis_accounts.py` fills the section:

- `homes` records each machine's own sign-ins.
- `sign-in` goes through every listed Claude Code plan that has no token yet,
  one browser approval each.
- `add-claude NAME --email E` runs `claude setup-token` and keeps the token
  (owner-only) on this Mac and the usage machines.
- `add-codex NAME --email E --on local devbox` signs Codex in to a home kept for
  that plan, which shares its sessions and settings with `~/.codex`.

Credentials never appear on a command line: a remote session reads its plan's
credential on its own machine.

## Saved limit resets

Claude and Codex give accounts resets to spend later: claude.ai's **Reset for
free**, Claude Code's weekly session reset and Codex's banked resets. claude.ai
offers its button only on the web and in Claude Desktop, but a reset applies to
the whole account, so lapis spends them for you, as OMP does.

Every five minutes, on each machine with Claude Code or Codex agents, lapis runs
a small helper with that machine's own sign-in (on this Mac, Claude Code's
keychain item, which macOS asks you once to let lapis read). It spends a reset
when a usage window is used up for at least another hour and the reset clears
it, or when a reset would expire within 12 hours and the weekly window is at
least a quarter used. It then checks that the account reads as reset and posts
a notification. The Opus and Sonnet weekly caps count: a reset that would leave
one of them used up is kept. **Use a saved limit reset for this agent's plan** in
Commands spends that plan's reset at once, and no other CLI's.

`limitResets` in `lapis.json` tunes it: `auto` (true), `minBlockedMinutes` (60),
`keepCredits` (0; kept for a block, not for a reset about to expire) and
`salvageHours` (12).
