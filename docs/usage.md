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

[Architecture](architecture.md#storage-credentials-and-diagnostics)
owns the general storage and credential contract; [status](status.md#components)
records the remaining live qualification.
