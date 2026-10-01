# Install

## Download

The Mac app is on the [releases page](https://github.com/RESMP-DEV/lapis/releases/latest):
open `lapis-macos-arm64.dmg` and drag lapis to Applications. It needs an Apple
silicon Mac with macOS 14 or later. It is signed with a Developer ID and
notarized, and built and checked on macOS 26.5.

lapis uses the agent CLIs you already have installed. Opened from Finder or the
Dock, it starts agents with your login shell's environment, so they find the
same tools and keys as in your terminal.

## Where it keeps things

The app keeps `lapis.json` and its runtime state in `~/.lapis` (or
`$LAPIS_HOME`). Private workspace metadata and window placement live under
`~/.lapis/runtime/`, as does the owner-only attention log. That log may contain
conversation titles or first prompts, so it is not for sharing. Runtime state is
not configuration: do not copy it between machines. A build from this repository
keeps the same things under the checkout's ignored `runtime/`, and builds and
captures under `build/`.

## Updates

lapis checks the latest release once a day and installs an update after asking
(Sparkle). Package provenance and updater checks are implemented on main, but no
tracked evidence yet shows an installed Sparkle update on that source. Settings
can also keep agents running at login.

The intended quit-and-open upgrade path reattaches to the same agent processes.
That same-child reattachment is exercised separately; it is not evidence of an
installed Sparkle transition. Launch fingerprints, the service protocol, the
workspace registry and resume records stay compatible across builds in the
supported protocol, and a source test pins the fingerprints.

On macOS 27 agents started from the window count as its background processes,
so two conditions apply:

- lapis must be allowed under **Allow in the Background** (System Settings,
  General, Login Items & Extensions). macOS adds it the first time you quit with
  agents still running.
- The old `lapis.app` must stay in place until the old window has exited.

Otherwise macOS ends every agent when the window quits.

## Closing and quitting

Closing the window (its close button, Command-Shift-W, or Command-W with no
agent left) only hides it: lapis keeps running, with its alerts and the phone,
and its Dock icon brings the window back. Command-Q quits and detaches from the
agents. Reopening lapis reconnects the same agents and restores each category's
selection and the window's placement. Neither stops an agent; Command-W on an
agent does.

The phone gateway is set up from this repository; the app does not include it
yet. See [phone](phone.md). How the app is built, signed and notarized is in
[CONTRIBUTING](../CONTRIBUTING.md#build-the-mac-app).
