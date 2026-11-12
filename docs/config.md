# Config

Settings live in `lapis.json`: `~/.lapis/lapis.json` for the app, the checkout's
`lapis.json` for a development build. It applies as soon as it changes, whether
Settings (Command-comma), you or an agent edits it, and Command-R reloads it by
hand. Besides key bindings and appearance it holds these:

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
  "alerts": {"finished": true},
  "keepAwake": true
}
```

## New agents

`newAgent` holds the defaults for the new-agent form: the CLI, the starting
folder, the approval mode (`edits`, `auto` or `full`), models to offer, and, per
machine in `machines`, a folder that differs. See [agents](agents.md). A CLI's
list in `models` is shown as written, in that order, and its first model is
preselected and passed to the CLI as its model. The same list replaces the
CLI's in the visual view's model menu; a name the CLI also lists keeps the
CLI's name for it and its effort levels (Antigravity's `gemini-4-hydrogen-high`
and `gemini-4-hydrogen-low` still show as one Gemini 4 Hydrogen with those two
levels), and any other name shows as written with no levels of its own.

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

New agents of a listed CLI start without running its update first. An update
already running when you add the key still finishes
before its waiting agents start. Every CLI that is absent or set to `true` keeps
updating as before; `--no-harness-updates` still turns off updates for all of
them. See [keeping CLIs current](agents.md#keeping-clis-current).

## Scrolling

Typing over a full-screen agent that you scrolled back returns it to the
bottom of its own view before the key is delivered. That is
`scrollToBottomOnTyping`, on by default; set it to `false` to leave a
scrolled-back program where it is:

```json
"scrollToBottomOnTyping": false
```

The wheel still reaches a full-screen program either way. See
[history](workspace.md#history).

## Visual view

Settings writes the [visual view](workspace.md#visual-view)'s choices under
`visual`: `theme` (`terminal`, `tokyo-night`, `codex`, `claude`, `graphite`,
`black`, `claude-light`, `codex-light`), `font` (`system`, `serif`, `avenir`,
`helvetica`), `codeFont` (`system`, `terminal`, `menlo`), `size` in pixels
(10 to 40; 0 follows the terminal font), `all`, which shows every Claude
Code agent as a thread, and `editor`, what Command-click on a file path opens
it in (`vscode`, `cursor`, `zed`, `textedit`, `system`, or `auto` for the first
of those installed). An unknown name falls back to the first choice.
`"factory": true` opens the [factory view](workspace.md#factory-view); it is
written only while the view is on.

```json
"visual": {"theme": "terminal", "font": "system", "codeFont": "system", "size": 0, "all": false, "editor": "auto"}
```

Light mode lives there too. `appearance` is `dark`, `light` or `system`
(follows macOS as it changes). It picks the thread's theme from `darkTheme`
or `lightTheme` and the window's from `darkWindowTheme` or `lightWindowTheme`,
each your last choice of that kind; terminals keep their own colors. A
thread theme is light when its name ends in `-light`; of the window themes,
`daylight` is light. Choosing a theme of the other kind switches the mode to
that kind. `theme` and the top-level `theme` record what shows now. A file
without `appearance` shows what it did before, as dark or light by its thread
theme.

```json
"visual": {"appearance": "system", "darkTheme": "terminal", "lightTheme": "claude-light", "darkWindowTheme": "oled", "lightWindowTheme": "daylight"}
```

Two hand-written keys size the threads the visual view keeps read so switching
agents is instant: `keepMB`, about how much memory they may take besides the
shown one (default 1024, 0 keeps none), and `keepThreads`, how many (default
32). The least recently shown goes first; the shown thread is always kept.
Twenty conversations of 9 to 358 MB, all kept, measured 0.5 to 0.8 GB.

```json
"visual": {"keepMB": 1024, "keepThreads": 32}
```

## Alerts

A completed turn or request you were not looking at plays one quiet cue when
`alerts.finished` is enabled. Requests use the same cue, and the cue does not
repeat: it plays once per turn or request, when the agent becomes unseen, and
never again for a turn you have looked at. You are looking at an agent while it
is on the stage, lapis is in front and you are at the Mac (some keyboard, mouse
or trackpad input within two minutes); showing it by any path, bringing lapis
to the front or coming back to the Mac with it on the stage counts. Settings
has one switch and Play button for it. lapis posts no macOS notifications.
Keys of retired alerts are ignored, and older files may keep
them: `alerts.sound` and `alerts.repeat` (the repeating request chime),
`alerts.notify`, `alerts.awayAfter` and `alerts.remindAfter` (background
notifications and their reminders), and `alerts.judge` (a model's judgement of
whether a finished turn needed you, which kept the others quiet).

A turn that ends on what you already saw of that agent stays quiet: while lapis
is in front and you are at the Mac (some keyboard, mouse or trackpad input
within two minutes), the screen of the agent you are looking at is noted every
second, and a finished turn whose screen above Claude Code's input box is
unchanged does not chime.

Every decision, a chime or its quiet reason, is logged, one JSON line each, to
`runtime/attention.jsonl` in the lapis folder: the moment, the agent's
conversation title and id, its CLI, the event (`finished`), a `kind` of chime,
what the turn did to the agent's `mark` (`marked`, `looking`, `waiting`,
`unchanged` or `repeat`) and the `decision` (chimed, or the quiet reason). Each
unseen mark set or cleared adds a line of `kind` `mark` with the agent's id
only, its status, the event (`unseen` or `seen`) and `why` (`finished`,
`request`, `quiet`, `away`, `restored`; `focus`, `window`, `present`, or
`looking: …` for a turn seen as it ended). The log is owner-only, and the title
can be the conversation's first prompt, so the file is not for sharing. Before a line
would cross 2 MiB the log rotates to `attention.jsonl.1` (one predecessor, the
older one removed) and the fresh file opens with a `rotated` marker line.

Use `alerts.finishedFile` for your own sound. When it is unset, `alerts.soundFile`
is used at half volume. An explicitly set but unavailable `finishedFile` falls
back to the built-in finished cue rather than borrowing a different file.

```json
"alerts": {"finishedFile": "~/.lapis/sounds/done.wav", "finished": true}
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
  `area` such as `stage`, `sidebar` (an agent's row), `strip`, `side-terminal`,
  `categories` or `dialog`,
  and the agent's session id where there is one). `wheel`: one record per scroll
  gesture per interval with summed deltas and whether it scrolled lapis's
  `history` or went to the `program`. `pointer`: the pointer's position and the
  item under it, at most one record per `pointerSampleMs` while it moves; the
  latest position wins and `folded` counts the moves it stands for. `0` records
  no movement.
- `agent-focus`, `category`, `history`, `dialog`, `command`, `shortcut`
  (``Command-` ``), `window` and `app`: the selected agent and category with how
  each changed (`via`: `click`, `key`, `shortcut`, `tab-away`, `terminal-key`,
  `wheel` or `program`), the history position, dialogs and menus opening and
  closing, the command run from the palette, window focus and app activation.

Passwords stay out. While macOS secure input is on, or the line the cursor is on
in the terminal that has the keyboard names a password, passphrase, passcode,
PIN or OTP (also one-time and verification codes, any case, as whole words) or
ends in `secret:`, `token:` or `key:`, typed characters, IME text and pastes are
recorded as `redacted` with only their class or length; keys such as Return
and Backspace keep their names. The rule reads the screen, so a prompt that
mentions a password redacts your typing there too. While a dialog whose name
mentions a sign-in, account, credential or password is open nothing at all is
recorded; closing it leaves one `dialog` record with the unrecorded
milliseconds.

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
"tabAway": {"work": ["work"], "overdueHours": 4}
```

Tab's order does not learn: an agent that needs you (a request, an unseen turn
or an unseen guess) and waits more than `overdueHours` (0 turns this off) comes
first, oldest first, so neglected work cannot sink; then work categories,
requests, turns you have not seen, and the newest. `work`
lists the categories, by name and without case, that count as work; the
default matches a category called `work`. The retired `rank` key chose between
this order and a `learned` or `fixed` one; a learned order was the default
until October 11, when the log showed it mostly learned the order of your
Command-number walk (597 jumps against 24 Tab moves in five days) rather than
which agent needed you. Older files may keep `rank`; it is ignored and left as
written.

Only agents truly waiting on you qualify: a request, or a turn that finished
with no background work in flight, unseen or with a guess you have not seen, and
not parked; a turn you looked at, its guess included, waits no longer. Looking at a
finished turn for 2.5 seconds or more and moving on without a prompt parks the
agent: Tab passes it over until it next begins to wait. A request
is never parked. Choices and Tab moves are kept in `runtime/tab_away.jsonl`,
owner-only. A `runtime/tab_away_model.json` left by an older build is no
longer read or written.

## The rest

- `keepAwake` keeps the Mac from sleeping while it is plugged in, so the phone
  can reach it.
- `sidebarVisible` (on unless set) shows the sidebar of categories and their
  agents; Command-B switches it. `previewsVisible` (off unless set) shows the
  strip of live previews under the stage; **Show agent previews** in Commands
  switches it. A file that already sets either keeps its value, and lapis
  writes both whenever it saves the file.
- `usage` (and the older top-level `showUsage`), `accounts` and `limitResets`
  belonged to retired plan features (the usage meter, lapis-managed plans and
  limit resets); a file that still has them loads, and lapis leaves them as
  written. Each CLI uses the sign-in its machine already has.
- `nextPrompt` turns on guessed next prompts; see [suggestions](suggestions.md).
- Key bindings are listed in [keys](keys.md); Settings lists them under Keys.

## Settings

Command-comma (or **Settings** in Commands, which also answers to
"Appearance") opens Settings: General, Appearance, Reading view, Terminal, New
agents, Keys and Advanced, one at a time, with a search across all of them.
Each change applies at once and is written here; a row that differs from the
built-in value has **Reset**, and each section has a Reset for all of its rows.
Besides the keys above, it writes these, and only the section it changed, so
the rest of that section (machines, models, providers) stays as written:

- `newAgent.harness`, `newAgent.folder` and `newAgent.mode` (an empty value
  removes the key);
- `harnessUpdates.<cli>` (turning updates back on removes the key);
- `nextPrompt.auto`, `interactionLog.enabled` and `tabAway.overdueHours`
  (0 to 168);
- `sidebarVisible`, `previewsVisible` and `scrollToBottomOnTyping`.

Key bindings, models per CLI, folders per machine, `harnessArguments`, the
chime's sound files, the next-prompt model, providers and limits, the
interaction log's sizes and Tab's work categories are only in this file:
lists, paths and endpoints read better as JSON than as controls. Advanced
opens it in its default app.

## Window state across restarts

The agents run in their session services, so a restart of the window (an
install swaps the app and opens it again) leaves them running. What only the
window knew is kept in `runtime/gui_state.json` in the lapis folder and comes
back with it: the guessed next prompts at each agent's cursor (whether you saw
them, a guess Tab typed in, a guess still being made), which agents have a turn
or request you have not looked at (for Tab and the chime; not drawn) and when
each began to need you (Tab's order); a turn that ended while no window ran is
marked once,
what you last saw of each agent (so a turn ending on it
stays quiet), the agents Command-Shift-T can reopen, and the new-agent form's
last choices and the side terminal.

The file is private to you (`0600`), versioned, replaced whole on each write
and at most 1 MiB. Writes are at most one every two seconds, with the newest
state, and finish when lapis quits. A missing, unreadable, corrupt, oversized or
other-version file is ignored with a warning. Nothing restored pings or acts on
an agent: marks for agents that are gone or now in another conversation (after
`/clear`) are dropped, a guess shows again only while its conversation is
still at the turn it was made for, rejected entries are pruned from the saved
state, and the new-agent form and side-terminal machines are checked against the
available choices before they are recalled. Delete the file to start the window
fresh.
