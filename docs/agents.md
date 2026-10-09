# Agents

## Starting one

**New agent** (Command-T) asks three things in turn:

1. **The CLI.** Claude, Codex, OpenCode, Grok, OMP, Antigravity and Kimi appear
   in that order; one that is not installed is marked unavailable. Arrows and
   Return pick one; Escape goes back from the folder step.
2. **The folder.** The field starts at your home folder; arrows pick a
   suggestion and Tab or Return completes it, and the browse button opens the
   native picker. Folder lists put the ten folders with the most recent and
   frequent agent work first (work in a folder below counts for it), then the
   rest by name, with `_folders` last.
3. **The model and approval mode**, as chips under the folder. Models come from
   the CLI itself for your account (its default first; Kimi, OpenCode and OMP
   from their config, recent models and roles). The approval mode is **Accept
   edits**, **Auto** or **Full access**, each passed as that CLI's own flag: Full
   access the first time, or `newAgent.mode` from [config](config.md). A CLI
   without the chosen mode (OMP, OpenCode and Antigravity have no Auto, Kimi and
   OpenCode no Accept edits) uses the nearest supported mode with less access,
   or adds no mode flag when none exists.

An agent asked for without a mode (from the phone, or a resumed conversation)
uses `newAgent.mode`, or Full access when that setting is absent. If the CLI
cannot offer a configured mode, lapis can choose a less-permissive mode but does
not automatically grant a higher one; unknown preferences add no mode flag.
Explicit request modes win. Claude Code 2.1.283–2.1.285 changed no-mode defaults
across providers and entry points, so lapis passes the selected supported mode,
unless your `harnessArguments` already choose a mode. For Codex this includes an
explicit `--sandbox`, so a configured `--sandbox read-only` is preserved when a
request omits its mode. Literal arguments after `--` do not choose launch modes
or replace the generated conversation-resume argument.

The next agent starts with the same CLI, mode and model. Everything else stays in
each CLI's own config. lapis adds no flags of its own; to add yours to every new
agent of a CLI, set `harnessArguments` (see [config](config.md)). Shell aliases
do not apply, because lapis starts the executable directly.

The form's **Machine** row offers this Mac and the hosts in your ssh config (left
and right arrows, while up and down choose the CLI). On another machine the agent
runs over ssh in that machine's `newAgent` folder, else its home, in its login
shell. When such a Claude Code agent's connection drops (the Mac changed
networks or slept), lapis reconnects it and it resumes its conversation. The
The agent strip's **+** opens the same form, machines included. The category
rail's **+** creates a category instead.

A new tab shows the CLI's mark and a home-relative folder such as `~/dev/lapis`;
agents in the same folder are numbered. An agent still named after its folder
takes its conversation's title: Claude Code's own title, Codex's thread name,
else the first message typed. A name you choose stays.

Every agent gets its own session service, identity and endpoint. Agents always
start as top-level sessions: if lapis itself was opened from inside another
agent's terminal, that agent's session markers (Claude Code's child-session and
transcript flags, and Grok, OpenCode, OMP and Codex sandbox markers) are removed
first. No approval settings or global hooks are changed; a Claude agent's hooks
are passed to that one process by its service. An agent on another machine
gets its hooks (Claude Code) or notify program (Codex) on its command line, and
reports its turns through its own terminal, so its finished turns ping and get
suggestions as a local agent's do; that needs `python3` there.

## What lapis knows about an agent

Codex and Claude Code report their turns: Codex through its observer, Claude
Code through the session service's hook adapter, which also reports permission
prompts and questions. Other CLIs show an estimate from their output, **Output
active** or **Quiet**, rather than guessing at turns. **Turn finished** means a
turn ended, not that the task or the process did. A new Codex agent reads **No
prompt yet** until its first turn. Claude Code's idle reminder after a turn is
not a request.

A request (a permission prompt, a question) pings once and marks the agent just
as a finished turn does. Answer it in the agent's terminal; lapis never changes
a CLI's approval policy. For Codex agents, **Requests** (and **Review requests**
in Commands) lists what the agent is asking; select one, then explicitly
approve, decline, cancel or send answers. Opening or selecting never approves,
and a stale source disables the buttons. Unsupported Codex builds keep explicit
limits on status and responses; there is no way around qualification.

## Full-screen CLIs

lapis runs Claude Code and Grok full screen (Claude Code through
`CLAUDE_CODE_NO_FLICKER=1`, also over ssh, and Grok with `--fullscreen`); Codex
and OpenCode are full screen already. A full-screen program repaints when its
terminal is resized, where a classic renderer can leave a torn prompt. Set
`CLAUDE_CODE_NO_FLICKER=0` in your login shell to keep Claude Code's classic
renderer. Every CLI starts at the stage's size.

