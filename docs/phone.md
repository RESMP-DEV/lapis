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

For ZeroTier, install with the Mac's address on the network the phone joins:

```sh
uv run --no-project python scripts/install_ios_app.py --host <mac-zerotier-address>
```

## When the phone cannot connect

"The Mac did not answer" covers a refused connection, a timeout and a failed
name lookup alike. Most accepted requests are logged, but silence is not proof
that traffic never arrived: when all 16 gateway connections are held, the
accept thread can return a busy reply without the normal request log, and an
abandoned `list_agents` or `deck` long-poll can also end without a log line.
Check those two cases before following the routing checks below, which are in
the order they mattered on 2026-10-27:

- **The gateway does not listen on ZeroTier.** The startup line says `via
  Tailscale` only, and the LaunchAgent's arguments lack `--zerotier`.
  `launch_agent.py install` records only the CLIs that are on PATH when it
  runs; launchd's own PATH never includes `/usr/local/bin`. Rerun the install
  from a shell where `zerotier-cli` resolves. If `launchctl bootstrap` exits 5
  the old job was still unloading: bootstrap the plist again. If Tailscale was
  ready but the Mac had not yet joined a private ZeroTier network when the
  gateway started, that running process still has no ZeroTier admission: join
  the network, then restart the gateway.
- **The tailnet's access policy drops the phone.** Tailscale ping (UDP)
  succeeding proves nothing about TCP. Test from another node with
  `curl http://<mac-tailscale-ip>:7349/api/agents`, and read the Mac's
  allowed sources from `tailscale debug netmap` (`PacketFilter[].Srcs`), but a
  successful request from another node proves only that node is allowed. Check
  the phone's tailnet address against the source ranges that may reach the
  Mac on port 7349. A work tailnet can admit only colleagues' machines;
  ZeroTier avoids it.
- **The phone and Mac are on different ZeroTier networks.** `zerotier-cli
  listnetworks` on the Mac must show the network the phone joined. The iOS
  app runs one network at a time: the switch that is on is the one in use.
- **The phone has no ZeroTier address.** The phone's log shows `ZeroTierPTP:
  Missing IPv4 address` and `addresses = ()` until the member, the node ID at
  the bottom of the ZeroTier app, is authorized on the controller.
- **The app still uses an old host.** The saved host outranks the bundled
  default, so reinstalling over the app keeps it. Writing the app's
  preferences plist with `devicectl device copy to` does not take effect:
  iOS serves the cached value. Uninstall, then install with `--host`.
- **iCloud Private Relay carries the request.** It proxies plain HTTP to a
  name that is not local; the phone's log shows `mask.icloud.com` and
  `Received response code 502 ... unreachable through proxy`. A raw ZeroTier
  address connected with Private Relay off; whether it is relayed when on is
  untested.

Opening `http://<mac-address>:7349/` in Safari on the phone separates the path
from the app: the gateway logs that request and answers 403, because browsers
are refused.

From the Mac, with the phone on a cable, `xcrun devicectl` installs, launches,
copies app data and reports the lock state (a locked phone refuses launches).
For the screen and logs, run `sudo "$(command -v uvx)" pymobiledevice3 remote
tunneld` in the background, then `uvx pymobiledevice3 developer dvt screenshot
--tunnel "" out.png` and `uvx pymobiledevice3 syslog live`. Neither taps the
screen, and an XCUITest runner built to drive other apps failed to install on
a retail iPhone with Xcode 26.6 ("Root install style is not supported").

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

[Ultra Tab for iPhone](https://github.com/RESMP-DEV/ultratab) uses the same gateway, with
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
