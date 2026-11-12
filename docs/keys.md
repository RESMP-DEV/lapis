# Keys

These are the macOS defaults. Linux uses Control-Shift in place of Command.
Every binding can be changed in [config](config.md), and Settings lists them
all under Keys. Terminal Control chords stay with the agent.

## Agents and categories

| Keys | Does |
| --- | --- |
| Command-T | New agent |
| Command-N | New category |
| Command-W | Close what is in front: the side terminal's panel, else the agent, else the window when the category is empty |
| Command-Shift-T | Reopen the last agent you closed, resuming its conversation |
| Command-O | Resume a past Claude Code or Codex conversation |
| Command-1 to 9 | Go to one of the first nine categories |
| Command-] / [, Command-Shift-J / K, Command-Shift-Up / Down, Command-Option-Left / Right | Next or previous category |
| Command-Shift-[ / ] | Previous or next agent in the sidebar's order, on into the neighbouring category (empty ones are passed over) and around |

## Stage

| Keys | Does |
| --- | --- |
| Command-` or Control-` | Show or hide the side terminal |
| Command-~ | Pick the side terminal's machine |
| Command-B | Hide or show the sidebar of categories and their agents |
| Command-Shift-F | Switch between the [factory view](workspace.md#factory-view) and categories |

## Terminal

| Keys | Does |
| --- | --- |
| Command-C / Command-V | Copy the selection / paste; selecting text (a drag or a double-click) copies it too |
| Command-plus, minus, zero | Text size bigger, smaller, reset |
| Command-Left / Right | Start or end of the line |
| Command-Backspace / Command-Delete | Delete to the line's start / end |
| Command-hover, Command-click | Show, then open, a link, file or folder |

## Visual view

These are the thread's own keys and are not configurable. See
[the visual view](workspace.md#visual-view) for what each CLI does with them.

| Keys | Does |
| --- | --- |
| Return | Send; while the agent works, queue (Claude Code, agy) or steer (Codex) |
| Command-Return | While the agent works, send now: Claude Code reads it within the turn, agy at its next step, and Codex interrupts and sends it as a new turn; with nothing typed, the same for the prompts already waiting; when the agent is idle, send |
| Shift-Return | New line |
| Escape | With nothing typed while the agent works, stop the turn; otherwise show the terminal |
| Tab | Take lapis's suggestion; again, with it unchanged, send it; on an empty bar, the next agent that needs you, its bar ready for Tab again. A held Tab repeats nothing |

## Window

| Keys | Does |
| --- | --- |
| Command-Shift-P | Commands |
| Command-comma | Settings |
| Command-R | Reload `lapis.json` |
| Command-M | Minimize |
| Command-Shift-W | Close the window (lapis keeps running) |
| Command-Q | Quit, leaving agents running |
