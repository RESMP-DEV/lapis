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
remote copies preserve the previous token. The panel names failures. Escape
ends an unfinished sign-in, and a new attempt retires older copies. Codex plans are
still added with the script below.

`scripts/lapis_accounts.py` fills the section too:

- `homes` records each machine's own sign-ins.
- `add-claude NAME --email E` runs `claude setup-token` and keeps the token
  (owner-only) on this Mac and the usage machines.
- `add-codex NAME --email E --on local devbox` signs Codex in to a home kept for
  that plan, which shares its sessions and settings with `~/.codex`.

Credentials never appear on a command line: a remote session reads its plan's
credential on its own machine.
