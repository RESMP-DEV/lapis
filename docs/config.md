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

## CLI updates

`harnessUpdates` turns off lapis's automatic update for the CLIs set to
`false`, so a version you chose stays installed:

```json
"harnessUpdates": {"omp": false}
```

New agents of a listed CLI start without running its update first, and **Update
this tab's CLI and reload it** says updates are off for that CLI instead of
running one. An update already running when you add the key still finishes
before its waiting agents start. Every CLI that is absent or set to `true` keeps
updating as before; `--no-harness-updates` still turns off updates for all of
them. See [keeping CLIs current](agents.md#keeping-clis-current).

## Alerts

An unseen completed turn or request plays one quiet cue when `alerts.finished`
is enabled. Requests use the same cue and background notification; custom sound
files do not restore repeating request chimes. Appearance has one switch and
Play button for the shared cue, plus the background-notification switch.

A turn that ends on what you already saw of that agent stays quiet: while lapis
is in front, the screen of the agent you are looking at is noted every second,
and a finished turn whose screen above Claude Code's input box is unchanged
neither chimes nor notifies. A request still chimes and notifies even then.

Every decision — a chime or a notification, and each quiet reason — is logged,
one JSON line each, to `runtime/attention.jsonl` in the lapis folder: the
moment, the agent's conversation title, its CLI, the event (`needs you` or
`finished`), a `kind` of chime or notification, and the `decision` (chimed,
posted, or the quiet reason). The log is owner-only, and the title can be the
conversation's first prompt, so the file is not for sharing. Before a line
would cross 2 MiB the log rotates to `attention.jsonl.1` (one predecessor, the
older one removed) and the fresh file opens with a `rotated` marker line.

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

## Interaction log

`interactionLog` keeps a local record of what you do in lapis's own windows, to
study later how you work with your agents (for example, to train a better
next-prompt guesser). It is off unless you turn it on:

```json
"interactionLog": {"enabled": true, "pointerSampleMs": 50, "maxFileMiB": 64, "maxFiles": 8}
```

It records only input lapis's windows receive, never input to other apps.
Records are JSON lines in `runtime/interaction.jsonl` in the lapis folder. The
folder must be private to you (`0700`); lapis refuses one others can read rather
than change it. The log itself is `0600`. Every record has a format version
`v`, a `run` id for this launch, a gapless `seq`, `mono_us` (a monotonic clock in
microseconds, comparable within one run) and `wall` (UTC, milliseconds). The
kinds:

- `key`: each press and release, with the key, its text, modifiers, auto-repeat,
  what had the keyboard (the agent's lapis session id and CLI, or the named item)
  and the selected agent, and `handled`: `agent` (sent to the program), a
  `shortcut` and its `action`, `copy`, `paste`, `suggestion-fill`,
  `suggestion-send`, `tab-away`, `held-for-request`, `ime`, `window` (a Command
  chord left to the window), `not-accepted` or `delivered` (taken by another
  item, such as a dialog's field). `returnedLive` marks a key that left history.
- `ime`: composition and commits, including dictation. `paste`: every paste into
  a terminal (Command-V, a dropped file, a Tab-filled guess) with its length and
  text (up to 65,536 characters, then `truncated`). `copy`: text copied from a
  terminal by Command-C, a drag or a double-click.
- `mouse`: presses, releases and double-clicks with the position in the window
  and what is under it (`target`: the named item and its named ancestors, the
  `area` such as `stage`, `strip`, `side-terminal`, `categories` or `dialog`,
  and the agent's session id where there is one). `wheel`: one record per scroll
  gesture per interval with summed deltas and whether it scrolled lapis's
  `history` or went to the `program`. `pointer`: the pointer's position and the
  item under it, at most one record per `pointerSampleMs` while it moves; the
  latest position wins and `folded` counts the moves it stands for. `0` records
  no movement.
- `agent-focus`, `category`, `tiles`, `history`, `dialog`, `command`, `shortcut`
  (Command-` and the global Command-Option-L), `window` and `app`: the selected
  agent, category and tiles with how each changed (`via`: `click`, `key`,
  `shortcut`, `tab-away`, `notification`, `attention-key`, `terminal-key`,
  `wheel` or `program`), the history position, dialogs and menus opening and
  closing, the command run from the palette, window focus and app activation.

Passwords stay out. While macOS secure input is on, or the line the cursor is on
in the terminal that has the keyboard names a password, passphrase, passcode,
PIN or OTP (also one-time and verification codes, any case, as whole words) or
ends in `secret:`, `token:` or `key:`, typed characters, IME text and pastes are
recorded as `redacted` with only their class or length; keys such as Return
and Backspace keep their names. The rule reads the screen, so a prompt that
mentions a password redacts your typing there too. While the plan sign-in or
usage (accounts) dialog is open nothing at all is recorded; closing it leaves
one `dialog` record with the unrecorded milliseconds.

Each file rotates before it would pass `maxFileMiB` (1 to 1024): the current file
becomes `interaction.jsonl.1`, older ones move up, and only `maxFiles` files (1
to 64, the current one included) are kept, so the log never holds more than
`maxFileMiB` × `maxFiles`. Records are written in the background; if the disk
cannot keep up, records are dropped and a `dropped` record counts them, and
typing never waits. The log holds what you typed and pasted, so do not share it.

## Tab's next agent

`tabAway` sets how Tab, with nothing to type, picks the next agent that needs
you (see [suggestions](suggestions.md#tab)):

```json
"tabAway": {"rank": "learned", "work": ["work"]}
```

`rank` is `learned` (the default) or `fixed`. Learned starts from a prior (work
first, then requests, then turns you have not seen, then the newest) and fits it
to where you go: each time you settle on a waiting agent, by Tab or by hand.
`fixed` keeps the earlier order: a guess not yet seen, then an unseen turn or a
request, then a guess already seen, the longest waiting first. `work` lists the
categories, by name and without case, that count as work; the default matches a
category called `work`. Either way only agents truly waiting on you qualify: a
request, or a turn that finished with no background work in flight. Choices and
Tab moves are kept in `runtime/tab_away.jsonl` and the fitted model in
`runtime/tab_away_model.json`, both owner-only.

## The rest

- `editor` is the app that opens an agent's folder (else the first of Cursor, VS
  Code, Zed, Windsurf and Sublime Text installed).
- `keepAwake` keeps the Mac from sleeping while it is plugged in, so the phone
  can reach it.
- `usage`, `accounts` and `limitResets` are described in [usage](usage.md).
- `nextPrompt` turns on guessed next prompts; see [suggestions](suggestions.md).
- Key bindings are listed in [keys](keys.md); Appearance shows and edits them.
