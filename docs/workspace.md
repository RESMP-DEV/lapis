# Workspace

## The stage, the sidebar and categories

One terminal stage shows the selected agent. The sidebar beside it is the
navigation, as Chrome's vertical tabs are with tab groups: every category is a
colour-coded group: its name in its colour, with its agents listed under it,
each with its status mark and title, and a line in the colour down their rows. A
click on an agent shows it and its category; Command-Shift-] and [ walk the list
in order, on from a category's last agent into the next category's first, and
Command-1 to 9 still go to the first nine categories (a category's name shows
its shortcut on hover). The agent on the stage has a leading edge in its
category's colour and a faint fill of it, and the shown category's name is bold
in its full colour while the others' are quieter. Larger terminal text makes the sidebar's
names larger too. With nothing open, the stage lists what to do next (new
agent, resume, terminal, reopen, the latest conversations and the categories
that have agents), moved with up and down and run with Return, so nothing needs
the mouse.

Categories group agents and each remembers its selected agent. Each takes one of
eight colours (grey, blue, red, yellow, green, pink, purple, cyan) by its place
in the list until you choose one under **Colour** in its menu (a right-click on
its name); a chosen colour is kept with the category in `runtime/workspace.json`
in the lapis folder. A narrow
window, or Command-B, hides the sidebar and shows a category selector instead.

The strip of live previews under the stage is off unless you turn it on with
**Show agent previews** in Commands (or `previewsVisible` in
[config](config.md#the-rest)). It shows each of the category's agents as a
card with its latest lines (the rows ending at its cursor), refreshed at most
four times a second, in order; moving along it keeps part of the neighboring
card in view, like Neovim's `sidescrolloff`, and the last card is **+** for a
new agent. Previews never take input or resize a terminal.

Each card on the strip starts with a mark that says what the agent is doing,
and the mark grows with the text size. The sidebar shows only the rare marks
(diamond, filled square, dash), after the agent's title; a working, done or
opening agent's row is its title alone.

| Mark | Meaning |
| --- | --- |
| Filled dot, activity colour | Working |
| Hollow ring | Its turn is done (finished or idle) |
| Diamond, attention colour | Waiting on your answer to a request |
| Filled square, fault colour | Connection lost |
| Hollow square | Opening, or status unknown |
| Dash | Ended |

The mark never says whether you have looked at an agent: a finished agent looks
the same either way, and lapis draws no unseen dots, category counts or Dock
badge. Look at an agent when you want to. Attention never reorders categories
or takes the keyboard.

Keyboard focus, activity, pending requests and lost connections each have their
own color and shape. The Command theme uses dark opaque surfaces with restrained
teal accents, and the macOS title bar follows the theme while keeping the
standard window controls. One configurable fixed-width font serves the terminal
and machine readouts.

## Arranging

Agents in the sidebar drag onto another category's name to move there, and onto
the sidebar's **+** to start a new category; cards on the strip do the same and
also drag along it to reorder. Command-click and Shift-click pick several to drag
together. Categories drag up and down the sidebar. Commands and an agent's menu
(a right-click on its row or card) rename, reorder or move an agent without
restarting it.

## Commands

**Commands** (Command-Shift-P) searches every action with its shortcut. It holds
**New category**, the reload actions from [agents](agents.md), the agent menu's
actions and **Show agent previews**.

## Visual view

The visual view shows a Claude Code, Codex or Antigravity agent as a readable
thread instead of its terminal: your prompts as bubbles, its replies as prose,
and its work as one-line steps ("Ran", "Read", "Edited +4 −10", "Thought for
6s") that open on click; an edit's diff and a command's output show only once
its step is opened. It reads the
conversation the CLI saves on this Mac (Claude Code's transcript, Codex's
rollout under `~/.codex/sessions`, Antigravity's
`~/.gemini/antigravity-cli/brain/<conversation>/.system_generated/logs/transcript_full.jsonl`)
and follows it as the agent works; anything it does not recognize shows as a
plain block. Agents on other machines show their terminal only.

As in the Codex app, a finished turn folds to your prompt, one **Worked for
3m 12s** line, and the final reply. A turn a background task or subagent woke
shows only that line and the reply, with no "finished" notice. Clicking the
line opens the thinking and steps between, indented, under a summary of the
work ("ran 4 commands, edited 2 files, read 3 files") as one-line steps; each step then opens on its own to show its command output
or diff, and a step that took a second or more shows how long ("12s", "3m
4s", "1h 2m", "2d 3h").

While the agent works, the running turn is your prompt, a line such as
**Working · 2m 10s · 14 steps**, and under it one small box four lines tall:
the turn's steps as the CLI records them, one line each ("Read greet.cpp",
"Edited greet_test.cpp +12 −3", "Ran ctest 14s", "Thought for 6s", the
model's short notes between steps), the newest at the bottom and earlier ones
rolling up and out. The wheel or trackpad over the box scrolls back through
the turn without the box growing (past either end it scrolls the thread).
Clicking a step opens it under the box, as an opened "Worked for" step does,
and clicking it again closes it. The box shows only what the conversation
file holds, never the terminal's screen; for Claude Code a last line reads
"Thinking…" once the newest step is done. When the turn ends the box goes and
the turn folds as above. While the conversation compacts (a `/compact`, or
Claude Code's own when the context fills) the line reads **Compacting
conversation for 1m 3s** with nothing under it.

The model's thinking shows dimmed as it goes. While the newest step is a
thought, it fills the rest of the box with its last lines (the box does not
grow); an older thought is one dim line with its first sentence. In an opened
"Worked for" a thought is the same one line with how long it took, and it opens
to the whole thought as dim prose. A thought the CLI saved empty, or that is
only agy's stock "I'm thinking through how to approach this.", is a plain
**Thought** line that does not open; that stock line is also dropped from the
front of a real thought, and runs of blank lines fold to one. An opened step
(thought, command output, diff) sits a little in from the thread behind a thin
rule, with more room below it than above.

Subagents and background tasks are steps of their own. A subagent is one line,
**Subagent · Map the lexer · 4m 10s · 23 steps**, running (with a dot) or done;
clicking it opens how it went (its type, model, tokens), what it was asked, its
own steps as one-line rows in a box like the live box (each opens under it; a
subagent's own subagents stay lines) and its final report. Its steps are read
from its own conversation file only when it is opened. A background command or
monitor is one line too, **Started in background · npm test --watch**; the
agent's later looks at its output and its end attach to that line, which opens
to its status, exit code, run time, how often it was checked and its last lines
of output, rather than adding rows. The turn a finished task wakes still folds
to its own "Worked for" line with no "finished" row.

While any run, a strip under the live box lists them, one quiet line each:
**Subagent · Map the lexer · Explore / Sonnet 5.5 · 3m 12s · 14 steps**,
**Background · npm test --watch · 1m 5s**. It is at most four lines tall and
scrolls past that; with more than four it is one line, **3 subagents and 2
background tasks running**, that opens the list. A line opens its task under
the strip, and finished ones leave it. When the agent's turn has ended but its
work runs on, the thread ends in **Waiting on 2 subagents · 4m 2s** (counted
from the turn's end) and the strip, instead of looking finished.

A task counts as running until its end is recorded. One whose end never was
(the CLI quit) counts as ended, with "no end recorded", once its last sign of
life is an hour old: its start, a check or event in the conversation, or the
last write to its own file (a subagent's conversation, a command's output); a
monitor also stops at its own expiry. All of this is read from the files, so it
is the same after lapis restarts.

A prompt you send while the agent works shows at once under the box, marked
**Queued · the agent reads it at its next step**; several keep their order.
When the agent reads it mid-turn it moves into the thread where it was read,
above the box, marked **Steered · read after 3 more steps** (or just
**Steered** where the CLI does not say how long it waited), and the box
starts again under it with the steps that follow. The steps before it fold
into their own **Worked for** line, so a finished turn reads: prompt, Worked
for, steering prompt, Worked for, reply, each prompt shown once. A prompt the
CLI holds until the turn ends becomes the next turn's prompt, with no mark.

Above **Commands** in the sidebar, **This agent** switches the selected agent
between its terminal and the thread, and **All agents** shows every Claude
Code, Codex and Antigravity agent on this Mac as a thread (saved, so it
survives a restart). A per-agent choice wins over **All agents** until lapis
restarts or **All agents** is switched again. Both are also in Commands. The
terminal keeps running underneath but takes no input while covered; Escape
brings it back.

What differs by CLI:

- **Claude Code** saves a step when it is called and its result when it is
  done, so the box shows a running step (with a dot) until its result lands.
  Its own queue is in the file: a prompt sent mid-turn shows queued at once
  and then as that record, and its "read after N more steps" counts the tool calls
  between its sending and its reading. A subagent's steps are in
  `<session>/subagents/agent-<id>.jsonl` beside the conversation; its end,
  steps, tokens and report come in the task notification Claude Code writes
  when it finishes (SendMessage to it runs it again). A background command
  (`run_in_background`, or one moved there when it ran long) ends with its own
  notification and exit code; BashOutput, TaskOutput, KillShell, TaskStop and
  reading its output file attach to it. A monitor's events count as its checks.
- **Codex** saves each step when it is done, so the box gains a step as each
  one finishes (within milliseconds of it, in rollouts on this Mac). A message
  sent mid-turn is read at Codex's next step; lapis shows it queued until
  Codex records it and counts the steps between. Its reasoning shows only when Codex saves a
  summary (`model_reasoning_summary` not `none`). Its model and effort show in
  the toolbar without choices, since Codex's `/model` is a picker. A command
  still running when Codex's exec_command yields is a background task until
  the process exits (Codex writes the command's item then); its polls attach
  to it. Subagents come from Codex's collaboration items (spawn_agent, and the
  states a later wait or close reports), each opening to its own rollout; no
  rollout on this Mac has one yet, so this follows Codex 0.157.1's source. A new agent's
  thread appears with Codex's first saved message (before its first turn ends,
  lapis takes the conversation the agent's own Codex holds open, else the one
  started in the agent's folder since the agent started when no other agent
  there could have started it; see [after a reboot](agents.md#after-a-reboot)).
- **Antigravity** never tells lapis its conversation, so the thread is the
  one the agent's own agy holds open, pinned to the agent; it follows `/clear`
  and never shows another agent's conversation from the same folder. agy
  records no turn times, so "Worked for" counts from the
  prompt to the final reply. agy writes each step to its file as it
  completes, and reads a message sent mid-turn at its next step: input
  written within ten minutes of a tool step in a running turn counts as
  steering it (later input starts a new turn). As for Codex, lapis shows it
  queued until agy records it. The toolbar's model and effort come from the
  setting agy notes in a prompt, so a change from the menus shows from the
  next prompt; the menus send `/model <id>` and `/effort <level>`. A turn
  woken by a timer or a finished background task folds like one Claude Code's
  task notification woke. agy's checkpoint, which summarizes the steps before
  it, shows as a divider. invoke_subagent is a step per subagent it created,
  each opening to its own conversation in `brain/<id>`; agy records no end for
  one, so a subagent ends with its message to the agent, when manage_subagents
  lists it idle or kills it, or when its own conversation comes to rest. A
  command agy left running ("background task with task id") is a background
  task: manage_task checks attach to it and agy's "Task id ... finished"
  message ends it.
- For both, the dialog peek is off (it reads Claude Code's screen layout),
  a slash command sent from the bar that opens a
  picker or panel, or that lapis does not know, shows the terminal, and
  **Edit** on a prompt is not offered.

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

A prompt you send shows in the thread at once, in the same frame as Return,
and the bar clears: at the thread's end with the turn shown starting
(**Working**) when the agent was idle, or marked **Queued** under the box when
it was working. When the CLI records the prompt, its own row takes the
bubble's place. lapis sends the text and a Return together, then checks the
agent's input box 1.5 seconds later: if the box still holds the text, it sends
one more Return (never more, and only then, so a Return cannot land in a
dialog); if that does not take it either, the bubble says **Not sent yet ·
press Return in the terminal**. The note goes once the box no longer holds the
text. Prompts sent from the bar are never written to disk by lapis.

The bar's toolbar is the same for every CLI: **+** attaches files or a folder,
then the model and the effort (Low, Medium, High, Extra High, Max) the agent
last reported, each a menu that changes it by typing that CLI's own command
(`/model`, `/effort` for Claude Code and Antigravity), and a ring with the context window's use
written beside it ("21% · 206k / 1M"). Nothing in the view uses hover popups.
Files attach by **+**, by dropping them on the thread, or by pasting (Finder
files, or a screenshot saved as a PNG); each shows as a chip and is sent as an
`@path` mention, so the CLI reads images as images and any other file or folder
as it would from its own prompt. Backspace in an empty bar removes the last one.

Typing `/` and a few letters in a Claude Code agent's bar lists the three
closest slash commands (letters in order match, so `/rp` finds
`/reload-plugins`): Claude Code's own, then your commands and skills from
`~/.claude/commands`, `~/.claude/skills` and the agent folder's `.claude/`.
A Codex agent's bar lists Codex's own commands (Codex 0.157 reads no custom
prompt files; its skills are `$name` mentions, not commands). An Antigravity
agent's bar lists agy's own commands, then the skills it offers as commands:
the project's `.agents/skills` (from the agent's folder up to its repository
root) and your `~/.gemini/config/skills`. Neither list offers `/exit`, nor
Codex's `/archive` and `/delete`, which end the session. For these two CLIs a
command that opens a picker or panel (`/model`, `/resume`, `/status`) shows
the terminal in place of the thread, since the view cannot draw their dialogs;
one that runs as a turn (`/compact`, a skill) stays in the thread.
Up and Down pick, Return runs the command, Tab fills it in to add to, and
Escape closes the list. What a command prints shows under it in the thread
("/model", then "Set model to …"), as does a `!` shell command's output. One
that runs for a while with no turn of its own shows **Running /review for
40s** until it is done; `/compact` shows as
one row between turns, with **Compacting conversation** while it runs. A command that opens a dialog of the CLI's own (the pickers of `/model`
and `/effort` with nothing after them, `/config`, `/resume`, `/permissions`,
`/mcp` and the like) shows the terminal, with the keyboard, for as long as the
dialog is up, and the thread returns when it closes. The bar never types into
such a dialog: sending while one is open shows it instead and keeps your
message in the bar. A request the agent raises does not do this; it waits in
the terminal as before.

Code in replies is colored by its language with Tokyo Night's palette (its Day
variant on light themes), as are the commands steps ran. Tables get a quiet
grid and a bold header; quotes read quieter than the reply. Math in replies
(`$...$` and `\(...\)` in a sentence, `$$...$$` and `\[...\]` on their own) is
set as text: Greek letters and symbols, real superscripts and subscripts,
fractions as `(a + b)/(c + d)`, roots, and italic variables, with display
equations centred on their own line. Dollar amounts and shell variables such as
`$HOME` stay as written, as does anything in code. Math lapis cannot set well
(matrices, aligned equations, fractions three deep, unfamiliar commands)
shows its LaTeX source in the code style instead. Your own prompts show as you
typed them. Images, SVGs, GIFs
and charts a message names show inline at the column's width. A path that
stands alone as its own paragraph of a reply (bare, in backticks, or as a
Markdown image or link) is replaced by its picture, player or card right there,
between the paragraphs around it; a path inside a sentence, a list item or a
code block stays text, and its file shows under the reply instead (under your
prompt, for what you attached), never twice. A picture with transparency sits
on the thread's own background with no frame. A path to a file that does not
exist yet stays text until the reply is next drawn. Copying a reply still gives
the path as written. Click a picture to open it full size.

Every file the thread shows (a picture in place or under a reply, an SVG, a GIF,
a player, a file card, or what you attached to a prompt) can leave the window.
Drag it to Finder, Discord, Slack, Mail or a browser's upload to attach the file
itself; a picture carries its pixels too, for apps that take an image. A click
that does not move still opens it, and dragging along a player's line still
seeks. Right-click (or Control-click) it for a menu: **Open**, **Show in
Finder**, **Copy Image** for a picture (the image and its file, so it pastes as
a picture into a chat and as a file into Finder) or **Copy File** for anything
else, **Copy Path**, and **Save As…** for a copy elsewhere; a picture also has
**Open With**, the apps Command-click opens paths in. None of these change the
file.
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
and no colors or hidden characters come along. A selection that covers a whole
equation copies its LaTeX source; part of one copies what is shown. Links underline under the
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

## Resuming

Command-O resumes a past Claude Code or Codex conversation from this Mac,
newest first under its CLI's own title, narrowed as you type.

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
