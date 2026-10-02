# Suggestions

lapis can guess what you will type next to an agent, as Cursor's Tab guesses an
edit, so a day of supervising agents can be spent pressing Tab.

## Turning it on

Add this to `~/.lapis/lapis.json`:

```json
{"nextPrompt": {"auto": true}}
```

The other settings are `model` (`claude-opus-5-5`), `effort`, `minConfidence`
(0, so any model-reported top guess shows) and `maxPerHour` (60). Each prediction
attempt uses the Claude Code plan this Mac is signed in to: API keys, custom
endpoints and cloud-provider variables are removed from the call. An attempt
may retry a malformed answer once, so `maxPerHour` bounds attempts rather than
individual provider requests.

Claude Code has its own prompt suggestions, which may show beneath lapis's. Set
`promptSuggestionEnabled` to false in Claude Code's settings to leave only
lapis's.

## How a guess is made

When a Claude Code or Codex agent finishes a turn, lapis reads its conversation
where it runs (over ssh for another machine) and asks the model, on this Mac,
with your `~/.claude/CLAUDE.md`, the agent's screen, every other agent's state
and your latest prompts elsewhere. The model's top guess shows dim after the
agent's cursor, as long as the model reported a probability for it. None shows
while an agent waits on a request such as a permission prompt.

## Tab

- **Tab** sends the guess: the service queues its text and Return together.
  A guess too long to show whole, or on several lines, is only typed, for you
  to read first. Suggestions require a service from this build; an older
  service refuses the operation visibly until it is upgraded and restarted.
- **Option-Tab** only types it.
- **Typing** leaves the guess there: start your own, clear it with
  Command-Delete, and Tab still takes the guess.
- **Tab with nothing offered** and nothing typed moves to the next agent that
  needs you: a guess you have not seen first, then a turn that finished unseen
  or a request, then a guess you passed over, the longest waiting first. When
  nothing waits, Tab goes to the program as usual. With the default
  `minConfidence` of 0 a finished Claude Code or Codex turn nearly always
  offers a guess, so Tab usually takes the guess instead of moving to another
  agent. Raise `minConfidence` to bring the attention queue back, or reach a
  waiting agent by opening it.

Tab keeps its usual meaning in shells and in CLIs lapis does not guess for.

## What is recorded

Every guess is kept, owner-only, in `~/.lapis/runtime/next_prompt.jsonl`: when it
was on screen, whether Tab or Option-Tab used it, how many keys you typed first
and how long you took, or that the next turn replaced it unused. A prediction
that failed (with a stable reason category) or was skipped at the hourly cap
is kept too. A use is recorded only after the service admits its input.
Admission does not guarantee the CLI consumed it before a crash. The log
rotates at 4 MiB and retains one previous file; helper output is bounded and
raw error output is never copied into the log.
Evaluation reports missing conversations, pending answers and failed calls
separately. Missing judge results are excluded from quality scores; `grading`
reports how many judgments were requested, how many succeeded and their coverage.
Scores describe only successfully graded predictions, so read them with that
coverage. Provider or
authentication failures stop the attempt; only a malformed answer is retried.

```sh
python3 scripts/next_prompt_eval.py log            # acceptance: of the guesses you saw, the share you used
python3 scripts/next_prompt_eval.py log --judge    # the ones you passed over, against what you typed
python3 scripts/next_prompt_eval.py replay --machine HOST   # the same prediction on past transcripts
```

The design and the measurements behind the defaults are in
[architecture](architecture.md#predicting-the-next-prompt-september-29).
