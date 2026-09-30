#!/usr/bin/env python3
"""Drive the lapis Android app over adb: the control panel for the device.

One tool for scripted and ad-hoc device work, mirroring what the iOS side
gets from XCUITest plus what adb adds (install, reverse tunnels, wm overrides,
logcat, gfx stats). Selectors use the app's test tags (exposed as resource
ids) first, so scripts do not depend on pixel coordinates:

    python3 tools/qa/android_ctl.py launch --host 127.0.0.1:7351 --fresh
    python3 tools/qa/android_ctl.py wait --text "echo agent" --timeout 20
    python3 tools/qa/android_ctl.py tap --text "echo agent"
    python3 tools/qa/android_ctl.py tap --id composer
    python3 tools/qa/android_ctl.py text "hello from the phone"
    python3 tools/qa/android_ctl.py tap --id send
    python3 tools/qa/android_ctl.py term            # the rendered screen text
    python3 tools/qa/android_ctl.py screenshot out.png

Verbs: state, ui, find, tap, text, key, swipe, scroll, screenshot, launch,
stop, log, crash, reverse, wait, term, wm, gfx. Progress goes to stderr;
results print to stdout (--json where structured). Exit codes: 0 ok,
1 not found or timed out, 2 adb or device error, 3 usage.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = "dev.lapis.remote"
ACTIVITY = "dev.lapis.remote/dev.lapis.remote.ui.MainActivity"

DUMP_REMOTE = "/sdcard/lapis_ui_dump.xml"
SHOT_REMOTE = "/sdcard/lapis_shot.png"

KEYCODES = {
    "ENTER": "66",
    "BACK": "4",
    "HOME": "3",
    "APP_SWITCH": "187",
    "TAB": "61",
    "DEL": "67",
    "SPACE": "62",
    "UP": "19",
    "DOWN": "20",
    "LEFT": "21",
    "RIGHT": "22",
}


def find_sdk() -> Path | None:
    """The Android SDK, from the environment, the default location, or
    apps/android/local.properties."""
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(name)
        if value and (Path(value) / "platform-tools").is_dir():
            return Path(value)
    default = Path.home() / "Library" / "Android" / "sdk"
    if (default / "platform-tools").is_dir():
        return default
    local = ROOT / "apps" / "android" / "local.properties"
    if local.is_file():
        for line in local.read_text().splitlines():
            if line.startswith("sdk.dir="):
                candidate = Path(line.removeprefix("sdk.dir=").strip())
                if (candidate / "platform-tools").is_dir():
                    return candidate
    return None


def shell_quote(text: str) -> str:
    """One argument quoted for the device's /bin/sh, which adb shell uses."""
    return "'" + text.replace("'", "'\\''") + "'"


def escape_input_text(text: str) -> str:
    """`input text` takes no spaces (%s) and its argument must survive the
    device shell; %s in the original text becomes a space (known limit)."""
    return shell_quote(text.replace(" ", "%s"))


def parse_bounds(raw: str | None) -> tuple[int, int, int, int] | None:
    if not raw or not raw.startswith("["):
        return None
    digits = [int(part) for part in raw.strip("[]").replace("][", ",").split(",")]
    if len(digits) != 4:
        return None
    return digits[0], digits[1], digits[2], digits[3]


def bounds_center(bounds: tuple[int, int, int, int]) -> tuple[int, int]:
    left, top, right, bottom = bounds
    return (left + right) // 2, (top + bottom) // 2


def parse_dump(xml_text: str) -> list[dict]:
    """Flat node list from a uiautomator dump; empty text/desc attrs dropped."""
    root = ElementTree.fromstring(xml_text)
    nodes = []
    for element in root.iter("node"):
        bounds = parse_bounds(element.get("bounds"))
        if bounds is None:
            continue
        node = {"bounds": list(bounds), "clickable": element.get("clickable") == "true"}
        for key, attribute in (
            ("id", "resource-id"),
            ("text", "text"),
            ("desc", "content-desc"),
            ("class", "class"),
        ):
            value = element.get(attribute) or ""
            if value:
                node[key] = value
        nodes.append(node)
    return nodes


def select(
    nodes: list[dict],
    *,
    id: str | None = None,
    text: str | None = None,
    desc: str | None = None,
    contains: str | None = None,
) -> list[dict]:
    """Nodes matching every given selector. `contains` matches any string
    attribute; exact selectors compare whole values."""

    def wanted(node: dict) -> bool:
        if id is not None and node.get("id") != id:
            return False
        if text is not None and node.get("text") != text:
            return False
        if desc is not None and node.get("desc") != desc:
            return False
        if contains is not None and not any(
            contains in node.get(key, "") for key in ("text", "desc", "id")
        ):
            return False
        return True

    return [node for node in nodes if wanted(node)]


