"""Run the lapis Android app's live-session checks on a real device over adb.

The Android sibling of check_ios_remote.py, driven through adb instead of
XCUITest. Starts, all disposable and on this Mac:

- session services running tools/qa/fake_agent.py ("echo agent", a "second
  agent" to navigate between, and a "parked" registry entry not running),
- the gateway (apps/remote/lapis_remote.py) on 127.0.0.1 with --allow-local
  and a tailscale fixture script, so only loopback peers are admitted,
- `adb reverse tcp:7351 tcp:7351`, so the phone reaches that gateway over
  USB as a loopback client: no network, no overlay join, nothing manual.
  The product admission path (the owner's Tailscale/ZeroTier overlay) is
  unchanged; this is the same test admission the iOS simulator check uses.

Then it installs the debug APK, launches it with the debug overrides
(gatewayHost 127.0.0.1:7351, resetCache, terminalFontSize 12) and drives the
UI through tools/qa/android_ctl.py, asserting on both sides: the phone's
rendered terminal (through its accessibility description) and a Mac-side
client (lapis_remote.WireSession) attached the way the desktop is, which
must see the phone's input, answer it, and never be closed by it.

Checks: list, open, sync, draft (type without Enter), scrollback, resize
(`wm size`), wheel (drag reaches a full-screen program), interrupt (^C chip
stops a running program), background (home and return), snippets (add, run,
survive a process restart), crash scan. Screenshots land
under build/android/remote-screens/<stamp>/ and a JSON receipt under
build/android/.

    uv run --no-project python scripts/check_android_remote.py [--only CHECK ...]
        [--no-install] [--keep]

Exit codes: 0 green; 1 a check failed or no device; 3 skipped (no Android
SDK, Gradle, JDK 17+, desktop build, or APK with --no-install).
"""

import argparse
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "apps" / "remote"))
sys.path.insert(0, str(ROOT / "tools" / "qa"))
sys.path.insert(0, str(ROOT / "scripts"))

try:
    import android_ctl  # noqa: E402
    import check_android  # noqa: E402
    import lapis_remote  # noqa: E402
except ModuleNotFoundError as missing:
    # A missing sibling module is an environment gap (the SDK/JDK/Gradle
    # probes live in check_android), not a failed check: skip, exit 3.
    print(f"missing module {missing.name}; skipping (exit 3)", file=sys.stderr)
    sys.exit(3)

SERVICE = ROOT / "build" / "desktop" / "services" / "session" / "lapis_session_service"
APK = (
    ROOT
    / "apps"
    / "android"
    / "app"
    / "build"
    / "outputs"
    / "apk"
    / "debug"
    / "app-debug.apk"
)
BUILD = ROOT / "build" / "android"
PORT = 7351

SKIP_EXIT = 3


