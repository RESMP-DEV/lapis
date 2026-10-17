# Suggestions

lapis can guess what you will type next to an agent, as Cursor's Tab guesses an
edit, so a day of supervising agents can be spent pressing Tab.

## Turning it on

Add this to `~/.lapis/lapis.json`:

```json
{"nextPrompt": {"auto": true}}
```

The other settings are `model` (`claude-opus-5-5`), `effort`, `minConfidence`
(0, so the helper's top guess shows, including its numeric fallback for an
unscored candidate) and `maxPerHour` (60). Each prediction
attempt takes the route your agents take: the `env` in `~/.claude/settings.json`
(a plan gateway, say) applies, while API keys, custom endpoints and
cloud-provider variables lapis itself inherited are removed. Without that `env`
the call uses the account in this Mac's keychain. An attempt
may retry a malformed answer once, so `maxPerHour` bounds attempts rather than
individual provider requests.

Claude Code has its own prompt suggestions, which may show beneath lapis's. Set
`promptSuggestionEnabled` to false in Claude Code's settings to leave only
lapis's.

## How a guess is made

When a Claude Code or Codex agent finishes a turn, lapis reads its conversation
where it runs (over ssh for another machine) and asks the model, on this Mac,
with your `~/.claude/CLAUDE.md`, the agent's screen, every other agent's state
and your latest prompts elsewhere. The model writes one guess, preferring a
short reply when that would do, with its probability, and judges whether the
agent needs you ("needs", "steer" or "fyi"; see alerts.judge in
[config](config.md)). The guess shows after the agent's cursor whenever it
passes `minConfidence`: dim, or nearly as bright as text when the probability
is 0.45 or more (from Oct 3 to 9, guesses scored that high were taken 65% of
the time, against 25 to 34% below). None shows while an agent waits on a
request such as a permission prompt.

A turn the agent ends on its own, with no prompt from you since a guess you
never saw (an agent woken by its background work), is not guessed at once: the
guess is made when you next show that agent. From Oct 7 to 9 such repeats were
44% of guesses, and 72% of them went unseen.

Experiments: `experiment` (default 0.2) of guesses go to another arm, chosen
evenly from `experimentModels` (default `["claude-sonnet-5-5"]`, through the
same CLI) and, when `localEndpoint` and `localModel` are set, a local model
behind an OpenAI-compatible endpoint (for example
`"localEndpoint": "http://gpu-host:8000/v1"`). Each guess's `arm` (`control`,
`model` or `local`) and `model` are in `next_prompt.jsonl`, so arms can be
compared on what you then sent. Set `experiment` to 0 to always use `model`.

Three more arms run on every guess, each drawn at random and logged with it:

- **Context** (`contextArms`, default all four): `base` sends the conversation
  as above; `thread` adds your earlier messages in the conversation, from
  before the turns shown; `retrieval` adds the six past exchanges on this Mac
  whose agent reply best matches this one, with what you sent to each; and
  `evidence` tells the model to look for the reply's unmeasured claim and ask
  for it. Past exchanges come from the last 60 days of transcripts, indexed in
  the runtime folder (owner-only, `next_prompt_pairs.json`) a few seconds of
  work per guess until it is complete. Logged as `context_arm`.
- **Floor**: half the guesses use `experimentMinConfidence` (default 0.4) as
  their floor instead of `minConfidence`, when it is higher. The floor a guess
  was offered against is its `min_confidence`.
- **Return**: when you show an agent whose guess is at least `returnMinutes`
  old (default 30) and was not typed, `returnShare` (default 0.5) of those
  returns guess again, telling the model how long it has been; the rest keep
  the old guess. Each is logged as a `return` event with its `arm` (`reguess`
  or `keep`). Set `returnShare` to 0 to keep old guesses.

`next_prompt_eval.py log` reports these as `by_context_arm`, `by_floor` and
`returns`. An offline replay of 232 past guesses (Oct 16) could not tell the
context arms apart; guessing again after a return was the largest effect it
found.

A shown guess shapes what you send (you can take it), so how often you take
guesses is not how well they predict you. `holdout` (default 0.1, at most 0.5)
of guesses are made and logged but never shown, and compared with what you then
typed; `next_prompt_eval.py log` reports them as `held_out` beside the same
figures for shown guesses, and acceptance per arm as `by_arm`. The model is
also told which of your past prompts were guesses you took unchanged (marked
`[took a guess]`), so it does not learn its own words as your style.

An agent on another machine reports its finished turns through its own
terminal: lapis starts it with hooks (Claude Code) or a notify program (Codex)
that run a small relay with that machine's `python3` (Python 3.11+ to preserve
Codex's configured notify program through `tomllib`). Nothing is installed or
left there. Without `python3` there, or for an agent started before this, its
turns are not reported and no guess is made; restart it to add the hooks.

