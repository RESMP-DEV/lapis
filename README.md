# lapis

<img src="assets/lapis.svg" width="80" height="80" alt="lapis silk Cabochon icon">

lapis is a Mac app for running many coding agents at once. Each agent (Claude
Code, Codex, Grok, OpenCode, OMP, Kimi or Antigravity) gets its own live
terminal. You group agents into colour-coded categories, see every agent and its
state in the sidebar, and jump straight to whichever needs you. Each agent runs in its own session service, so closing lapis normally
leaves it running (macOS 27 adds [conditions](docs/install.md#updates)); if its
service is gone, it starts again in its card when lapis next opens and resumes
its conversation where its CLI can. [Install](docs/install.md#updates)
distinguishes exercised reattachment from installed-update and reboot gates.

It is built for speed first: switching agents and typing should feel instant on
an Apple silicon Mac with a high-refresh display.

## Download

Get `lapis-macos-arm64.dmg` from the
[latest release](https://github.com/RESMP-DEV/lapis/releases/latest), open it
and drag lapis to Applications. It needs an Apple silicon Mac with macOS 14 or
later, uses the agent CLIs you already have installed, and checks for released
updates. See
[install](docs/install.md) for what it keeps where and how upgrades work.

## A quick tour

- **Early access starts local.** The supported first slice is a local Codex or
  Claude Code workspace. Remote hosts, the phone and next-prompt suggestions
  remain implemented or prototype surfaces, but are outside that
  first support slice; [status](docs/status.md) separates implemented code from
  exercised, measured and unqualified behavior.
- **Start an agent** with Command-T: pick a CLI, a folder, a model and how much it
  may do without asking, on this Mac. The machine picker also offers hosts from
  your ssh config, but remote sessions are outside the early-access support
  slice until remote reconnect and fan-out qualification passes. Each CLI uses
  the sign-in its machine already has.
  [Agents](docs/agents.md)
- **Categories** group agents. The sidebar lists each category, in its colour,
  with its agents under it; click one, or step through them all with
  Command-Shift-[ and ], to show it on the stage. A strip of live previews under
  the stage is there if you turn it on.
  [Workspace](docs/workspace.md)
- **See what needs you**: agents and categories are marked until you look, and with
  suggestions on, Tab with no guess to accept moves to the next agent waiting on
  you. A chime says when.
- **Your phone** can show and drive the same agents as a separately installed
  prototype over its gateway. It is outside the early-access support slice.
  [Phone](docs/phone.md)
- **Suggestions** can guess your next prompt for Claude Code or Codex only after
  you enable `nextPrompt.auto`; they are optional and experimental. With a dim
  guess showing, Tab types it for you to edit and Tab twice sends it; with none,
  Tab moves to the next agent that needs you. With
  suggestions disabled, Tab keeps its usual meaning in the terminal.
  [Suggestions](docs/suggestions.md)
- Every shortcut is in [keys](docs/keys.md) and every setting in
  [config](docs/config.md).

## Build from source

```sh
uv run --no-project python scripts/lapis.py doctor
uv run --no-project python scripts/lapis.py bootstrap
uv run --no-project python scripts/lapis.py build
uv run --no-project python scripts/lapis.py run
```

[Build](docs/build.md) covers checks and development fixtures;
[CONTRIBUTING](CONTRIBUTING.md) covers setup, tests and pull requests.

## Docs

| Page | What it covers |
| --- | --- |
| [install](docs/install.md) | Download, where lapis keeps things, closing, upgrading |
| [agents](docs/agents.md) | Starting agents, CLIs and modes, requests, restarts, reboots |
| [workspace](docs/workspace.md) | The sidebar and categories, the strip, the visual view, links, history |
| [keys](docs/keys.md) | Every keyboard shortcut |
| [config](docs/config.md) | `lapis.json`: defaults, alerts, CLI flags |
| [phone](docs/phone.md) | The prototype iPhone app and its separately installed gateway |
| [suggestions](docs/suggestions.md) | Optional next-prompt guesses, Tab, and their acceptance log |
| [build](docs/build.md) | Building, checks, probes and fixtures |
| [status](docs/status.md) | What is qualified, what remains, and the evidence |
| [architecture](docs/architecture.md) | Design decisions and the plan |

Agents working on lapis start with [AGENTS.md](AGENTS.md) (`CLAUDE.md` is the
same file).

## License

MIT; see [LICENSE](LICENSE). The Mac app also carries Qt (LGPL-3.0), MoltenVK
(Apache-2.0) and Ghostty's VT library (MIT); their notices are in
[third_party/](third_party) and in the app under `Contents/Resources/Notices`,
and each release attaches the Qt source it was built from.
