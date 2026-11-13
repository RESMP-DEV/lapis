# Suggestions

lapis can guess what you will type next to an agent, as Cursor's Tab guesses an
edit, so a day of supervising agents can be spent pressing Tab.

## Turning it on

Add this to `~/.lapis/lapis.json`:

```json
{"nextPrompt": {"auto": true}}
```

The other settings are `model` (`claude-opus-5-5`), `effort` (`low`, `medium`,
`high`, `xhigh` or `max`; empty leaves Claude Code's own) and `maxPerHour`
(60). Each prediction
attempt takes the route your agents take: the `env` in `~/.claude/settings.json`
(a plan gateway, say) applies, while API keys, custom endpoints and
cloud-provider variables lapis itself inherited are removed. Without that `env`
the call uses the account in this Mac's keychain. An attempt
may retry a malformed answer once, so `maxPerHour` bounds attempts rather than
individual provider requests.

### Several models

`providers` replaces that single call with an ordered list of entries, tried
first to last, and `random` adds a pool to draw from first. Each entry is
either a coding-plan CLI or an OpenAI-compatible endpoint:

```json
{"nextPrompt": {
  "random": [
    {"name": "fable", "cli": "claude", "model": "claude-fable-5-2", "effort": "high"},
    {"name": "astra", "cli": "codex", "model": "gpt-6-astra", "effort": "high",
     "label": "GPT-6 Astra · high"},
    {"name": "carbon", "cli": "agy", "model": "gemini-4-carbon-high", "effort": "high"}
  ],
  "providers": [
    {"name": "primary", "url": "http://gpu-host:8317/v1",
     "model": "deepseek-v4.1", "apiKeyEnv": "PRIMARY_KEY", "timeoutSeconds": 20,
     "allowInsecureHttp": true},
    {"name": "fallback", "url": "https://openrouter.ai/api/v1",
     "model": "deepseek/deepseek-v4.1-flash-1017", "apiKeyEnv": "OPENROUTER_API_KEY"}
  ]}}
```

A CLI entry names `cli` (`claude`, `codex` or `agy`) and `model`, and
optionally `effort` (`low` to `max`); it has no `url` or key, and an entry that
mixes the two shapes, names another CLI or another effort is dropped. It runs
the CLI on this Mac on the plan that CLI is signed in to, with every metered
key, endpoint and cloud-account variable removed, once, in an empty folder of
its own:

- Claude Code: the call above, `claude -p --model M --effort E --tools ""
  --setting-sources "" --strict-mcp-config --no-session-persistence
  --system-prompt … --output-format json`.
- Codex: `codex exec --model M -c model_reasoning_effort=E --sandbox read-only
  --ephemeral --ignore-user-config --ignore-rules --skip-git-repo-check --cd
  <folder> --color never --json --output-last-message <file> -`, so neither
  `config.toml` (its MCP servers, hooks and profiles) nor rules load and no
  session is saved. Codex has no per-call system prompt, so the instructions
  lead the message on stdin.
- Antigravity: `agy --model M --output-format json --mode plan --sandbox
  --disable-slash-commands --print-timeout Ns --print <message>`. The model is
  chosen for that call only; `/model`, which changes your default, is never
  used. An Antigravity model id carries its thinking level
  (`gemini-4-carbon-high`), so `--effort` is passed only when the id does not
  end in it. Antigravity has no per-call system prompt either, and saves the
  conversation as it saves any other.

Each guess draws up to two entries from `random` at random (every order
equally likely, entries cooling down left out), then tries `providers` in
order. Without `random`, the ordered list alone; without either, the single
Claude Code call.

`label` is the author shown beside the guess (in the visual view's bar, and
after the Tab key in the terminal when the row has room). It defaults to the
model and effort: `Fable 5.2 · high`, `GPT-6 Astra · high`, `Gemini 4 Carbon ·
high`. A guess whose author is not known shows none.

An endpoint entry needs `url` and `model`; `name` defaults to the model. `apiKeyEnv` names
a variable read from the process environment, then `~/.env_vars`; the key is
never stored in `lapis.json`. Without it the request carries no
`Authorization` header, for a local endpoint. The key is sent only to the
endpoint it is configured for: a redirect does not carry it. A keyed
`http://` endpoint off this machine is refused unless the provider sets
`allowInsecureHttp: true`, since cleartext would expose the key and the
prompt text to the network path; loopback endpoints and `https://` are
unaffected.
An endpoint's `maxTokens` defaults to 4096 (128 to 8192) and `temperature`
to 0 (0 to 2).

`timeoutSeconds` is an entry's own time, a repair attempt included: 30 by
default for an endpoint (1 to 60), 75 for a CLI (1 to 120). A timeout, an
error or non-zero exit, a usage or rate limit, a failed sign-in or an
unreachable host moves to the next entry at once, and a limited call is never
repeated at once; an unreadable or empty answer is asked once more on the same
entry, and two move on. If all fail, the guess records each entry's failure.

A guess's model time is its entries' timeouts added up (the two slowest pool
entries, since either may be drawn, and every ordered entry), at most 150
seconds; the app stops the helper 30 seconds after that, at most 180 as
before. The ordered entries after the one being asked keep their own time, so
a slow first pick shortens the second pick, never the last resort: with five
CLI entries at 75 seconds and an endpoint at 30, the first draw has 75, the
second 45 and the endpoint 30. The single Claude Code call keeps 150 seconds.

An entry that failed sits out the next guesses: after a CLI's usage or rate
limit, 15 minutes; after two failures in a row of any other kind (or two
limits at an endpoint), 2 minutes. An answer from it clears that, as does
restarting lapis. A CLI that is not on this Mac sits out too. When every entry
sits out, the guess is skipped (`reason: cooling`) without counting against
`maxPerHour`.

Each `predicted` record in `next_prompt.jsonl` keeps who answered (`provider`,
`model`, `effort` and the shown `author`), the call's `cost` (Claude Code's
reported cost or the endpoint's usage cost; Codex and Antigravity report
none), `ms`, the pool's `drawn` order and its `seed`, the entries that failed
first (`fallbacks`: `provider` and `kind` only) and the ones left out
(`excluded`: `provider` and `why`, `cooling` or `missing`). A failed guess
keeps the same lists. `scripts/next_prompt_eval.py log` splits use by who
answered and by the first draw, which compares the pool's models without
favouring the ones that answer when others fail.