def keycode(name: str) -> str:
    return KEYCODES.get(name.upper(), name)


class DeviceError(RuntimeError):
    pass


class Skipped(DeviceError):
    """A harness condition, not a product failure: the check could not run
    (unreadable device state), so the receipt must record it as skipped.
    The runner catches this before DeviceError and uses the bare message,
    which starts with "SKIPPED:" to fit its existing classification."""

    pass


class Device:
    """One adb device; every shell line is one checked subprocess call."""

    def __init__(self, serial: str | None = None, package: str = PACKAGE):
        sdk = find_sdk()
        if sdk is None:
            raise DeviceError("Android SDK not found; set ANDROID_HOME")
        self.adb = str(sdk / "platform-tools" / "adb")
        self.serial = serial
        self.package = package

    def run(self, *args: str, timeout: float = 30) -> str:
        command = [self.adb]
        if self.serial:
            command += ["-s", self.serial]
        command += list(args)
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=timeout, check=False
        )
        if result.returncode != 0:
            raise DeviceError(
                f"adb {' '.join(args)} failed: {result.stdout}{result.stderr}".strip()
            )
        return result.stdout

    def shell(self, command: str, timeout: float = 30) -> str:
        return self.run("shell", command, timeout=timeout)

    # -- UI -----------------------------------------------------------------

    def ui(self, retries: int = 4) -> list[dict]:
        """A fresh uiautomator dump; the dump service refuses while the screen
        is animating, so transient failures retry."""
        for attempt in range(retries):
            output = self.shell(f"uiautomator dump {DUMP_REMOTE}")
            if "dumped to" not in output:
                if attempt + 1 == retries:
                    raise DeviceError(f"uiautomator dump failed: {output.strip()}")
                time.sleep(0.4)
                continue
            handle, name = tempfile.mkstemp(suffix=".xml")
            try:
                os.close(handle)
                self.run("pull", DUMP_REMOTE, name)
                xml_text = Path(name).read_text(errors="replace")
            finally:
                Path(name).unlink(missing_ok=True)
                # Also when the pull failed: a leftover on /sdcard lingers
                # past a crashed run and could be mistaken for a fresh dump.
                self.shell(f"rm {DUMP_REMOTE}")
            return parse_dump(xml_text)
        raise DeviceError("unreachable")

    def find(
        self,
        *,
        id: str | None = None,
        text: str | None = None,
        desc: str | None = None,
        contains: str | None = None,
    ) -> dict | None:
        nodes = select(self.ui(), id=id, text=text, desc=desc, contains=contains)
        return nodes[0] if nodes else None

    def wait(
        self,
        *,
        id: str | None = None,
        text: str | None = None,
        desc: str | None = None,
        contains: str | None = None,
        timeout: float = 10.0,
        interval: float = 0.5,
    ) -> dict | None:
        deadline = time.monotonic() + timeout
        while True:
            node = self.find(id=id, text=text, desc=desc, contains=contains)
            if node is not None:
                return node
            if time.monotonic() >= deadline:
                return None
            time.sleep(interval)

    def wait_gone(
        self,
        *,
        id: str | None = None,
        text: str | None = None,
        desc: str | None = None,
        contains: str | None = None,
        timeout: float = 10.0,
        interval: float = 0.5,
    ) -> bool:
        deadline = time.monotonic() + timeout
        while True:
            if self.find(id=id, text=text, desc=desc, contains=contains) is None:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(interval)

    def tap(self, x: int, y: int) -> None:
        self.shell(f"input tap {x} {y}")

    def tap_node(
        self,
        *,
        id: str | None = None,
        text: str | None = None,
        desc: str | None = None,
        contains: str | None = None,
    ) -> dict:
        node = self.find(id=id, text=text, desc=desc, contains=contains)
        if node is None:
            raise DeviceError(f"no element matches {id or text or desc or contains}")
        self.tap(*bounds_center(tuple(node["bounds"])))
        return node

    def type_text(self, text: str) -> None:
        self.shell(f"input text {escape_input_text(text)}")

    def key(self, name: str) -> None:
        self.shell(f"input keyevent {keycode(name)}")

    def locked(self) -> bool:
        """Whether the keyguard (or its emergency dialer) is showing. A
        locked phone still launches the app behind the keyguard, so every
        UI check then fails with misleading messages (missing composer,
        "command bar disabled") instead of the real cause. Two window
        flags drive the decision: isKeyguardShowing is the purpose-built
        keyguard signal, and the dreaming flag additionally catches
        keyguard-adjacent overlays (both verified true on the target
        build with the display on and off). The emergency dialer is a
        real activity composing over the keyguard and can clear those
        flags while the phone stays locked, so the resumed activity is
        inspected too — matched per line (the 300-char slice after a
        marker substring could spill into unrelated sections) and
        case-insensitively (the components are CamelCase; this build
        reports "Resumed:"/"ResumedActivity:" lines where AOSP logs
        "topResumedActivity="). Build-specific fields that vanish read
        as unlocked, preserving the pre-guard behavior."""
        window = self.shell("dumpsys window")
        if "isKeyguardShowing=true" in window or "mDreamingLockscreen=true" in window:
            return True
        for line in self.shell("dumpsys activity activities").splitlines():
            stripped = line.strip()
            if not stripped.startswith(
                ("topResumedActivity=", "ResumedActivity:", "Resumed:")
            ):
                continue
            lowered = stripped.lower()
            if "emergency" in lowered or "keyguard" in lowered:
                return True
        return False

    def ime_shown(self) -> bool:
        """Whether the on-screen keyboard is currently covering the stage.

        Fails closed: `mInputShown` is a build-specific dumpsys field, so a
        dump that carries no IME state at all raises instead of silently
        reporting "hidden" — a renamed or trimmed field would otherwise skip
        the BACK press, leave the keyboard up, and blame the app for the
        harness's blind scroll. Raised as Skipped with a "SKIPPED:" message:
        the app was never exercised when the harness cannot read IME state,
        so the receipt records a skipped check, not a product failure.
        """
        dump = self.shell("dumpsys input_method")
        if "mInputShown=" not in dump:
            raise Skipped(
                "SKIPPED: dumpsys input_method reported no IME state; "
                "the keyboard state is unreadable, so the scroll cannot "
                "be attempted"
            )
        return "mInputShown=true" in dump

    def dismiss_ime(self) -> None:
        """Drop the on-screen keyboard a previous composer use left open.

        The stage's imePadding shrinks the terminal to the strip above the
        keyboard, so scroll gestures aimed at screen center land on the
        keyboard and never reach the list. BACK dismisses only the keyboard
        while it is shown; it is sent only under that condition and only
        after dumpsys confirms, because a BACK with the keyboard already
        down would navigate out of the stage.
        """
        if not self.ime_shown():
            return
        self.key("BACK")
        for _ in range(10):
            if not self.ime_shown():
                return
            time.sleep(0.3)
        raise DeviceError("the on-screen keyboard did not dismiss")

    def swipe(self, x1: int, y1: int, x2: int, y2: int, ms: int = 300) -> None:
        self.shell(f"input swipe {x1} {y1} {x2} {y2} {ms}")

    def scroll(self, direction: str, count: int = 1, ms: int = 320) -> list[int]:
        """Fling inside the terminal (or screen center) toward older content
        (finger down) or newer (finger up); returns each gesture's span."""
        size = self.shell("wm size")
        # An override (a test's `wm size WxH`) wins over the physical panel;
        # it is reported on its own line after the physical one.
        lines = size.splitlines()
        # Each candidate separately: next()'s default expression evaluates
        # eagerly, so a nested default would raise StopIteration on a dump
        # that has an Override but no Physical line before finding either.
        line = next(
            (part for part in lines if part.startswith("Override")), None
        ) or next((part for part in lines if part.startswith("Physical")), None)
        if line is None:
            raise DeviceError(
                f"wm size reported neither override nor physical: {size.strip()!r}"
            )
        width, height = (int(value) for value in line.split(":")[1].strip().split("x"))
        spans = []
        x = width // 2
        middle = height // 2
        span = height // 4
        for _ in range(count):
            if direction == "older":
                self.swipe(x, middle - span // 2, x, middle + span, ms)
            else:
                self.swipe(x, middle + span // 2, x, middle - span, ms)
            spans.append(span)
            time.sleep(0.25)
        return spans

    def terminal_text(self) -> str:
        """The terminal's rendered text through its content description."""
        node = self.find(id="terminal")
        if node is None:
            raise DeviceError("no terminal on screen")
        return node.get("desc", "")

    def screenshot(self, out: Path) -> Path:
        # Samsung pollutes `exec-out screencap` stdout with warnings; the
        # /sdcard file plus pull is clean.
        self.shell(f"screencap -p {SHOT_REMOTE}")
        try:
            self.run("pull", SHOT_REMOTE, str(out))
        finally:
            self.shell(f"rm {SHOT_REMOTE}")
        return out

    # -- App lifecycle ------------------------------------------------------

    def launch(
        self,
        host: str | None = None,
        font: float | None = None,
        reset_cache: bool = False,
        fresh: bool = False,
    ) -> None:
        if fresh:
            self.stop()
        command = f"am start -n {ACTIVITY}"
        if host is not None:
            command += f" --es gatewayHost {shell_quote(host)}"
        if font is not None:
            command += f" --es terminalFontSize {font}"
        if reset_cache:
            command += " --ez resetCache true"
        self.shell(command)

    def stop(self) -> None:
        self.shell(f"am force-stop {self.package}")

    def pid(self) -> str | None:
        # pidof exits 1 with no output when the process is not running.
        try:
            output = self.shell(f"pidof {self.package}").strip()
        except DeviceError:
            return None
        return output or None

    def crash_lines(self) -> list[str]:
        """FATAL/AndroidRuntime lines for this package since boot."""
        output = self.run("logcat", "-d", "-b", "crash")
        lines = [line for line in output.splitlines() if self.package in line]
        return lines

    # -- Environment --------------------------------------------------------

    def reverse(self, port: int) -> None:
        self.run("reverse", f"tcp:{port}", f"tcp:{port}")

    def unreverse(self, port: int) -> None:
        self.run("reverse", "--remove", f"tcp:{port}")

    def state(self) -> dict:
        def prop(name: str) -> str:
            return self.shell(f"getprop {name}").strip()

        focus = ""
        for line in self.shell("dumpsys window").splitlines():
            if "mCurrentFocus" in line:
                focus = line.split("mCurrentFocus")[-1].strip()
                break
        installed = self.shell(f"dumpsys package {self.package}")
        version = next(
            (
                line.split("=", 1)[1].strip()
                for line in installed.splitlines()
                if line.strip().startswith("versionName=")
            ),
            "",
        )
        return {
            "serial": self.serial,
            "model": prop("ro.product.model"),
            "android": prop("ro.build.version.release"),
            "size": self.shell("wm size").strip().splitlines(),
            "density": self.shell("wm density").strip().splitlines()[-1],
            "focus": focus,
            "app_version": version,
            "app_pid": self.pid(),
        }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", help="adb serial; the default is the only device")
    parser.add_argument("--app", default=PACKAGE, help=f"package (default {PACKAGE})")
    sub = parser.add_subparsers(dest="verb", required=True)

    sub.add_parser("state", help="device and app state as JSON")

    ui = sub.add_parser("ui", help="the current screen's elements")
    ui.add_argument("--json", action="store_true")

    finder = sub.add_parser("find", help="first element matching the selectors")
    for flag in ("--id", "--text", "--desc", "--contains"):
        finder.add_argument(flag)
    finder.add_argument("--json", action="store_true")

    tap = sub.add_parser("tap", help="tap a selector or explicit coordinates")
    for flag in ("--id", "--text", "--desc", "--contains"):
        tap.add_argument(flag)
    tap.add_argument("xy", nargs="*", help="x y instead of selectors")

    text = sub.add_parser("text", help="type into the focused field")
    text.add_argument("value")

    key = sub.add_parser("key", help="press a key (ENTER, BACK, HOME, ...)")
    key.add_argument("name")

    swipe = sub.add_parser("swipe", help="input swipe")
    swipe.add_argument("x1", type=int)
    swipe.add_argument("y1", type=int)
    swipe.add_argument("x2", type=int)
    swipe.add_argument("y2", type=int)
    swipe.add_argument("ms", type=int, nargs="?", default=300)

    scroll = sub.add_parser("scroll", help="scroll the terminal older or newer")
    scroll.add_argument("direction", choices=["older", "newer"])
    scroll.add_argument("count", type=int, nargs="?", default=1)

    shot = sub.add_parser("screenshot", help="capture the screen to a PNG")
    shot.add_argument("out")

    launch = sub.add_parser("launch", help="start the app, with debug overrides")
    launch.add_argument("--host")
    launch.add_argument("--font", type=float)
    launch.add_argument("--reset-cache", action="store_true")
    launch.add_argument("--fresh", action="store_true", help="force-stop first")

    sub.add_parser("stop", help="force-stop the app")

    log = sub.add_parser("log", help="the app's logcat lines")
    log.add_argument("--lines", type=int, default=80)

    sub.add_parser("crash", help="exit 1 with the trace when the app crashed")

    rev = sub.add_parser("reverse", help="adb reverse tcp:PORT")
    rev.add_argument("port", type=int)
    rev.add_argument("--remove", action="store_true")

    wait = sub.add_parser("wait", help="wait for an element (or its absence)")
    for flag in ("--id", "--text", "--desc", "--contains"):
        wait.add_argument(flag)
    wait.add_argument("--timeout", type=float, default=10.0)
    wait.add_argument("--gone", action="store_true", help="wait until absent")

    term = sub.add_parser("term", help="the terminal's rendered text")
    term.add_argument("--contains")

    wm = sub.add_parser("wm", help="passthrough: wm size 908x2316 | size reset ...")
    wm.add_argument("args", nargs="+")

    gfx = sub.add_parser("gfx", help="dumpsys gfxinfo for the app")
    gfx.add_argument("args", nargs="*", help="e.g. framestats, reset")

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        device = Device(serial=args.serial, package=args.app)
        selectors = {
            key: getattr(args, key)
            for key in ("id", "text", "desc", "contains")
            if hasattr(args, key) and getattr(args, key) is not None
        }

        if args.verb == "state":
            print(json.dumps(device.state(), indent=1))
        elif args.verb == "ui":
            nodes = device.ui()
            if args.json:
                print(json.dumps(nodes, indent=1))
            else:
                for node in nodes:
                    label = node.get("id") or node.get("text") or node.get("class")
                    print(f"{label} {node['bounds']}")
        elif args.verb == "find":
            node = device.find(**selectors)
            if node is None:
                print("not found", file=sys.stderr)
                return 1
            print(json.dumps(node, indent=1) if args.json else str(node["bounds"]))
        elif args.verb == "tap":
            if args.xy:
                if len(args.xy) != 2:
                    print("tap wants x y", file=sys.stderr)
                    return 3
                device.tap(int(args.xy[0]), int(args.xy[1]))
            else:
                device.tap_node(**selectors)
        elif args.verb == "text":
            device.type_text(args.value)
        elif args.verb == "key":
            device.key(args.name)
        elif args.verb == "swipe":
            device.swipe(args.x1, args.y1, args.x2, args.y2, args.ms)
        elif args.verb == "scroll":
            device.scroll(args.direction, args.count)
        elif args.verb == "screenshot":
            out = Path(args.out)
            out.parent.mkdir(parents=True, exist_ok=True)
            device.screenshot(out)
            print(str(out))
        elif args.verb == "launch":
            device.launch(
                host=args.host,
                font=args.font,
                reset_cache=args.reset_cache,
                fresh=args.fresh,
            )
        elif args.verb == "stop":
            device.stop()
        elif args.verb == "log":
            pid = device.pid()
            if pid is None:
                print("not running", file=sys.stderr)
                return 1
            print(device.run("logcat", "-d", f"--pid={pid}", "-t", str(args.lines)))
        elif args.verb == "crash":
            crashes = device.crash_lines()
            if crashes:
                print("\n".join(crashes), file=sys.stderr)
                return 1
            print("no crashes")
        elif args.verb == "reverse":
            if args.remove:
                device.unreverse(args.port)
            else:
                device.reverse(args.port)
        elif args.verb == "wait":
            if args.gone:
                if device.wait_gone(timeout=args.timeout, **selectors):
                    return 0
                print("still present", file=sys.stderr)
                return 1
            node = device.wait(timeout=args.timeout, **selectors)
            if node is None:
                print("timed out", file=sys.stderr)
                return 1
            print(json.dumps(node))
        elif args.verb == "term":
            value = device.terminal_text()
            if args.contains and args.contains not in value:
                print(f"terminal does not contain {args.contains!r}", file=sys.stderr)
                return 1
            print(value)
        elif args.verb == "wm":
            print(
                device.shell("wm " + " ".join(shell_quote(part) for part in args.args))
            )
        elif args.verb == "gfx":
            quoted = " ".join(shell_quote(part) for part in args.args)
            print(device.shell(f"dumpsys gfxinfo {device.package} " + quoted))
        return 0
    except DeviceError as error:
        print(str(error), file=sys.stderr)
        return 2
    except subprocess.TimeoutExpired:
        print("adb timed out", file=sys.stderr)
        return 2
    except Exception as error:
        # The documented contract: 0 answered, 1 answered no, 2 tool failure.
        # An unexpected error is a tool failure, not a traceback and exit 1.
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
