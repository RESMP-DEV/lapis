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
    blaming the chip.

    Overshoot is terminal, not retryable: a fling can carry the chip past
    either edge the containment test measures, and repeating the same
    direction only pushes it farther out. The loop reverses at most once —
    a second reversal means this swipe length cannot settle the chip inside
    the bar's window, which a retry of the same gesture will not fix — and
    gives up as soon as a swipe leaves the chip's bounds unmoved, instead
    of burning the whole budget on a row that is not responding."""
    reversed_once = False
    previous = None
    for _ in range(max_swipes):
        bar = device.find(id="command-bar")
        if bar is None:
            return None
        bar_right = bar["bounds"][2]
        # A bar narrower than twice the margin would turn
        # `bar_right - margin -> margin` into a zero-width or backwards
        # gesture; a bar that narrow also cannot hide a chip off-window,
        # so treat it as unrevealable rather than flinging a degenerate
        # swipe.
        margin = min(80, (bar_right - 16) // 2)
        if margin < 16:
            return False
        node = device.find(id=target_id)
        if node is not None:
            left, top, right, bottom = node["bounds"]
            if left >= 0 and right <= bar_right:
                return True
            if previous is not None and all(
                abs(current - seen) <= 1
                for current, seen in zip((left, right), previous)
            ):
                # The last swipe moved the chip nothing measurable.
                return False
            previous = (left, right)
            row_y = (top + bottom) // 2
            if left < 0:
                if reversed_once:
                    return False
                reversed_once = True
                # The chip was carried past the row's left edge (the swipe
                # ends with fling momentum), and a further leftward swipe
                # only pushes it farther out — the containment test then
                # can never pass again and the loop exhausts. Swipe back
                # toward the row start instead, once.
                device.swipe(margin, row_y, bar_right - margin, row_y, 250)
                time.sleep(0.4)
                continue
        else:
            _, bar_top, _, bar_bottom = bar["bounds"]
            row_y = bar_top + (bar_bottom - bar_top) // 4
        device.swipe(bar_right - margin, row_y, margin, row_y, 250)
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
    # serve() — apps/remote/lapis_remote.py, the bind loop at the bottom of
    # the file — retries its bind forever (EADDRINUSE is caught like a
    # missing Tailscale, logged as "waiting to serve", and retried after
    # 5s; also reproduced locally in the round-8 review), so a stale --keep
    # gateway holding the port never surfaces through gateway.poll(): this
    # run's gateway stays alive waiting to bind. The only reliable
    # stale-owner test is whether the responder knows this run's freshly
    # minted agent ids — a stale fixture can echo the fixture titles but
    # never this run's uuids.
    run_ids = {agent["id"] for agent in agents}
    deadline = time.monotonic() + 10
    while True:
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:{PORT}/api/agents",
                headers={"X-Lapis-Client": "android"},
            )
            with urllib.request.urlopen(request, timeout=1) as response:
                listing = json.load(response)
            if not isinstance(listing, dict):
                # Whatever answered is not this repo's gateway (the stale
                # check exists precisely for a foreign owner on the port),
                # so force it down the cannot-know-this-run's-ids path
                # instead of crashing on a .get of a list or scalar.
                listing = {}
            answered = [
                item
                for category in listing.get("categories", ())
                if isinstance(category, dict)
                for item in category.get("agents", ())
                if isinstance(item, dict)
            ]
        except (OSError, ValueError):
            answered = None
        if answered is not None:
            if not any(item.get("id") in run_ids for item in answered):
                # The phone would drive the stale fixture's agents while the
                # Mac client watches this run's silent ones, and every
                # Mac-side check would fail against the wrong session;
                # refuse before any check runs.
                raise SystemExit(
                    f"a stale gateway owns 127.0.0.1:{PORT} (this run's is "
                    f"alive but cannot bind while it holds the port); stop "
                    f"it and rerun; see {run.logs / 'gateway.log'}"
                )
            if any(item.get("title") == "echo agent" for item in answered):
                # The gateway's own normalization of the registry, with the
                # mode field WireSession needs.
                return lapis_remote.load_workspace(str(registry))["agents"], gateway
        exit_code = gateway.poll()
        if exit_code is not None:
            raise SystemExit(
                f"the fixture gateway exited early with {exit_code}; "
                f"see {run.logs / 'gateway.log'}"
            )
        if time.monotonic() >= deadline:
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
        # The harness launch forces the bar on (the commandBarEnabled
        # extra overrides the stored setting, like the font-size extra), so
        # a missing bar is a product regression, not a runnable-state
        # precondition: fail it; there is no skip case.
        if device.find(id="command-bar") is None:
            return (
                "the command bar did not compose although the launch extra forced it on"
            )
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

    def check_settings_toggle():
        # The Settings switch's one device-visible effect: hide and show
        # the bar through the stored setting. Runs on a plain launch (no
        # commandBarEnabled extra) so no override masks the setting — every
        # other launch in this suite forces the bar on.
        def set_switch(on, already_open=False):
            if not already_open:
                device.tap_node(id="settings")
            switch = device.wait(id="command-bar-setting", timeout=10)
            if switch is None:
                return "the command bar switch never appeared"
            # The switch stays disabled until its stored-value read lands
            # (SettingsScreen gates it on readLanded), and a tap on a
            # disabled control is a silent no-op: poll for the node to
            # report itself enabled first. Dumps that never carry the
            # attribute fall through after the poll instead of wedging.
            deadline = time.monotonic() + 5
            while switch.get("enabled") is not True and time.monotonic() < deadline:
                time.sleep(0.2)
                switch = device.find(id="command-bar-setting")
                if switch is None:
                    return "the command bar switch vanished from the settings screen"
            checked = switch.get("checked")
            if checked is None:
                # Without a reported state the tap would be blind and could
                # invert an already-correct switch: fail as a harness-side
                # probe gap, not a product defect.
                return "the settings switch reported no checked state to the dump"
            if checked != on:
                device.tap_node(id="command-bar-setting")
                time.sleep(0.4)
                after = device.find(id="command-bar-setting")
                if after is None or after.get("checked") != on:
                    return "the settings switch did not flip after the tap"
            device.tap_node(text="Done")
            if device.wait(id="settings", timeout=10) is None:
                return "Done never returned to the workspace list"
            return None

        def open_stage(expect_bar):
            device.tap_node(text="echo agent")
            if device.wait(id="terminal", timeout=15) is None:
                return "the stage did not open"
            # The setting read lands a moment after composition, so poll
            # for the bar to settle rather than sampling once.
            deadline = time.monotonic() + 3
            present = device.find(id="command-bar") is not None
            while present != expect_bar and time.monotonic() < deadline:
                time.sleep(0.2)
                present = device.find(id="command-bar") is not None
            if present != expect_bar:
                return (
                    f"the command bar is {'visible' if present else 'absent'} "
                    f"but the setting says {'on' if expect_bar else 'off'}"
                )
            return None

        # set_switch's problems come in two shapes, and only one of them
        # may ride under a SKIPPED when the body's assertions passed.
        # Probe-shaped: the dump never established what to tap, or lost it
        # mid-probe — the restore could not act, and the next run's
        # ensure-on step re-aligns the stored setting. Effect-shaped (any
        # other message, including "did not flip" and "Done never
        # returned"): the restore acted and the product state did not
        # follow — the same persistence behavior the body asserts, and on
        # the body-passed path the only place it can still surface, so it
        # fails the run instead of hiding under a skip.
        PROBE_SHAPED = frozenset(
            (
                "the command bar switch never appeared",
                "the settings switch vanished from the settings screen",
                "the settings switch reported no checked state to the dump",
            )
        )

        device.launch(fresh=True, command_bar=None)
        if device.wait(text="echo agent", timeout=25) is None:
            return "the workspace list never came back"
        problem: str | None = None
        restore_problem: str | None = None
        try:
            try:
                # From a known-on state: the read path shows the bar...
                problem = set_switch(True)
                if problem is None:
                    problem = open_stage(expect_bar=True)
                if problem is None:
                    shot("13-bar-shown")
                    # ...and hiding it through Settings removes it on re-entry.
                    device.tap_node(id="back")
                    problem = set_switch(False)
                if problem is None:
                    problem = open_stage(expect_bar=False)
                if problem is None:
                    shot("14-bar-hidden")
            except Exception as error:
                # The taps above raise when a screen wedges mid-check. The
                # restore below still runs; folding the raise into the
                # body's diagnosis keeps the receipt complete on this path
                # too, because a raise escaping the try would discard
                # restore_problem and record only the bare device error.
                problem = f"the check raised {type(error).__name__}: {error}"
        finally:
            # Leave the setting on whatever happened above: a failed run
            # must not leave the device stored-off for the next
            # store-honoring launch (this check's own next run).
            try:
                if device.find(id="terminal") is not None:
                    device.tap_node(id="back")
                    time.sleep(0.3)
                # Positive marker only: the settings screen owns its own
                # tag, present whatever became of the switch inside it.
                # Its absence means "not on the settings screen" — the
                # stage, a dialog, or a wedged dump — which says nothing
                # about the switch; on those screens the restore must
                # navigate first instead of waiting on a switch that
                # cannot appear there. The previous shape (the workspace
                # list's settings button being gone) was true on every one
                # of those screens too, so a wedged stage made the restore
                # skip the navigation and report the wrong "switch never
                # appeared" diagnosis.
                already_open = device.find(id="settings-screen") is not None
                restore_problem = set_switch(True, already_open=already_open)
            except Exception as error:
                # The whole cleanup is guarded, not just set_switch: a
                # raise escaping the finally would replace the body's
                # diagnosis with the cleanup's own error.
                restore_problem = f"restore raised {type(error).__name__}: {error}"
        if restore_problem is not None:
            # Neutral wording on purpose: a restore problem can occur with
            # the setting already stored on (a lost Done tap), so this
            # reports what went unconfirmed, never a state it cannot know.
            restore_note = (
                f"the settings-toggle restore did not confirm: {restore_problem}"
            )
            if problem is None and restore_problem in PROBE_SHAPED:
                # The product assertions passed and the restore could not
                # even act — a harness condition, not a defect verdict. The
                # SKIPPED prefix keeps it in the receipt without failing
                # the run, the file's convention for exactly this split,
                # and the next run's ensure-on first step re-aligns the
                # stored setting. Every other restore problem fails below:
                # an unconfirmed effect (the switch did not flip, Done did
                # not return) or a raise is a persistence signal this
                # check owns, and the receipt's failed list is where a
                # gate reads it.
                return f"SKIPPED: {restore_note}"
            return (
                restore_note if problem is None else f"{problem}; also, {restore_note}"
            )
        return problem

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
        "settings_toggle": check_settings_toggle,
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
    try:
        phone_locked = device.locked()
    except (android_ctl.DeviceError, subprocess.TimeoutExpired) as error:
        # Mirror the adb-devices timeout handling above: this probe has no
        # fixture to clean up, but a hung or failing adb should still report
        # a named condition instead of a raw traceback.
        print(
            f"could not read the lock state ({type(error).__name__}: {error}); "
            "restart the adb server and rerun",
            file=sys.stderr,
        )
        return 1
    if phone_locked:
        # The app launches behind the keyguard and its networking still
        # works, so a locked phone produces a full run of misleading
        # failures (missing composer, "command bar disabled in settings")
        # instead of the real cause. Fail before spawning any fixture.
        print(
            "the phone is locked; unlock it and rerun — UI checks cannot "
            "see behind the keyguard",
            file=sys.stderr,
        )
        return 1

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
            except android_ctl.Skipped as skip:
                # A harness condition the check cannot run through (not a
                # product failure): keep the bare "SKIPPED:" message so the
                # classifier below records it as skipped instead of failed.
                problem = str(skip)
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
