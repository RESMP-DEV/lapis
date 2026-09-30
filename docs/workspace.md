# Workspace

## The stage, the strip and categories

One terminal stage shows the selected agent. Under it, the strip shows each of
the category's agents as a card with its latest lines (the rows ending at its
cursor), refreshed at most four times a second, in tab order. There is no tab
row: the strip is how you move through a category. Moving along it keeps part of
the neighboring card in view, like Neovim's `sidescrolloff`, and the last card is
**+** for a new agent. With nothing open, the stage lists what to do next (new
agent, resume, terminal, reopen, the latest conversations and the categories
that have agents), moved with up and down and run with Return, so nothing needs
the mouse.

Categories group agents and each remembers its selected agent. The category
rail and the strip are separate levels of navigation; a narrow window shows a
category selector instead of the rail. A card whose agent finished a turn or
started needing you while another was selected pulses until you select it, and
its category shows a pulsing dot. The Dock badge counts agents, in any category,
that need you and have not been looked at. Attention never reorders categories
or takes the keyboard.

Keyboard focus, activity, pending requests and lost connections each have their
own color and shape. The Command theme uses dark opaque surfaces with restrained
teal accents, and the macOS title bar follows the theme while keeping the
standard window controls. One configurable fixed-width font serves the terminal
and machine readouts.

## Tiles

Drag a card onto an edge of the stage to tile that agent beside the one shown,
as iTerm2 splits a tab, or onto a tile's middle to swap it in. Dividers drag to
share the space; agents are resized once, when the drag ends. Command-D starts an
agent like the selected one (folder, CLI, model and mode) tiled to the right,
Command-Shift-D below it. Command-Control-arrows move between tiles,
Command-Shift-Return fills the stage with one, and a tile's ✕ or a drag back to
the strip takes it off the stage while it keeps running. A strip agent that is
not tiled takes the selected tile when you click it. Each category keeps its
tiles.

## Arranging

Cards drag along the strip to reorder, onto a category in the rail to move
there, and onto the rail's **+** to start a new category; Command-click and
Shift-click pick several to drag together. Categories drag up and down the rail.
Commands and a card's menu rename, reorder or move an agent without restarting
it.

## Commands

**Commands** (Command-Shift-P) searches every action with its shortcut. It holds
**New category**, the reload and update actions from [agents](agents.md), and
the card actions: show an agent's folder in Finder, open it in your editor
(`editor` in [config](config.md), else the first of Cursor, VS Code, Zed,
Windsurf and Sublime Text installed), or copy its path.

## Finding

Command-K finds an agent as you type: by the letters of its name, folder,
category, CLI or machine, or by text on its screen; Return shows it. Command-F
finds text in the selected terminal: the screen first, then older history a
screen at a time (Return goes older, Shift-Return newer). Command-O resumes a
past Claude Code or Codex conversation from this Mac, newest first under its
CLI's own title, narrowed as you type.

## The side terminal

Command-` (or Control-`, as in VS Code) shows a plain shell over the stage's
right half for a quick command, and hides it again. Command-~ picks its machine:
this Mac or an ssh host. It is never an agent: one shell per machine, in no
category and with no alerts, which keeps running when lapis closes and ends when
you type `exit`.

## In the terminal

Terminal text defaults to 14 pixels. Appearance takes a size from 10 to 32 with
plus, minus and reset, and Command-plus, minus and zero do the same.

Drag to select text or double-click a word; Command-C copies it and typing
clears it. Files dropped on a terminal paste their quoted paths. Command-Left and
Right move to the start or end of the line, Command-Backspace deletes to the
line start and Command-Delete to its end (Control-A, E, U and K reach the agent
as they are).

Hold Command over a web link or a file or folder the agent names: it is
underlined, the pointer becomes a hand and the destination shows on hover.
Command-click opens it as the Finder would: a link in the browser, an image in
Preview, a folder in the Finder (an app or a program is shown in its folder,
not run). Paths count from the agent's folder, `~` and `/` included, and
`file.cpp:12` finds the file. An agent over ssh prints paths on another machine,
so only its web links open. Visible `http(s)` addresses, `www.` addresses and
the CLI's own OSC 8 links (including labels whose destination is hidden) work
across wrapped rows and history; OSC 8 opens `http(s)` and local `file:` links
only. An agent whose service predates labeled links still gets visible links
until it restarts.

## History

Everything an agent prints is kept, compressed, back to its first line. The
mouse wheel scrolls back through it a row at a time, and a bar appears down the
terminal's right edge: drag it to jump anywhere, the top being the first line.
Scrolling past the newest row, typing, or **Live** returns to the live screen,
and a key you type reaches the agent. Over a full-screen program on the
alternate screen, the wheel scrolls the program itself, as in other terminals.
History actions are also under **Agent**.