class Run:
    """Owned processes with per-name logs under build/android/remote-logs/."""

    def __init__(self):
        self.runtime = Path(
            tempfile.mkdtemp(prefix="lapis-android-", dir="/tmp")
        ).resolve()
        self.processes = []
        self.logs = BUILD / "remote-logs"
        self.logs.mkdir(parents=True, exist_ok=True)

    def start(self, name, command, env=None):
        log = (self.logs / (name + ".log")).open("wb")
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=log,
            start_new_session=True,
            env={**os.environ, **(env or {})},
        )
        self.processes.append(process)
        return process

    def service(self, identifier, program, arguments, directory):
        endpoint = self.runtime / (identifier + ".sock")
        (self.runtime / "history").mkdir(mode=0o700, exist_ok=True)
        self.start(
            "service-" + identifier[:8],
            [str(SERVICE), str(endpoint), str(directory), program, *arguments],
            {"LAPIS_HISTORY_ROOT": str(self.runtime / "history")},
        )
        deadline = time.monotonic() + 20
        while not lapis_remote.service_answers(str(endpoint)):
            if time.monotonic() > deadline:
                raise SystemExit(f"service {identifier} did not start")
            time.sleep(0.1)
        return str(endpoint)

    def stop(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    continue
        for process in self.processes:
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        # Services own their agents in new sessions; match the exact runtime
        # socket or the fixture interpreter, never a path mention (the iOS
        # check's rule: a shebang script appears after its interpreter in ps).
        fake = str(ROOT / "tools" / "qa" / "fake_agent.py")
        try:
            ps_output = subprocess.run(
                ["ps", "-axo", "pid=,command="],
                capture_output=True,
                text=True,
                timeout=30,
            ).stdout
        except subprocess.TimeoutExpired:
            # A wedged ps must not hang teardown on top of everything else;
            # sweep what we can from the process table we already got.
            ps_output = ""
        for row in ps_output.splitlines():
            fields = row.split(None, 1)
            if len(fields) != 2:
                continue
            try:
                command = shlex.split(fields[1])
            except ValueError:
                continue
            owned_service = command[:1] == [str(SERVICE)] and any(
                Path(argument).parent == self.runtime and argument.endswith(".sock")
                for argument in command[1:]
            )
            owned_agent = len(command) > 1 and command[1] == fake
            if owned_service or owned_agent:
                try:
                    os.kill(int(fields[0]), signal.SIGKILL)
                except (ProcessLookupError, PermissionError, ValueError):
                    pass


class MacClient(threading.Thread):
    """Stays attached to the echo agent the way the desktop is. When the
    phone's line arrives it answers "pong from mac"; it records whether it
    was ever closed (phone activity must never close it) and every grid size
    it saw, which is how the harness observes the phone resizing the PTY."""

    def __init__(self, agent):
        super().__init__(daemon=True)
        self.session = lapis_remote.WireSession(
            agent, 100, 30, mode=lapis_remote.DISCOVER
        )
        self.stopping = threading.Event()
        self.saw_phone = False
        self.closed = None
        self.sizes = set()
        self.last_size = None

    def run(self):
        while not self.stopping.is_set():
            try:
                received = self.session.receive(0.5)
                if received is None:
                    continue
                kind, data = received
                if kind == lapis_remote.STATUS:
                    self.closed = lapis_remote.status_message(data)
                    return
                body = self.session.accept_snapshot(kind, data)
                if body is None:
                    continue
                rendered = lapis_remote.render_snapshot(body)
                self.sizes.add((rendered["columns"], rendered["rows"]))
                self.last_size = (rendered["columns"], rendered["rows"])
                if self.saw_phone:
                    continue
                screen = "\n".join(
                    "".join(run[0] for run in line) for line in rendered["lines"]
                )
                if "echo: ping from phone" in screen:
                    self.saw_phone = True
                    self.session.text(b"pong from mac\r")
            except (OSError, EOFError, lapis_remote.GatewayError) as error:
                if not self.stopping.is_set():
                    self.closed = str(error)
                return
            except Exception as error:
                # Anything else — a frame the renderer cannot digest (TypeError,
                # IndexError, ...) or an unexpected library failure — must
                # surface as a closed client in the receipt, not die silently
                # in this thread.
                if not self.stopping.is_set():
                    self.closed = f"malformed frame: {error}"
                return

    def stop(self):
        self.stopping.set()
        self.join(5)
        self.session.close()


def wait_terminal(device, needle, timeout=20.0, quiet=False):
    """Poll the phone's rendered terminal until it contains the text."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if needle in device.terminal_text():
                return True
        except android_ctl.DeviceError:
            pass
        time.sleep(0.5)
    if not quiet:
        print(f"terminal never showed {needle!r}", file=sys.stderr)
    return False


def send_line(device, text):
    device.tap_node(id="composer")
    device.type_text(text)
    device.tap_node(id="send")


def reveal(device, target_id, max_swipes=8):
    """Swipe the chip's own row left until the target chip is actually on
    screen. Rows in the compact bar compose every chip, so the dump reports
    off-screen bounds; tapping those coordinates taps nothing.

    Containment is judged against the bar's own right edge, not the panel
    size: on a foldable `wm size` can report the unfolded panel while the
    cover display is in use, and a chip past the visible edge would then
    pass a `right <= panel_width` check and get tapped out of the window.
    The swipe row comes from the chip's own bounds, so a snippet chip
    swipes the snippet row, not the key row above it. Returns None when the
    bar itself is absent, so callers can name that failure instead of
    blaming the chip."""
    for _ in range(max_swipes):
        bar = device.find(id="command-bar")
        if bar is None:
            return None
        bar_right = bar["bounds"][2]
        node = device.find(id=target_id)
        if node is not None:
            left, top, right, bottom = node["bounds"]
            if left >= 0 and right <= bar_right:
                return True
            row_y = (top + bottom) // 2
            if left < 0:
                # The chip was carried past the row's left edge (the swipe
                # ends with fling momentum), and a further leftward swipe
                # only pushes it farther out — the containment test then
                # can never pass again and the loop exhausts. Swipe back
                # toward the row start instead.
                device.swipe(80, row_y, bar_right - 80, row_y, 250)
                time.sleep(0.4)
                continue
        else:
            _, bar_top, _, bar_bottom = bar["bounds"]
            row_y = bar_top + (bar_bottom - bar_top) // 4
        device.swipe(bar_right - 80, row_y, 80, row_y, 250)
        time.sleep(0.4)
    return False


def fixture(run):
    """The registry, services, and loopback gateway; returns (agents, gateway
    process) with the listing already answering."""
    fake = str(ROOT / "tools" / "qa" / "fake_agent.py")
    python = sys.executable
    agents = []
    for title, category in (("echo agent", "build"), ("second agent", "build")):
        identifier = str(uuid.uuid4())
        endpoint = run.service(identifier, python, [fake], ROOT)
        agents.append(
            {
                "id": identifier,
                "title": title,
                "category": category,
                "harness": "grok",
                "endpoint": endpoint,
                "program": python,
                "arguments": [fake],
                "directory": str(ROOT),
            }
        )
    parked = str(uuid.uuid4())
    agents.append(
        {
            "id": parked,
            "title": "parked",
            "category": "later",
            "harness": "claude",
            "endpoint": str(run.runtime / (parked + ".sock")),
            "program": "/bin/sh",
            "arguments": [],
            "directory": "/",
        }
    )
    registry = run.runtime / "workspace.json"
    registry.write_text(
        json.dumps(
            {
                "version": 2,
                "activeCategory": "build",
                "categories": [
                    {"id": "build", "name": "Build"},
                    {"id": "later", "name": "Later"},
                ],
                "agents": agents,
            }
        )
    )
    # execve splits the shebang on whitespace, so an interpreter path with a
    # space in it would leave the kernel refusing the script; a runtime-local
    # symlink keeps the shebang a single token on any host.
    python_link = run.runtime / "python"
    python_link.symlink_to(python)
    tailscale = run.runtime / "tailscale-fixture"
    tailscale.write_text(
        f"#!{python_link}\n"
        "import json, sys\n"
        "if sys.argv[1:] != ['status', '--json']: sys.exit(1)\n"
        "print(json.dumps({'Self': {'UserID': 1, 'TailscaleIPs': [], "
        "'DNSName': 'fixture.invalid'}, "
        "'User': {'1': {'LoginName': 'device-fixture'}}}))\n"
    )
    tailscale.chmod(0o700)
    gateway = run.start(
        "gateway",
        [
            python,
            str(ROOT / "apps" / "remote" / "lapis_remote.py"),
            "--registry",
            str(registry),
            "--bind",
            "127.0.0.1",
            "--port",
            str(PORT),
            "--allow-local",
            "--tailscale",
            str(tailscale),
        ],
    )
    deadline = time.monotonic() + 10
    while True:
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:{PORT}/api/agents",
                headers={"X-Lapis-Client": "android"},
            )
            with urllib.request.urlopen(request, timeout=1) as response:
                listing = json.load(response)
            if any(
                item["title"] == "echo agent"
                for category in listing["categories"]
                for item in category["agents"]
            ):
                exit_code = gateway.poll()
                if exit_code is not None:
                    # A stale gateway (an earlier --keep run) can still own
                    # the port: this probe is then answered by that fixture,
                    # the phone drives its agents while the Mac client
                    # watches this run's silent ones, and every Mac-side
                    # check fails against the wrong session. The bind
                    # failure already killed this run's gateway, so say so
                    # instead of validating against the wrong fixture.
                    raise SystemExit(
                        f"a stale gateway owns 127.0.0.1:{PORT} "
                        f"(this run's exited {exit_code}); stop it and rerun; "
                        f"see {run.logs / 'gateway.log'}"
                    )
                # The gateway's own normalization of the registry, with the
                # mode field WireSession needs.
                return lapis_remote.load_workspace(str(registry))["agents"], gateway
        except (OSError, ValueError, KeyError):
            pass
        if gateway.poll() is not None or time.monotonic() >= deadline:
            raise SystemExit(
                f"fixture gateway did not start; see {run.logs / 'gateway.log'}"
            )
        time.sleep(0.1)


def install_apk(device, allow_build):
    if APK.exists():
        device.run("install", "-r", str(APK))
        return True
    if not allow_build:
        print(f"no APK at {APK}; run check_android.py first", file=sys.stderr)
        return False
    gradle_command = check_android.find_gradle()
    env = check_android.java_environment()
    if gradle_command is None or env is None:
        return False
    env["ANDROID_HOME"] = str(android_ctl.find_sdk().resolve())
    env["ANDROID_SDK_ROOT"] = env["ANDROID_HOME"]
    if (
        check_android.gradle(gradle_command, env, ":app:assembleDebug") != 0
        or not APK.exists()
    ):
        print("assembleDebug failed", file=sys.stderr)
        return False
    device.run("install", "-r", str(APK))
    return True


def checks(device, mac, screens):
    """Each named check drives the phone and asserts on both sides."""

    # Set when a check may have left the busy program owning the PTY: the
    # later checks that type into the terminal report SKIPPED instead of
    # failing with messages that blame the wrong interaction.
    wedged = {"busy": False}

    def shot(name):
        device.screenshot(screens / f"{name}.png")

    def bar_absent():
        if device.find(id="command-bar") is None:
            return "the command bar is disabled in settings; enable it and rerun"
        return None

    def long_press(device, node):
        x, y = android_ctl.bounds_center(tuple(node["bounds"]))
        device.swipe(x, y, x, y, 600)

    def check_list():
        device.launch(host=f"127.0.0.1:{PORT}", font=12, reset_cache=True, fresh=True)
        if device.wait(text="echo agent", timeout=25) is None:
            return "the workspace never listed the agents"
        if (
            device.find(text="second agent") is None
            or device.find(text="parked") is None
        ):
            return "the listing is missing an agent"
        shot("01-workspace")
        return None

    def check_open():
        # By visible text, not the id tag: the card's tag is the agent id,
        # which the harness does not know ahead of the listing.
        device.tap_node(text="echo agent")
        if device.wait(id="terminal", timeout=15) is None:
            return "the terminal never appeared"
        if not wait_terminal(device, "lapis fake agent"):
            return "the agent's banner never rendered"
        shot("02-stage")
        return None

    def check_sync():
        device.tap_node(id="composer")
        device.type_text("ping from phone")
        device.tap_node(id="send")
        if not wait_terminal(device, "echo: ping from phone"):
            return "the phone's line never rendered"
        if not wait_terminal(device, "pong from mac", timeout=15):
            return "the Mac's answer never reached the phone"
        time.sleep(0.5)
        if not mac.saw_phone:
            return "the Mac-side client never saw the phone's line"
        if mac.closed is not None:
            return f"phone activity closed the Mac client: {mac.closed}"
        shot("03-sync")
        return None

    def check_draft():
        device.tap_node(id="composer")
        device.type_text("draft without enter")
        device.tap_node(id="type")  # without Enter: the PTY echoes the text on
        # the prompt line, but the line is never submitted.
        if not wait_terminal(device, "draft without enter"):
            return "the typed draft never echoed to the prompt line"
        time.sleep(1.5)
        if "echo: draft without enter" in device.terminal_text():
            return "the draft was submitted although Enter was never sent"
        shot("04-draft")
        # Leave the prompt clean. The composer is already empty (sending the
        # type-without-Enter draft clears it), but the PTY's canonical buffer
        # still holds the pending line: an Enter submits it, so later checks
        # start from a fresh "› " prompt instead of concatenating onto it.
        send_line(device, "")
        if not wait_terminal(device, "echo: draft without enter"):
            return "the draft cleanup submit never echoed"
        return None

    def check_scrollback():
        send_line(device, "marker before count")
        if not wait_terminal(device, "echo: marker before count"):
            return "the marker never appeared"
        send_line(device, "count")
        if not wait_terminal(device, "count 120", timeout=30):
            return "the count output never finished"
        # The composer submit leaves the keyboard open, and the stage's
        # imePadding then shrinks the terminal to the strip above it —
        # screen-center scroll gestures would land on the keyboard and
        # never reach the list. A reader drops the keyboard to read.
        device.dismiss_ime()
        # The drag that scrolled older must surrender the terminal's
        # follow-bottom anchor (correct app behavior: output must not yank
        # the view out from under the reader). The accessibility text is a
        # scroll-independent concatenation of history plus live frame, so
        # no text anchor can observe scroll position; the real observable
        # is the follow state the app speaks at the head of the terminal's
        # description, which flips to "reading" only when the drag ends
        # with the live bottom out of view.
        found = False
        for _ in range(16):
            device.scroll("older", 4)
            if "Reading earlier output." in device.terminal_text():
                found = True
                break
        if not found:
            return "scrolling older never surrendered the follow-bottom anchor"
        # Scroll back down so later checks see new output. The same state
        # line is the anchor in reverse: the app reports following only
        # when the last visible row is the live bottom — "count 120" one
        # row above the prompt does not qualify, so the drag must land on
        # the prompt itself.
        restored = False
        for _ in range(16):
            device.scroll("newer", 4)
            if "Following live output." in device.terminal_text():
                restored = True
                break
        if not restored:
            return "scrolling never returned to the live bottom"
        shot("05-scrollback")
        return None

    def check_resize():
        # The last frame's grid, not the set of grids ever seen: a reset that
        # restores the phone's real panel returns to a size the set already
        # holds, so only the ordered observation can see it come back.
        default = mac.last_size
        if default is None:
            return "the Mac client never saw a frame before the resize check"
        size = device.shell("wm size")
        physical = next(
            line.split(":", 1)[1].strip()
            for line in size.splitlines()
            if line.startswith("Physical size")
        )
        override = "720x1600" if physical != "720x1600" else "600x1200"
        device.shell(f"wm size {override}")
        try:
            time.sleep(2.0)
            if mac.last_size == default:
                return f"the grid never changed after wm size {override}"
            if not wait_terminal(device, "count 120", timeout=15):
                return "the terminal lost its content after the resize"
            shot("06-resized")
        finally:
            device.shell("wm size reset")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and mac.last_size != default:
            time.sleep(0.5)
        if mac.last_size != default:
            return f"the grid never returned to {default[0]}x{default[1]} after wm size reset"
        return None

    def check_wheel():
        # A full-screen program that reports the mouse takes the wheel: the
        # drag must reach the program (SGR wheel events) instead of scrolling
        # the phone's archive, and the program ends mouse mode on the first
        # event and says what arrived.
        send_line(device, "mouse")
        if not wait_terminal(device, "mouse ready", timeout=20):
            return "the mouse program never announced itself"
        shot("07-wheel-mode")
        # Sending leaves the composer focused and the keyboard up; the
        # keyboard window then covers the drag's start point, so the app
        # never sees the gesture. BACK is the dismiss key (ESC is not).
        device.key("BACK")
        time.sleep(0.8)
        device.scroll("newer", 3)
        if not wait_terminal(device, "mouse got", timeout=30):
            return "the drag never produced a wheel event the program saw"
        # "first ESC[<64;…" means real events arrived; "ESCnothing" means none.
        # Judge the newest result line: the archive keeps earlier arms, and
        # their text would answer for this one.
        results = re.findall(r"mouse got[^\n|]*", device.terminal_text())
        if not results or "ESC[<" not in results[-1]:
            return "the program saw zero wheel events from the drag"
        shot("08-wheel-delivered")
        return None

    def check_interrupt():
        # The milestone C finish line: ^C through the bar's chord chip must
        # interrupt a running program (the PTY turns the byte into SIGINT),
        # not just be assumed to. The busy loop reports how far it got, and
        # the agent must still answer afterwards. On any failure the busy
        # loop still owns the foreground PTY (it survives the app's process
        # death; the agent lives on the gateway side), so the exit paths
        # best-effort-interrupt it and the checks that type into the
        # terminal afterwards skip, rather than reporting misleading
        # failures about their own interactions.
        absent = bar_absent()
        if absent is not None:
            return absent
        send_line(device, "busy")
        interrupted = False
        try:
            if not wait_terminal(device, "busy 3", timeout=20):
                return "the busy program never started (busy may still own the PTY)"
            revealed = reveal(device, "key-ctrl-c")
            if revealed is None:
                return "the command bar vanished mid-check"
            if not revealed:
                return (
                    "the ^C chip never scrolled into view (busy may still own the PTY)"
                )
            device.tap_node(id="key-ctrl-c")
            if not wait_terminal(device, "busy interrupted after", timeout=15):
                return "the ^C chord never interrupted the busy program"
            interrupted = True
        finally:
            if not interrupted:
                wedged["busy"] = True
                # Best effort, and never at the cost of the real failure
                # above. find() returns the chip even when its bounds are
                # off-screen (uiautomator dumps every composed chip), and
                # tapping off-screen coordinates is silently dropped — so
                # re-reveal first; that is the branch that actually runs
                # when the failure was the reveal itself.
                try:
                    if reveal(device, "key-ctrl-c"):
                        device.tap_node(id="key-ctrl-c")
                        time.sleep(1.5)
                except Exception:
                    # Best effort: any failure here (adb hung, device
                    # pulled, subprocess timeout) must not replace the
                    # real failure already recorded above.
                    pass
        send_line(device, "after interrupt")
        if not wait_terminal(device, "echo: after interrupt"):
            return "the agent did not answer after the interrupt"
        shot("09-interrupt")
        return None

    def check_background():
        if wedged["busy"]:
            return "SKIPPED: the busy program may still own the PTY after the interrupt failure"
        device.key("HOME")
        time.sleep(1.0)
        device.launch()  # the same task comes forward and reattaches
        if device.wait(id="terminal", timeout=15) is None:
            return "the terminal did not come back"
        send_line(device, "back again")
        if not wait_terminal(device, "echo: back again"):
            return "typing after returning from the background failed"
        if mac.closed is not None:
            return f"backgrounding closed the Mac client: {mac.closed}"
        shot("10-background")
        return None

    def check_snippets():
        # Snippet lifecycle: added through the editor, sent as paste+Enter
        # (the fake agent echoes the line back), and still there after
        # process death (DataStore) — no gateway sync involved anywhere.
        # The store survives everything this harness resets (resetCache
        # wipes only caches; the fixture rebuilds only agents), so the
        # precondition must be established here: clear the list through the
        # editor and assert on a run-unique text, so stale entries from
        # earlier runs can neither crowd the 24-snippet cap nor occupy
        # index 0 under this check's feet.
        if wedged["busy"]:
            return "SKIPPED: the busy program may still own the PTY after the interrupt failure"
        absent = bar_absent()
        if absent is not None:
            return absent
        snippet_text = f"git status {time.strftime('%H%M%S')}"
        # The + chip is the snippet row's rightmost child, off-screen once
        # any snippet persists; the leftmost chip is always visible and its
        # long-press opens the same editor.
        first = device.find(id="snippet-0")
        if first is not None:
            long_press(device, first)
        else:
            device.tap_node(id="snippets-add")
        if device.wait(id="snippet-new", timeout=10) is None:
            return "the snippet editor never opened"
        for _ in range(25):
            if device.find(id="snippet-0-delete") is None:
                break
            device.tap_node(id="snippet-0-delete")
            time.sleep(0.3)
        else:
            return "existing snippets never cleared"
        device.tap_node(id="snippet-new")
        device.type_text(snippet_text)
        device.tap_node(id="snippet-add")
        device.tap_node(id="snippet-done")
        chip = device.wait(text=snippet_text, timeout=10)
        if chip is None:
            return "the added snippet chip never appeared"
        shot("11-snippet-added")
        device.tap_node(text=snippet_text)
        if not wait_terminal(device, f"echo: {snippet_text}"):
            return "tapping the snippet never ran it"
        # Process death must not lose the list: force-stop and a plain
        # relaunch (no resetCache) restores it from DataStore.
        device.stop()
        time.sleep(1.0)
        device.launch()
        if device.wait(text="echo agent", timeout=25) is None:
            return "the workspace never came back after the restart"
        device.tap_node(text="echo agent")
        if device.wait(id="terminal", timeout=15) is None:
            return "the stage did not reopen after the restart"
        chip = device.wait(text=snippet_text, timeout=10)
        if chip is None:
            return "the snippet did not survive the restart"
        shot("12-snippet-restored")
        return None

    def check_crash():
        crashes = device.crash_lines()
        if crashes:
            return "the app crashed: " + crashes[0]
        return None

    return {
        "list": check_list,
        "open": check_open,
        "sync": check_sync,
        "draft": check_draft,
        "scrollback": check_scrollback,
        "resize": check_resize,
        "wheel": check_wheel,
        "interrupt": check_interrupt,
        "background": check_background,
        "snippets": check_snippets,
        "crash": check_crash,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", action="append", help="check name; repeat several")
    parser.add_argument(
        "--serial",
        help="adb serial to drive; required when more than one device is "
        "attached, so an unexpected emulator cannot steal the run",
    )
    parser.add_argument(
        "--no-install", action="store_true", help="reuse the installed APK"
    )
    parser.add_argument("--keep", action="store_true", help="keep the fixture running")
    arguments = parser.parse_args()

    if not SERVICE.exists():
        print("missing the desktop build; build the desktop first", file=sys.stderr)
        return SKIP_EXIT
    sdk = android_ctl.find_sdk()
    if sdk is None:
        print(f"Android SDK not found; skipping (exit {SKIP_EXIT})", file=sys.stderr)
        return SKIP_EXIT

    adb = str(sdk / "platform-tools" / "adb")
    try:
        listed = subprocess.run(
            [adb, "devices"], capture_output=True, text=True, timeout=30
        ).stdout
    except subprocess.TimeoutExpired:
        print(
            "adb devices timed out; restart the adb server and rerun", file=sys.stderr
        )
        return 1
    attached = [
        fields[0]
        for line in listed.splitlines()[1:]
        # Two columns and state "device"; "adb server" chatter and
        # unauthorized/offline rows do not count as attached.
        if len(fields := line.split()) == 2 and fields[1] == "device"
    ]
    if not attached:
        print("no adb device attached; connect one and rerun", file=sys.stderr)
        return 1
    serial = arguments.serial
    if serial is not None and serial not in attached:
        print(f"--serial {serial} is not an attached device", file=sys.stderr)
        return 1
    if serial is None and len(attached) > 1:
        # The default only ever picks the sole attached device; with several
        # attached, an unexpected emulator could silently steal the run.
        print(
            "more than one adb device attached ("
            + ", ".join(attached)
            + "); name one with --serial",
            file=sys.stderr,
        )
        return 1
    device = android_ctl.Device(serial=serial or attached[0])

    if not arguments.no_install or not device.state().get("app_version"):
        if not install_apk(device, allow_build=not arguments.no_install):
            return SKIP_EXIT if not APK.exists() else 1

    run = Run()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    screens = BUILD / "remote-screens" / stamp
    screens.mkdir(parents=True, exist_ok=True)
    mac = None
    failed = []
    skipped = []
    try:
        agents, gateway = fixture(run)
        device.reverse(PORT)
        echo = next(agent for agent in agents if agent["title"] == "echo agent")
        mac = MacClient(echo)
        mac.start()

        available = checks(device, mac, screens)
        chosen = arguments.only or list(available)
        for name in chosen:
            if name not in available:
                print(f"unknown check {name}", file=sys.stderr)
                return 1
            print(f"[{name}]", flush=True)
            try:
                problem = available[name]()
            except Exception as error:
                # A check that dies mid-gesture (device pulled, adb hung) is a
                # failed check, not a lost receipt: record it and keep going.
                problem = f"{type(error).__name__}: {error}"
            if problem is None:
                print(f"[{name}] ok", flush=True)
            elif problem.startswith("SKIPPED:"):
                # A check that cannot run meaningfully (wedged PTY) is not a
                # failure of what it tests; record it so the receipt says so.
                print(f"[{name}] {problem}", file=sys.stderr)
                skipped.append(name)
            else:
                print(f"[{name}] FAILED: {problem}", file=sys.stderr)
                failed.append((name, problem))

        saw_phone = mac.saw_phone
        closed_by = mac.closed
        # Snapshot before sorting: the client thread is still attached and
        # adds sizes as frames arrive; sorting a live set would race it.
        grids = sorted(f"{columns}x{rows}" for columns, rows in list(mac.sizes))
        receipt = {
            "checks": chosen,
            "failed": [name for name, _ in failed],
            "failures": {name: problem for name, problem in failed},
            "skipped": skipped,
            "mac_saw_phone": saw_phone,
            "mac_closed_by": closed_by,
            "grids": grids,
        }
        print(f"screens {screens}")
        print(f"Mac grids seen: {grids}")
        # Written before the Mac client stops: teardown problems must not
        # cost the run its evidence.
        (BUILD / "remote-check.json").write_text(json.dumps(receipt, indent=1) + "\n")
        mac.stop()
        mac = None
        return 1 if failed else 0
    finally:
        try:
            device.shell("wm size reset")
        except android_ctl.DeviceError:
            pass
        try:
            device.unreverse(PORT)
        except android_ctl.DeviceError:
            pass
        try:
            device.stop()
        except android_ctl.DeviceError:
            pass
        if mac is not None:
            mac.stop()
        if not arguments.keep:
            run.stop()
            shutil.rmtree(run.runtime, ignore_errors=True)
        else:
            print(f"kept: runtime {run.runtime}, logs {run.logs}")


if __name__ == "__main__":
    sys.exit(main())
