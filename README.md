# lapis

<img src="assets/lapis.svg" width="80" height="80" alt="lapis silk Cabochon icon">

lapis is a Mac app for running many coding agents at once. Each agent (Claude
Code, Codex, Grok, OpenCode, OMP, Kimi or Antigravity) gets its own live
terminal. You group agents into categories, watch every agent's latest lines in
a strip under the one you are working with, and jump straight to whichever needs
you. Agents keep running when you close or update lapis, and come back after a
restart.

It is built for speed first: switching agents and typing should feel instant on
an Apple silicon Mac with a high-refresh display.

## Download

Get `lapis-macos-arm64.dmg` from the
[latest release](https://github.com/RESMP-DEV/lapis/releases/latest), open it
and drag lapis to Applications. It needs an Apple silicon Mac with macOS 14 or
later, uses the agent CLIs you already have installed, and updates itself. See
[install](docs/install.md) for what it keeps where and how upgrades work.

## A quick tour

- **Start an agent** with Command-T: pick a CLI, a folder, a model and how much it
  may do without asking, on this Mac or any machine in your ssh config.
  [Agents](docs/agents.md)
- **Categories** group agents. The strip under the stage shows each agent's
  latest lines; drag a card onto the stage to tile it beside another.
  [Workspace](docs/workspace.md)
- **Jump to what needs you** with Command-J (the next) or Command-L (the latest),
  even from another app with Command-Option-L. Chimes and notifications say when.
- **Plan usage** for every CLI you are signed in to, on this Mac and your other
  machines, with several plans shared across sessions. [Usage](docs/usage.md)
- **Your phone** shows and drives the same agents. [Phone](docs/phone.md)
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
| [workspace](docs/workspace.md) | Categories, the strip, tiles, finding, links, history |
| [keys](docs/keys.md) | Every keyboard shortcut |
| [config](docs/config.md) | `lapis.json`: defaults, alerts, editor, CLI flags |
| [usage](docs/usage.md) | Plan usage and plans shared across sessions |
| [phone](docs/phone.md) | The iPhone app and its gateway |
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