## Tab

- **Tab** types the guess at the prompt and stops there, for you to edit.
- **Tab twice** sends it as typed: a second Tab before any other key presses
  Return (never over a permission prompt). If a request arrives first, Tab is
  consumed rather than sent to the dialog and the typed guess stays armed.
  After an edit, send with Return. Suggestions
  require a service from this build; an older service refuses the operation
  visibly until it is upgraded and restarted.
- **Option-Tab** does the same.
- **Typing** leaves the guess there: start your own, clear it with
  Command-Delete, and Tab still takes the guess.
- **Tab with nothing offered** and nothing typed since you arrived or last
  pressed Return (or since Tab twice sent a guess) moves to the next agent that
  needs you. Only agents truly waiting on you count: a request, or a turn that
  finished with no background tasks, subagents or wakeups in flight, unseen or
  with a guess, and not parked (looked at for 2.5 seconds and left without a
  prompt). An agent at work, or paused while its own background work runs,
  never does. Among those, anything waiting over four hours comes first, then
  work categories, requests, turns you have not seen, and the newest.
  `tabAway` in [config](config.md#tabs-next-agent) names your work categories
  or picks another order. When nothing waits, Tab goes to the
  program as usual. With the default
  `minConfidence` of 0 a finished Claude Code or Codex turn nearly always
  offers a guess, so Tab usually takes the guess instead of moving to another
  agent. Raise `minConfidence` to bring the attention queue back, or reach a
  waiting agent by opening it.

Tab keeps its usual meaning in shells and in CLIs lapis does not guess for.

A guess outlives a restart of the window (an install, say): it comes back,
seen or not and under the same offer, once lapis has read the agent's
conversation and found no prompt sent since. A guess still being made when the
window went is made again, and a turn that ended while no window was open gets
one, without a chime. See [config](config.md#window-state-across-restarts).

## What is recorded

Every guess is kept, owner-only, in `~/.lapis/runtime/next_prompt.jsonl`: when it
was on screen, whether Tab typed it in (and a second Tab sent it), how many keys
you typed first and how long you took, or that the next turn replaced it
unused. Once the prompt you
then sent reaches the conversation, an `outcome` record keeps it beside the
guess: `as_offered` (sent unchanged), `edited` (Tab typed it and you changed it)
or `own` (you typed something else), with a `similarity` from 0 to 1 and the
text you sent. Similarity compares at most the first 2000 characters; prompts
over 256 characters use a conservative shared-prefix/suffix score rather than
quadratic edit distance. Every such bounded or fallback score is marked with
`similarity_bounded`; `as_offered` still requires the complete prompt to be
unchanged. A prediction
that failed (with a stable reason category) or was skipped at the hourly cap
is kept too. A model failure also keeps its `kind` (`limit`, `auth`,
`timeout`, `network`, `malformed` or `other`) and the first line of its
message, with long tokens, email addresses and home folders masked. Three
failed guesses in a row show in the corner of the agent's terminal, with what
they hit, until a guess succeeds. A use is recorded only after the service admits its input.
Admission does not guarantee the CLI consumed it before a crash. The log
rotates at 4 MiB into `.1`, and an older `.1` is kept under its date rather
than deleted, since these are labels; helper output is bounded and
only the cleaned first line of a model failure is copied into the log.
Evaluation reports missing conversations, pending answers and failed calls
separately. Missing judge results are excluded from quality scores; `grading`
reports how many judgments were requested, how many succeeded and their coverage.
Scores describe only successfully graded predictions, so read them with that
coverage. Provider or
authentication failures stop the attempt; only a malformed answer is retried.

Tab's choices are kept beside it in `~/.lapis/runtime/tab_away.jsonl`: every
move Tab made, with each waiting agent's features and score, and every agent
you settled on with the agents that were waiting when you left the one before
(agent and category ids only, never titles or text). The ranker refits from
them in the background and keeps its model in `tab_away_model.json`.

```sh
python3 scripts/runtime_digest.py                  # the last day: guesses that worked, taken by arm, failures, Ultra Tab, pings
python3 scripts/tab_away_eval.py log               # Tab's ranker against your logged choices
python3 scripts/next_prompt_eval.py log            # acceptance: of the guesses you saw, the share you used
python3 scripts/next_prompt_eval.py log --judge    # the ones you passed over, against what you typed
python3 scripts/next_prompt_eval.py replay --machine HOST   # the same prediction on past transcripts
```

The feature remains optional and is governed by
[architecture](architecture.md#deferred-work).