Settings of finished experiments are ignored, and an older `lapis.json` may keep
them: `experiment` and `experimentModels` (a second model), `localEndpoint` and
`localModel` (a local model), `holdout` (guesses made but never shown),
`minConfidence` and `experimentMinConfidence` (a probability floor) and
`returnShare`. Every guess with a probability is offered.

Claude Code has its own prompt suggestions, which may show beneath lapis's. Set
`promptSuggestionEnabled` to false in Claude Code's settings to leave only
lapis's.

## How a guess is made

When a Claude Code, Codex or OMP agent finishes a turn, lapis reads its conversation
where it runs (over ssh for another machine) and asks the model, on this Mac,
with your `~/.claude/CLAUDE.md`, the agent's screen, every other agent's state
and your latest prompts elsewhere. The model writes one guess, preferring a
short reply when that would do, with its probability. The guess shows after
the agent's cursor: dim, or nearly as bright as text when the probability is
0.45 or more (from Oct 3 to 9, guesses scored that high were taken 65% of
the time, against 25 to 34% below). None shows while an agent waits on a
request such as a permission prompt.

The prompt asks for what you would actually type, not an ideal message. It
reads the agent's latest reply as a mode first: a question, a choice or a wait
is answered or met with a clarifying question; a result can be approved,
corrected, or used to start a new task; a list of next steps is not repeated
as a command. On 41 past prompts this mode framing and a shorter prompt
produced more sendable top guesses than the earlier best-version checklist,
which turned question and status turns into new instructions.

A turn the agent ends on its own, with no prompt from you since a guess you
never saw (an agent woken by its background work), is not guessed at once: the
guess is made when you next show that agent. From Oct 7 to 9 such repeats were
44% of guesses, and 72% of them went unseen.

When you show an agent whose guess is at least `returnMinutes` old (default
30) and was not typed, it is guessed again, telling the model how long it has
been; each is logged as a `return` event. An offline replay of 232 past guesses
(Oct 16) found guessing again after a return the largest effect it measured.

At a few dozen guesses a day few live comparisons can reach a verdict, so two
are split. Each is drawn on its own for every guess, so each comparison uses
every guess:

- **Context** (`contextArms`, default `["base"]`): `base` sends the
  conversation as above. The opt-in `wide` arm asks for five candidates, logs
  all five, and offers the first; listing it with `base` compares them evenly.
  Any other name is ignored. Logged as `context_arm`.
- **Prompt** (`promptArms`, default `["base", "checks"]`): `checks` adds three
  checks to the system prompt. An approval, continue or ship is five words or
  fewer. Before approving a result, the model compares the reply's restated
  goal with what you last asked for and, when the target drifted or an item was
  dropped, writes your correction instead. When your recent prompts or another
  agent show a different machine, folder, conversation or number than the
  agent is about to use, your message supplies it. They come from your Claude
  Code history (Oct 2026): seen guesses of 1 to 5 words were taken 26% of the
  time and those over 40 words 8%; a quarter of your corrections supplied a
  fact the agent lacked; a redo round cost a median 47 minutes; and 2.6% of
  guesses were corrections. Logged as `prompt_arm`; `["base"]` turns it off.

`next_prompt_eval.py log` reports these as `by_context_arm` and
`by_prompt_arm`, and returns as `returns`. Taking a guess is not the whole
result: `log --redo` reads, for each prompt you sent after a guess, your next
message after the agent answered it, and reports how often that was a redo
(pushback, confusion, continue or status), by whether you sent the guess,
edited it or typed your own, and for taken guesses by prompt arm. Records
written before the finished experiments were removed keep their fields (`arm`,
`hidden`, `min_confidence`); the reports read them without those comparisons.