A full-screen program repaints its whole screen for every scroll step and
streamed line. Claude Code brackets each repaint in a synchronized update (DEC
mode 2026); lapis shows only the finished frame, as iTerm2 and Ghostty do, so
scrolling never shows a screen half painted. An update that never ends is
shown after 250 ms.

## Keeping CLIs current

A new agent's CLI updates itself first (`claude update`, `omp update`, `grok
update`, `kimi upgrade`, `opencode upgrade`, `agy update`), at most every 30
minutes per CLI. The card reads **Updating Claude…** until the agent starts on
the new version, and results go to `runtime/harness-updates.log`. Codex is the
exception: lapis observes only Codex builds it has qualified, so it keeps that
build and starts Codex with its update prompt off
(`check_for_update_on_startup=false`). Starting a supported CLI explicitly
uses the same update queue; reconnecting and discovering agents do not. Pass
`--no-harness-updates` to keep every installation unchanged. Queued agents
wait until the updater and its installer have stopped, and restarting a queued
agent cannot skip that wait.

To keep one CLI on a version you chose, install that version and turn off its
updates in `lapis.json` ([config](config.md#cli-updates)). For OMP:

```sh
bun add -g @oh-my-pi/pi-coding-agent@<version>
```

```json
"harnessUpdates": {"omp": false}
```

New OMP agents then start on that version, other CLIs keep updating, and the
update command in Commands says updates are off for OMP instead of running
`omp update`.

A running agent keeps the version it started with. **Update this tab's CLI and
reload it** and **Update Claude Code and reload its tabs** in Commands run the
update where each agent runs (over ssh, without a terminal or password prompt),
once per CLI and machine, then reload those agents. They keep working
meanwhile; an update that fails says why and leaves them running.

## Long jobs

Claude Code 2.1.285 introduced a 30-minute limit on shell commands Claude runs
in the background. Versions through 2.1.287 apply that limit in terminal
sessions; `BASH_MAX_TIMEOUT_MS` raises its ceiling and
`BASH_DEFAULT_TIMEOUT_MS` changes the foreground default. Upstream 2.1.288
scopes the background limit to unattended sessions (`-p`, the Agent SDK, CI and
cloud), so agents in lapis running 2.1.288 or newer have no background limit in
their terminal. A job that must outlive the agent still belongs detached
(`nohup`, `setsid` or `tmux`, writing to a log the agent reads), which also
outlives restarts, reloads and plan switches.

## Restart, reload, reopen

An agent that has ended or cannot be reached keeps its last screen, with a bar
giving the reason and the key that closes it. **Restart agent** in Commands
starts it again in the same card, resuming its conversation.

**Reload tab**, **Reload category** and **Reload window** do the same for running
agents: each CLI ends and starts again in its card, resuming its conversation,
so it rereads settings such as Claude Code's permissions. An agent on another
machine that started before lapis knew its conversation is left running, since
reloading it would start a new one.

Command-Shift-T reopens the last agent you closed, resuming its conversation
where its CLI can. Command-O resumes any past Claude Code or Codex conversation
from this Mac as a new agent in its folder.

## After a reboot

If an agent's session service is gone when lapis opens (after a restart, a
crash or a power cut), lapis restarts it in its card, like a restored terminal
tab. Codex, Claude, Grok, OpenCode, OMP, Kimi and Antigravity resume their saved
conversation with their own resume option; other CLIs, or a conversation with no
saved transcript yet, start fresh in the same folder. Resume arguments you gave
explicitly win; lapis adds one only when none is present.

Each service records its conversation beside its endpoint (`<endpoint>.resume`),
from the Codex observer, the Claude hook adapter, or the `agent_checkpoint`
sequence that iTerm2 restore hooks print. For Codex builds lapis has not
qualified, and Codex agents whose service predates these records, lapis reads
the thread from the rollouts its app-server holds open, following `/new` and
`/resume`. Codex transcript lookup stays within the `sessions/YYYY/MM/DD`
layout and does not follow symlinks.

To have agents come back at login without opening a window, install the login
helper once:

```sh
uv run --no-project python scripts/restore_at_login.py install
```

It runs `lapis_desktop --restore-agents --serve`, restarts the agents whose
services died with the Mac, then keeps the workspace without a window so the
phone can start agents; opening lapis takes the workspace over and the helper
exits. It leaves running agents alone and needs a logged-in user session. It
keeps custom `CODEX_HOME`, `CLAUDE_CONFIG_DIR` and `LAPIS_HISTORY_ROOT` values
from the shell that installed it; reinstall it after changing those.
