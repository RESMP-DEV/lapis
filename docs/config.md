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

`harnessArguments` adds your own flags to every new agent of a CLI. lapis adds
none itself, and shell aliases do not apply because lapis starts the executable
directly.

## Alerts

An agent that needs you chimes (two rising taps), and again every few seconds
while the request waits and you are looking elsewhere, up to `repeat` times. A
Codex or Claude turn that ends out of view chimes once, quietly (`finished`).
While lapis is in the background the same moments post a notification
(`notify`), titled with the agent and naming its CLI ("Claude finished a
turn"); clicking it shows the agent. Appearance has the switches and a Play
button for each.

## The rest

- `editor` is the app that opens an agent's folder (else the first of Cursor, VS
  Code, Zed, Windsurf and Sublime Text installed).
- `keepAwake` keeps the Mac from sleeping while it is plugged in, so the phone
  can reach it.
- `usage` and `accounts` are described in [usage](usage.md).
- Key bindings are listed in [keys](keys.md); Appearance shows and edits them.