The model is also told which of your past prompts were guesses you took
unchanged (marked `[took a guess]`), so it does not learn its own words as
your style. Your prompts to other agents come from the last 60 days of
transcripts, indexed in the runtime folder (owner-only,
`next_prompt_pairs.json`), a few seconds of work per guess until it is
complete.

An agent on another machine reports its finished turns through its own
terminal: lapis starts it with hooks (Claude Code) or a notify program (Codex)
that run a small relay with that machine's `python3`. The Codex relay preserves
the configured notify program and has a fallback for Python before 3.11.
Nothing is installed or left there. Without `python3` there, or for an agent
started before this, its turns are not reported and no guess is made; restart
it to add the hooks.

## Keeping folders apart

A guess for one agent carries your recent prompts to other agents and a line
for every other agent (title, category, status). Two lists keep work in some
folders out of that:

```json
{"nextPrompt": {"separate": ["~/work/client"], "off": ["~/work/private"]}}
```

- **`separate`**: an agent working inside a listed folder is walled off. Its
  prompts and its line reach only guesses for agents inside the same listed
  folder, and a guess for it sees only those. Agents outside every listed
  folder see each other as before.
- **`off`**: an agent inside a listed folder gets no guesses, so nothing of
  it is sent to the model, and its prompts and line reach no other guess.

Each entry is an absolute path or starts with `~`; anything else is ignored,
and at most 64 are read. Both default to empty, which changes nothing. A
folder holds what is inside it by whole path components, so `~/work/client`
does not hold `~/work/clientele`; where listed folders nest, the deepest
holds the agent, and `off` wins over `separate`. Matching is exact in case.

Where a conversation works is read from its transcript, on the machine it
runs on, with `~` and symlinks resolved there: every `cwd` Claude Code
records, the `cwd` of Codex's `session_meta` and of each `turn_context`, and
OMP's `session` entry. A conversation that recorded no folder, or folders on
both sides of a wall, cannot be placed: with either list set, its prompts and
line reach no other guess, and a guess for it sees no other agent; with an
`off` list it gets no guess at all. The folder lapis started an agent in
stands in for its own guess when its transcript records none, and must agree
with what the transcript records. Another machine's agent lines are compared
with the lists as written, since `~` there is that machine's home: a folder
written with `~` is only compared with entries written with `~`, an absolute
one only with absolute entries, and any other pair cannot be placed. Your
prompts on that machine are walled where the helper runs, so `~` in a list
means that machine's home there.

The prompt index (`next_prompt_pairs.json`) records each transcript's folders;
an index written before this is read again a few seconds per guess, and until
an entry is, its prompts are left out while a list is set.
`next_prompt_eval.py replay` samples with the same lists from `lapis.json`, so
an `off` conversation is never sampled and walled prompts stay out of samples;
manifests saved before this keep what they held. A guess not made for an `off`
folder is logged as `skipped` with reason `off_folder`.

An OMP agent on this Mac reports its turns through an extension lapis writes
next to its OMP overlay in the runtime folder (`omp-lapis-turns.ts`, loaded by
`omp-lapis.yml`). It prints the same terminal sequences with a nonce lapis puts
in OMP's environment, only for the top-level agent (not subagents), and carries
the event and OMP's session id, never text. An OMP agent started before this
build has no extension loaded; reload it once.

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
  with a guess you have not seen, and not parked (looked at for 2.5 seconds and
  left without a prompt). A turn you looked at, its guess included, waits no
  longer. An agent at work, or paused while its own background work runs,
  never does. Among those, a request, an unseen turn or an unseen guess
  waiting over four hours comes first, then work categories, requests, turns
  you have not seen, and the newest.
  `tabAway` in [config](config.md#tabs-next-agent) names your work categories
  and the overdue time. When nothing waits, Tab goes to the
  program as usual. A finished Claude Code or Codex turn nearly always offers
  a guess, so Tab usually takes the guess instead of moving to another agent;
  reach a waiting agent by opening it.

Tab keeps its usual meaning in shells and in CLIs lapis does not guess for.

In the [visual view](workspace.md#visual-view)'s bar, where Tab has no other
meaning, the same round runs as Tab, Tab, Tab: the guess into the bar, sent
while unchanged, then on an empty bar the next agent that needs you, for every
CLI, with its own guess ready for the next Tab. There the guess counts as seen
once the bar shows it, and with nobody else waiting the bar says so.

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
(agent and category ids only, never titles or text). Tab's order does not
learn from them; they are the record for judging that order.

```sh
python3 scripts/runtime_digest.py                  # the last day: guesses that worked, taken by prompt arm, failures, Ultra Tab, pings
python3 scripts/next_prompt_eval.py log            # acceptance: of the guesses you saw, the share you used
python3 scripts/next_prompt_eval.py log --judge    # the ones you passed over, against what you typed
python3 scripts/next_prompt_eval.py log --redo     # how often your next message was a redo, taken or not, by prompt arm
python3 scripts/next_prompt_eval.py replay --manifest NAME --prompt-arm checks   # one saved sample under the checks
python3 scripts/next_prompt_eval.py replay --machine HOST   # the same prediction on past transcripts
```

The feature remains optional and is governed by
[architecture](architecture.md#deferred-work).
