# Phone

The iPhone app (`apps/ios`) talks to a small gateway on the Mac
(`apps/remote/lapis_remote.py`) over Tailscale or ZeroTier. It shows the same
agents as the Mac and can type to them, start them and arrange them.

## Access

There is nothing to sign in to. The gateway answers a request only when
`tailscale whois` names the Mac owner's login on an iOS or Android device, or
when the peer belongs to one of the Mac's private ZeroTier networks. So the
phone must be signed in to Tailscale with the Mac's account, or joined to its
ZeroTier network. Keep Tailscale active on both when using HTTP: its WireGuard
connection encrypts the traffic. The configured gateway address is trusted; hosts
elsewhere on the LAN are outside this arrangement. An explicit HTTPS address
stays HTTPS, and other URL schemes are refused.

## Setup

Keep the gateway running at login:

```sh
uv run --no-project python apps/remote/launch_agent.py install   # also status, uninstall
```

Its log is `~/Library/Logs/lapis-remote.log`. It serves the workspace lapis
uses: `LAPIS_HOME` if set, else the app's `~/.lapis` once that has a workspace,
else the checkout's `runtime/` (`--registry` and `--config` pin another).

With the phone unlocked on the same Wi-Fi or a cable, this builds, signs and
installs the app with your development profile, set to reach this Mac by its
Tailscale name:

```sh
uv run --no-project python scripts/install_ios_app.py
```

To work on it in Xcode, run `xcodegen` in `apps/ios`; `Config/Local.xcconfig`
(not committed) holds your team and default host.

## Using it

Categories appear in the Mac's order, numbered as the Mac numbers them, and
follow the Mac at once when one is moved, added or renamed there. Each lists its
agents as cards with the CLI's mark and the folder (`~/dev/infinity`; an agent
over ssh shows its host first, `devbox:~/lapis`).

Opening an agent shows its screen at phone width, with a key bar (esc, ^C,
arrows, enter, backspace, tab, ^U, ^D) and a message field that pastes and
presses Enter; dictation works there. Scrolling up loads earlier output from the
service's history, and the history bar works as on the Mac. Swiping left or
right moves to the next or previous agent in the category (the title shows "2 of
4"), so the list is only needed to change category. Rotation updates the
terminal grid while composing; the keyboard appearing alone keeps its row count.

The phone joins the agent beside the desktop: both show the same screen, either
can type, and the terminal takes the size of the device in use. Opening the
agent on the phone gives it the phone's size; closing it or locking the phone,
activating the lapis window, moving the pointer over the agent, or typing on
the Mac gives it back. Agents started before this build run services that cannot
be joined: opening one on the phone takes it from the desktop (its card offers
**Reconnect agent**), and restarting it gives it a service that can. The Mac must
be awake.

Scrolling over a full-screen program scrolls the program itself; on the phone a
vertical drag turns the wheel.

## Starting and arranging

The **+** at the top offers a new agent, **Resume conversation** (the Mac's
latest Claude Code and Codex conversations, searchable, each resumed as a new
agent in its folder), **Terminal** (a plain shell on the Mac or one of its ssh
machines) or a new category, added on the Mac without moving its window. Open
terminals are listed above the agents and swipe away the same way.

A new agent from the phone: pick the machine (this Mac, or an ssh host from your
ssh config and shell history, reachable and most used first), the CLI, the
category, a folder, a model and an approval mode. It opens as a new tab in that
category on the Mac, then on the phone once it runs. The folder starts at
`newAgent.folder` (the machine's own in `machines`, else the one for every
machine), then the ten folders where you have started the most agents, then the
machine's folders to browse, or a search by a few letters. The lists and each
agent's screen are fetched in the background, so opening the sheet or an agent
does not wait. An agent shown on the Mac keeps the stage. Starting agents needs
lapis on the Mac: an open window, or the
login helper from [agents](agents.md#after-a-reboot).

Swiping an agent right (or a long press) offers **Rename**; left offers
**Close**, and a full swipe closes it. Holding an agent offers **Move to**
another category, **Move earlier**, **Move later**, **Restart** once it has
stopped, and **Close**. Each category's heading has a menu: a new agent there,
**Rename**, **Arrange categories** (drag to order, tap to rename, add one) and
**Remove**, allowed once the category is empty and another remains.

Settings on the phone has its text size, the categories, and the Mac's settings
that matter away from it: keeping the Mac awake, its chimes, background
notifications and plan usage, saved to the Mac's `lapis.json`.

**Send screen to Mac** in an agent's menu saves a screenshot and the exact screen
data under `runtime/phone-captures/`, for debugging what the phone drew; failures
show in the same alert as delivery results.

## Ultra Tab

[Ultra Tab for iPhone](ultratab.md#on-the-iphone) uses the same gateway, with
the same admission (the owner's devices over Tailscale or the Mac's ZeroTier
networks, the `X-Lapis-Client` header, no browsers) and the same port. Two
routes serve it:

- `GET /api/deck` returns, read-only, the registry's categories and agents
  (id, title, category, folder and CLI; no launch commands), the lapis window's
  `agent_state.json` and the composed `ultratab_cards.json` as published
  beside the registry, whether that window still runs, and a version. With
  `?after=<version>` it answers once one of those files changes, or after at
  most eight seconds. A missing file is `null`; a linked, oversized or
  unreadable one is `null` with a line in `problems`.
- `POST /api/agents/<id>/submit` with `{"text": "..."}` joins the agent's
  session as an extra view, sends the text as one paste followed by Return,
  and leaves; it never resizes the terminal or replaces the window's
  connection. It answers `{"ok": true}` once the session admits the paste, and
  409 with the session's reason when it does not (a pending request, a session
  that cannot be joined or is not running).

## Testing

```sh
uv run --no-project python scripts/check_ios_remote.py [--codex] [--claude]
```

runs the app's UI tests in a headless simulator against disposable services.
