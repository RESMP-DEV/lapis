# Keys

These are the macOS defaults. Linux uses Control-Shift in place of Command.
Every binding can be changed in [config](config.md), and Appearance lists them
all. Terminal Control chords stay with the agent.

## Agents and categories

| Keys | Does |
| --- | --- |
| Command-T | New agent |
| Command-N | New category |
| Command-W | Close what is in front: the side terminal's panel, else the agent, else the window when the category is empty |
| Command-Shift-T | Reopen the last agent you closed, resuming its conversation |
| Command-O | Resume a past Claude Code or Codex conversation |
| Command-1 to 9 | Go to one of the first nine categories |
| Command-Shift-J / K, Command-Shift-Up / Down, Command-Option-Left / Right | Next or previous category |
| Command-Shift-[ / ] | Previous or next agent in the category; with tiles, see below |
| Command-J | The next agent, in any category, that needs you |
| Command-L | The agent that most recently needed you; again for the one before |
| Command-Option-L | The same, from any app, bringing lapis to the front |
| Command-K | Find an agent by name, folder, category, CLI, machine or screen text |

## Stage and tiles

| Keys | Does |
| --- | --- |
| Command-D | Start an agent like the selected one, tiled to the right |
| Command-Shift-D | The same, tiled below |
| Command-Control-arrows | Move to the tile on that side; it stops at the stage's edge |
| Command-Shift-Return | Fill the stage with the selected tile |
| Command-` or Control-` | Show or hide the side terminal |
| Command-~ | Pick the side terminal's machine |
| Command-B | Hide or show the category sidebar |

With tiles on the stage, Command-Control-arrows pick the nearest tile that shares
part of the selected tile's height (for left and right) or width (for up and
down). Command-Shift-[ and ] visit the tiles first, top row before bottom and
left before right, then the category's other agents in strip order. Those are
shown one after another in the tile you left, and the stage goes back to how it
was when the keys come round to the tiles again. Clicking a strip card that is
not tiled still puts it in the selected tile for good.

## Terminal

| Keys | Does |
| --- | --- |
| Command-F | Find text on the screen, then in history (Return older, Shift-Return newer) |
| Command-C / Command-V | Copy the selection / paste; selecting text (a drag or a double-click) copies it too |
| Command-plus, minus, zero | Text size bigger, smaller, reset |
| Command-Left / Right | Start or end of the line |
| Command-Backspace / Command-Delete | Delete to the line's start / end |
| Command-hover, Command-click | Show, then open, a link, file or folder |

## Window

| Keys | Does |
| --- | --- |
| Command-Shift-P | Commands |
| Command-comma | Appearance |
| Command-R | Reload `lapis.json` |
| Command-M | Minimize |
| Command-Shift-W | Close the window (lapis keeps running) |
| Command-Q | Quit, leaving agents running |
