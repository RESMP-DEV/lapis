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

## Visual view

The visual view shows a Claude Code agent as a readable thread instead of its
terminal: your prompts as bubbles, its replies as prose, and its work as
one-line steps ("Ran", "Read", "Edited +4 −10", "Thought for 6s") that open on
click, with each edit's diff shown under it. It reads the conversation Claude
Code saves on this Mac and follows it as the agent works; anything it does not
recognize shows as a plain block.

As in the Codex app, a finished turn folds to your prompt, one **Worked for
3m 12s** line that sums up the work ("ran 4 commands, edited 2 files, read 3
files"), and the final reply. Clicking that line opens the thinking and steps
between, indented; each step then opens on its own to show its command output
or diff, and a step that took a second or more shows how long ("12s", "3m
4s", "1h 2m", "2d 3h"). A turn still running shows every step as it happens,
then **Working for 1m 12s** and, under it, the agent's own screen above its
input box as it streams (thinking, tool output, the reply being written),
dim and slightly soft; click it to read it all sharp. The CLI saves each
block only once it is complete, so this live part comes from the terminal and
gives way to the saved steps as they land. A prompt you send while the agent
works shows at once, marked queued until the agent takes it in, and stays in
view when its turn folds.

Above **Commands** in the sidebar, **This agent** switches the selected agent
between its terminal and the thread, and **All agents** shows every Claude Code
agent on this Mac as a thread (saved, so it survives a restart). A per-agent
choice wins over **All agents** until lapis restarts or **All agents** is
switched again. Both are also in Commands. The terminal keeps running underneath
but takes no input while covered; Escape brings it back.

The bar at the bottom types to the agent: Return (or Command-Return) sends and
Shift-Return starts a new line; it is two lines tall at rest and grows to fit
what you type, and lapis's guess shows there in full. It edits as the terminal does on a Mac: Command-Left and
-Right go to the line's start and end, Option-Left and -Right move by word,
Command-Backspace and Option-Backspace delete to the line start and the word
start, Control-A, E, K, U and W work as in the shell, and Shift extends the
selection. While the agent works, the send button is a stop button (■) that
interrupts the turn, as Escape does in the terminal; Return still sends, and
the message waits in the agent's queue. lapis's guess at your next prompt shows in the empty bar; Tab puts it
in the bar to edit, and Tab again sends it. Tab with nothing typed or offered
moves to the next agent that needs you, as in the terminal. While the agent is
waiting on a request, the bar says so and sends nothing; answer it in the
terminal (Escape). Typing while reading goes to the bar.

The bar's toolbar is the same for every CLI: **+** attaches files or a folder,
then the model and the effort (Low, Medium, High, Extra High, Max) the agent
last reported, each a menu that changes it by typing that CLI's own command
(`/model`, `/effort` for Claude Code), and a ring with the context window's use
written beside it ("21% · 206k / 1M"). Nothing in the view uses hover popups.
Files attach by **+**, by dropping them on the thread, or by pasting (Finder
files, or a screenshot saved as a PNG); each shows as a chip and is sent as an
`@path` mention, so the CLI reads images as images and any other file or folder
as it would from its own prompt. Backspace in an empty bar removes the last one.

Typing `/` and a few letters in a Claude Code agent's bar lists the three
closest slash commands (letters in order match, so `/rp` finds
`/reload-plugins`): Claude Code's own, then your commands and skills from
`~/.claude/commands`, `~/.claude/skills` and the agent folder's `.claude/`.
Up and Down pick, Return runs the command, Tab fills it in to add to, and
Escape closes the list. What a command prints shows under it in the thread
("/model", then "Set model to …"), as does a `!` shell command's output. One
that runs for a while with no turn of its own, such as `/compact`, shows
**Running /compact for 40s** with the agent's screen under it until it is
done. A command that opens a dialog of the CLI's own (the pickers of `/model`
and `/effort` with nothing after them, `/config`, `/resume`, `/permissions`,
`/mcp` and the like) shows the terminal, with the keyboard, for as long as the
dialog is up, and the thread returns when it closes. The bar never types into
such a dialog: sending while one is open shows it instead and keeps your
message in the bar. A request the agent raises does not do this; it waits in
the terminal as before.

Code in replies is colored by its language with Tokyo Night's palette (its Day
variant on light themes), as are the commands steps ran. Tables get a quiet
grid and a bold header; quotes read quieter than the reply. Images, SVGs, GIFs
and charts a message names show inline at the column's width, under the reply
(or under your prompt, for what you attached); click one to open it full size.
Audio (MP3, WAV, M4A, AAC, FLAC, AIFF) and video (MP4, MOV, M4V) play in place
through macOS's own player: a video shows its first frame with a play button,
and its controls (time, a line to seek along, ↗ to open it in its app) appear
under the pointer; audio is a card with a play button and the same line.
Formats macOS cannot play (WebM, MKV, Ogg, Opus), HTML and PDF show as a card
that opens them in their app. Charts an agent draws with matplotlib
match the view's theme: lapis writes a style from it to `~/.lapis/visual` and
points local agents' `MATPLOTLIBRC` there (unless you set your own), with the
colors also in `palette.json` (`LAPIS_CHART_PALETTE`) for other plotting code.
Agents launched before the first style was written pick it up when reloaded.

Text in the thread selects with the mouse and copies with Command-C as plain
text that pastes cleanly anywhere: paragraphs, bullets and numbers are kept,
and no colors or hidden characters come along. Links underline under the
pointer. Command-click a file path anywhere in the thread (absolute, `~`, or
relative to the agent's folder, with any `:line` after it) to open it in your
editor at that line, or a folder in the Finder; Appearance's **Open files in**
picks VS Code, Cursor, Zed, TextEdit or the default app, from those installed
(the first installed of VS Code, Cursor and Zed until you choose). Hovering a message shows **Copy** (on prompts, replies and every code
block or output) and, on a prompt, **Edit**: a new agent that continues from
just before it, with the prompt waiting unsent in its input. The original
conversation is never changed.

Appearance has a **Visual view** section: a theme (Match terminal, Tokyo Night,
Codex, Claude, Graphite, Black, Claude light, Codex light), a reading font
(System, Serif, Avenir Next, Helvetica Neue), a code font and a text size, which
follows the terminal font unless set. Agents on other machines and other CLIs
are not supported yet.

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
mouse wheel scrolls back through it three rows a notch; a trackpad moves it by
pixels, so the text follows your fingers. A bar appears down the
terminal's right edge: drag it to jump anywhere, the top being the first line.
Scrolling past the newest row, typing, or **Live** returns to the live screen,
and a key you type reaches the agent. Over a full-screen program on the
alternate screen, the wheel scrolls the program itself, as in other terminals,
with a trackpad sending one wheel step per row it moves; typing first sends the program back to its bottom, by wheel or arrow keys,
before the key arrives. Reading lapis's own history still returns to live on
the first key, since that is where input is delivered. Set
`scrollToBottomOnTyping` to `false` in `lapis.json`
([config](config.md#scrolling)) to leave a scrolled-back full-screen program
where it is. History actions are also under **Agent**.
