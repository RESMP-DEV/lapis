# Config

Settings live in `lapis.json`: `~/.lapis/lapis.json` for the app, the checkout's
`lapis.json` for a development build. It applies as soon as it changes, whether
Appearance, you or an agent edits it, and Command-R reloads it by hand. Besides
key bindings and appearance it holds these:

```json
{
  "newAgent": {
    "harness": "codex",
    "folder": "~/dev",
    "mode": "edits",
    "machines": {"devbox": {"folder": "~/work"}},
    "models": {"codex": ["gpt-6-astra", "gpt-6-sol"]}
  },
  "harnessArguments": {"claude": ["--dangerously-skip-permissions"]},
  "alerts": {"sound": true, "finished": true, "repeat": 3, "notify": true},
  "editor": "Cursor",
  "keepAwake": true,
  "usage": {"show": true, "meter": ["codex", "claude", "grok"], "machines": ["devbox"]}
}
```

## New agents

`newAgent` holds the defaults for the new-agent form: the CLI, the starting
folder, the approval mode (`edits`, `auto` or `full`), models to offer, and, per
machine in `machines`, a folder that differs. See [agents](agents.md).

`harnessArguments` adds your own flags to every new agent of a CLI, alongside
lapis's selected model, approval mode and resume options. Configured approval
flags take precedence for a request with no explicit mode. Shell aliases do not
apply because lapis starts the executable directly.

## Alerts

An unseen completed turn or request plays one quiet cue when `alerts.finished`
is enabled. Requests use the same cue and background notification; custom sound
files do not restore repeating request chimes. Appearance has one switch and
Play button for the shared cue, plus the background-notification switch.

A turn that ends on what you already saw of that agent stays quiet: while lapis
is in front, the screen of the agent you are looking at is noted every second,
and a finished turn whose screen above Claude Code's input box is unchanged
neither chimes nor notifies. Every decision (chimed, notified, or quiet and
why) is logged, one JSON line each, to `runtime/attention.jsonl` in the lapis
folder.

Use `alerts.finishedFile` for your own sound. When it is unset, `alerts.soundFile`
is used at half volume. An explicitly set but unavailable `finishedFile` falls
back to the built-in finished cue rather than borrowing a different file.

```json
"alerts": {"finishedFile": "~/.lapis/sounds/done.wav", "finished": true, "notify": true}
```

Paths may start with `~/`; relative paths start beside `lapis.json`. Files are
limited to 4 MiB. Checks and reads run in the background, and the current cached
sound or the built-in cue plays while a check is pending. Edits, deletion and
permission changes are picked up asynchronously. File and playback diagnostics
update without reloading the configuration. On macOS, a file the playback layer
cannot use falls back to the built-in cue at its normal gain. Format and native
playback qualification are recorded in [status](status.md). If the built-in cue
also cannot play, the diagnostic reports that chime playback is unavailable.

## The rest

- `editor` is the app that opens an agent's folder (else the first of Cursor, VS
  Code, Zed, Windsurf and Sublime Text installed).
- `keepAwake` keeps the Mac from sleeping while it is plugged in, so the phone
  can reach it.
- `usage`, `accounts` and `limitResets` are described in [usage](usage.md).
- `nextPrompt` turns on guessed next prompts; see [suggestions](suggestions.md).
- Key bindings are listed in [keys](keys.md); Appearance shows and edits them.
