#!/usr/bin/env python3
"""Drive the real lapis GUI with synthetic X11 input and try to break it.

Run inside the isolated display, for example on anvil:

  uv run --no-project python scripts/lapis.py linux-gui \
    uv run --no-project --with python-xlib python tools/qa/breakit.py

Agents are tools/qa/fake_agent.py installed under harness names on a private
PATH. Each scenario records screenshots, process and registry checks, and GUI
warnings under build/qa/<run>/; receipt.json summarizes pass/fail.
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
import traceback
from pathlib import Path

from Xlib import X, XK, display
from Xlib.ext import xtest

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build/desktop/apps/desktop/lapis_desktop"
REGISTRY = ROOT / "runtime/workspace.json"
CONFIG = ROOT / "lapis.json"
FAKE_NAMES = ("omp", "grok", "kimi", "opencode", "gemini", "agy")
CATALOG = ("codex", "claude", "omp", "grok", "kimi", "opencode", "gemini", "agy")
WARNING = re.compile(
    r"qml|TypeError|ReferenceError|Binding loop|Unable to assign|QQmlComponent|"
    r"ASSERT|Segmentation|terminate called|QObject::|QThread|undefined",
    re.IGNORECASE,
)
SYMBOLS = {
    " ": "space",
    "/": "slash",
    "-": "minus",
    "_": "underscore",
    ".": "period",
    "~": "asciitilde",
    ",": "comma",
    ":": "colon",
    "'": "apostrophe",
    '"': "quotedbl",
    "!": "exclam",
    "?": "question",
    "=": "equal",
    "+": "plus",
    "[": "bracketleft",
    "]": "bracketright",
    "(": "parenleft",
    ")": "parenright",
    "\n": "Return",
}


class Failure(Exception):
    pass


class Keyboard:
    def __init__(self):
        self.display = display.Display()

    def code(self, name):
        keysym = XK.string_to_keysym(name)
        if keysym == 0:
            raise Failure(f"unknown key {name}")
        codes = list(self.display.keysym_to_keycodes(keysym))
        if not codes:
            raise Failure(f"no keycode for {name}")
        keycode, index = codes[0]
        return keycode, index % 2 == 1

    def raw(self, keycode, down):
        xtest.fake_input(self.display, X.KeyPress if down else X.KeyRelease, keycode)
        self.display.sync()

    def combo(self, text, pause=0.12):
        """Press a chord such as ctrl+shift+n."""
        names = {
            "ctrl": "Control_L",
            "shift": "Shift_L",
            "alt": "Alt_L",
            "super": "Super_L",
        }
        parts = text.split("+")
        modifiers = [self.code(names[part])[0] for part in parts[:-1]]
        key = parts[-1]
        keycode, shifted = self.code(SYMBOLS.get(key, key) if len(key) == 1 else key)
        if shifted and "shift" not in parts[:-1]:
            modifiers.append(self.code("Shift_L")[0])
        for modifier in modifiers:
            self.raw(modifier, True)
        self.raw(keycode, True)
        self.raw(keycode, False)
        for modifier in reversed(modifiers):
            self.raw(modifier, False)
        time.sleep(pause)

    def type(self, text, pause=0.02):
        for char in text:
            name = SYMBOLS.get(char, char)
            keycode, shifted = self.code(name)
            if shifted:
                self.raw(self.code("Shift_L")[0], True)
            self.raw(keycode, True)
            self.raw(keycode, False)
            if shifted:
                self.raw(self.code("Shift_L")[0], False)
            time.sleep(pause)

    def click(self, x, y):
        xtest.fake_input(self.display, X.MotionNotify, x=x, y=y)
        xtest.fake_input(self.display, X.ButtonPress, 1)
        xtest.fake_input(self.display, X.ButtonRelease, 1)
        self.display.sync()
        time.sleep(0.2)

    def lapis_window(self):
        def walk(window):
            try:
                name = window.get_wm_name() or ""
                klass = window.get_wm_class() or ()
                attributes = window.get_attributes()
            except Exception:  # noqa: BLE001 - windows vanish while walking
                return None
            if ("lapis" in name.lower() or any("lapis" in k for k in klass)) and (
                attributes.map_state == X.IsViewable
            ):
                geometry = window.get_geometry()
                if geometry.width > 300:
                    return window
            for child in window.query_tree().children:
                found = walk(child)
                if found is not None:
                    return found
            return None

        return walk(self.display.screen().root)

    def resize(self, width, height):
        window = self.lapis_window()
        if window is None:
            raise Failure("no lapis window to resize")
        target = window
        # Openbox reparents clients; resize the client, the frame follows.
        target.configure(width=width, height=height)
        self.display.sync()
        time.sleep(1.0)


class Run:
    def __init__(self, output, keep):
        self.output = output
        self.keep = keep
        self.keys = Keyboard()
        self.gui = None
        self.gui_log = output / "gui.log"
        self.results = []
        self.shots = 0
        self.bin = output / "bin"
        self.project = output / "project"
        self.config = None

    # Environment -------------------------------------------------------------

    def prepare(self):
        self.output.mkdir(parents=True, exist_ok=True)
        self.bin.mkdir(exist_ok=True)
        self.project.mkdir(exist_ok=True)
        (self.project / "nested").mkdir(exist_ok=True)
        agent = ROOT / "tools/qa/fake_agent.py"
        for name in FAKE_NAMES:
            link = self.bin / name
            if not link.exists():
                link.symlink_to(agent)
        agent.chmod(0o755)
        if REGISTRY.exists() and not self.keep:
            raise Failure(
                f"{REGISTRY} exists; pass --keep or remove the QA runtime first"
            )
        # The app persists sidebar and appearance changes into lapis.json.
        self.config = CONFIG.read_bytes() if CONFIG.exists() else None

    def environment(self):
        env = dict(os.environ)
        env["PATH"] = f"{self.bin}:{env.get('PATH', '')}"
        env.setdefault("LAPIS_FAKE_BURST", "0")
        return env

    def launch(self):
        with self.gui_log.open("a") as log:
            log.write(f"\n=== launch {time.strftime('%H:%M:%S')} ===\n")
        log = self.gui_log.open("a")
        self.gui = subprocess.Popen(
            [str(BINARY)],
            env=self.environment(),
            stdout=log,
            stderr=subprocess.STDOUT,
            cwd=str(ROOT),
            start_new_session=True,
        )
        self.wait(
            lambda: self.keys.lapis_window() is not None, 20, "lapis window appears"
        )
        time.sleep(2.0)
        window = self.keys.lapis_window()
        geometry = window.get_geometry()
        self.keys.click(geometry.x + 40 + geometry.width // 2, 200)

    def gui_alive(self):
        return self.gui is not None and self.gui.poll() is None

    # Observation --------------------------------------------------------------

    def shot(self, label):
        self.shots += 1
        path = self.output / f"{self.shots:03d}-{label}.png"
        subprocess.run(
            ["import", "-window", "root", str(path)], check=False, capture_output=True
        )
        return path.name

    def registry(self):
        try:
            return json.loads(REGISTRY.read_text())
        except (OSError, ValueError) as error:
            raise Failure(f"registry unreadable: {error}") from error

    def agents(self):
        return self.registry().get("agents", [])

    def processes(self):
        result = subprocess.run(
            ["ps", "-eo", "pid=,ppid=,stat=,args="],
            capture_output=True,
            text=True,
            check=True,
        )
        rows = []
        for line in result.stdout.splitlines():
            parts = line.split(None, 3)
            if len(parts) == 4:
                rows.append(
                    {
                        "pid": int(parts[0]),
                        "ppid": int(parts[1]),
                        "stat": parts[2],
                        "args": parts[3],
                    }
                )
        return rows

    def services(self):
        root = str(ROOT / "runtime")
        return [
            row
            for row in self.processes()
            if "lapis_session_service" in row["args"] and root in row["args"]
        ]

    def fake_agents(self):
        # The shebang runs each agent as "python3 <bin>/<name>".
        names = tuple(f"{self.bin}/{name}" for name in FAKE_NAMES)
        return [
            row
            for row in self.processes()
            if "lapis_session_service" not in row["args"]
            and any(name in row["args"].split() for name in names)
        ]

    def warnings(self):
        if not self.gui_log.exists():
            return []
        return [
            line.strip()
            for line in self.gui_log.read_text(errors="replace").splitlines()
            if WARNING.search(line) and "=== launch" not in line
        ]

    def wait(self, condition, seconds, label):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                if condition():
                    return
            except Failure:
                pass
            time.sleep(0.2)
        raise Failure(f"timed out: {label}")

    # Actions ------------------------------------------------------------------

    def new_agent(self, harness, directory=None):
        before = len(self.agents()) if REGISTRY.exists() else 0
        self.keys.combo("ctrl+shift+n")
        time.sleep(0.6)
        for _ in range(len(CATALOG)):
            self.keys.combo("Up", 0.03)
        for _ in range(CATALOG.index(harness)):
            self.keys.combo("Down", 0.05)
        self.keys.combo("Return", 0.5)
        self.keys.combo("ctrl+a")
        self.keys.type(str(directory or self.project))
        time.sleep(0.4)
        self.keys.combo("Return", 0.8)
        self.wait(
            lambda: len(self.agents()) == before + 1, 10, f"{harness} agent registered"
        )
        self.wait(
            lambda: len(self.fake_agents()) >= self.expected_fakes(),
            10,
            f"{harness} process running",
        )

    def expected_fakes(self):
        return sum(1 for agent in self.agents() if agent.get("harness") in FAKE_NAMES)

    def send(self, text):
        self.keys.type(text)
        self.keys.combo("Return", 0.3)

    def next_agent(self):
        self.keys.combo("ctrl+shift+bracketright", 0.3)

    def previous_agent(self):
        self.keys.combo("ctrl+shift+bracketleft", 0.3)

    def close_agent(self):
        self.keys.combo("ctrl+shift+w", 0.6)
        self.keys.combo("Return", 0.3)

    def new_category(self, name):
        self.keys.combo("ctrl+shift+alt+n", 0.6)
        self.keys.type(name)
        self.keys.combo("Return", 0.6)

    # Scenario bookkeeping -----------------------------------------------------

    def scenario(self, name, function):
        started = time.monotonic()
        warnings_before = len(self.warnings())
        record = {"name": name}
        try:
            detail = function() or {}
            record.update(detail)
            if not self.gui_alive() and not record.get("gui_may_exit"):
                raise Failure("GUI exited")
            record["passed"] = True
        except Exception as error:  # noqa: BLE001 - every failure is recorded
            record["passed"] = False
            record["error"] = f"{type(error).__name__}: {error}"
            record["traceback"] = traceback.format_exc(limit=4)
            record["screenshot"] = self.shot(f"{name}-failure")
            if not self.gui_alive():
                record["gui_exit"] = self.gui.returncode if self.gui else None
        record["new_warnings"] = self.warnings()[warnings_before:][:30]
        record["seconds"] = round(time.monotonic() - started, 1)
        self.results.append(record)
        status = "PASS" if record["passed"] else "FAIL"
        print(
            f"{status} {name} ({record['seconds']}s) {record.get('error', '')}",
            flush=True,
        )
        if not self.gui_alive():
            self.launch()

    # Scenarios ------------------------------------------------------------------

    def s_empty(self):
        return {"screenshot": self.shot("empty")}

    def s_create(self):
        for harness in ("kimi", "grok", "opencode"):
            self.new_agent(harness)
        time.sleep(1.5)
        return {"agents": len(self.agents()), "screenshot": self.shot("three-agents")}

    def s_type_and_switch(self):
        for word in ("hello", "wide", "title"):
            self.send(word)
            time.sleep(0.8)
            self.next_agent()
        time.sleep(1.0)
        return {"screenshot": self.shot("typed-and-switched")}

    def s_finish_pulse(self):
        self.send("slow")
        time.sleep(3.0)
        self.next_agent()
        shot_working = self.shot("slow-working-unselected")
        time.sleep(24.0)
        return {"screenshots": [shot_working, self.shot("slow-finished-should-pulse")]}

    def s_flood_while_typing(self):
        self.send("flood")
        self.next_agent()
        started = time.monotonic()
        self.send("typing while a neighbour floods")
        elapsed = time.monotonic() - started
        time.sleep(4.0)
        return {
            "typing_seconds": round(elapsed, 2),
            "screenshot": self.shot("flood-neighbour"),
        }

    def s_categories(self):
        self.new_category("Second")
        self.new_agent("gemini")
        self.new_agent("agy")
        time.sleep(1.0)
        shot = self.shot("second-category")
        self.keys.combo("ctrl+shift+Up", 0.6)
        back = self.shot("back-to-general")
        self.keys.combo("ctrl+shift+Down", 0.6)
        categories = self.registry().get("categories", [])
        if len(categories) < 2:
            raise Failure(f"expected two categories, registry has {len(categories)}")
        return {"categories": len(categories), "screenshots": [shot, back]}

    def s_close_running(self):
        before_agents = len(self.agents())
        before_fakes = len(self.fake_agents())
        self.close_agent()
        self.wait(lambda: len(self.agents()) == before_agents - 1, 8, "agent removed")
        self.wait(
            lambda: len(self.fake_agents()) == before_fakes - 1,
            8,
            "agent process ended",
        )
        return {"screenshot": self.shot("closed-running")}

    def s_close_hung(self):
        self.new_agent("omp")
        time.sleep(1.0)
        self.send("hang")
        time.sleep(1.0)
        before = len(self.fake_agents())
        started = time.monotonic()
        self.close_agent()
        self.wait(
            lambda: len(self.fake_agents()) == before - 1, 10, "hung agent killed"
        )
        return {
            "kill_seconds": round(time.monotonic() - started, 1),
            "screenshot": self.shot("closed-hung"),
        }

    def s_crash_then_close(self):
        self.new_agent("kimi")
        time.sleep(1.0)
        self.send("crash")
        time.sleep(2.0)
        shot = self.shot("crashed-agent")
        before = len(self.agents())
        self.keys.combo("ctrl+shift+w", 0.8)
        self.wait(
            lambda: len(self.agents()) == before - 1,
            6,
            "ended agent closes without confirmation",
        )
        return {"screenshots": [shot, self.shot("crashed-closed")]}

    def s_gui_kill_restore(self):
        agents = {agent["id"] for agent in self.agents()}
        fakes = {row["pid"] for row in self.fake_agents()}
        os.killpg(self.gui.pid, signal.SIGKILL)
        self.gui.wait()
        time.sleep(1.0)
        if {row["pid"] for row in self.fake_agents()} != fakes:
            raise Failure("killing the GUI changed the agent processes")
        self.launch()
        time.sleep(3.0)
        if {agent["id"] for agent in self.agents()} != agents:
            raise Failure("registry changed across GUI kill")
        if {row["pid"] for row in self.fake_agents()} != fakes:
            raise Failure("reopening started or lost agent processes")
        return {"agents": len(agents), "screenshot": self.shot("restored-after-kill")}

    def s_service_kill(self):
        services = self.services()
        if not services:
            raise Failure("no services to kill")
        victim = services[0]
        os.kill(victim["pid"], signal.SIGKILL)
        time.sleep(3.0)
        return {
            "killed_service": victim["pid"],
            "screenshot": self.shot("service-killed"),
            "gui_alive": self.gui_alive(),
        }

    def s_resizes(self):
        shots = []
        for width, height in (
            (640, 480),
            (700, 900),
            (980, 700),
            (1400, 960),
            (1580, 1150),
        ):
            self.keys.resize(width, height)
            shots.append(self.shot(f"size-{width}x{height}"))
        return {"screenshots": shots}

    def s_rapid_fire(self):
        import random

        rng = random.Random(7)
        actions = [
            lambda: self.next_agent(),
            lambda: self.previous_agent(),
            lambda: self.keys.combo("ctrl+shift+Down", 0.05),
            lambda: self.keys.combo("ctrl+shift+Up", 0.05),
            lambda: (
                self.keys.combo("ctrl+shift+p", 0.1),
                self.keys.combo("Escape", 0.05),
            ),
            lambda: (
                self.keys.combo("ctrl+shift+n", 0.1),
                self.keys.combo("Escape", 0.05),
                self.keys.combo("Escape", 0.05),
            ),
            lambda: self.keys.type("abc"),
            lambda: self.keys.combo("ctrl+shift+b", 0.05),
        ]
        for _ in range(200):
            rng.choice(actions)()
        time.sleep(1.5)
        self.keys.combo("Escape", 0.2)
        return {"screenshot": self.shot("after-rapid-fire")}

    def s_quit_restore(self):
        fakes = {row["pid"] for row in self.fake_agents()}
        self.keys.combo("ctrl+shift+q", 1.0)
        self.wait(lambda: not self.gui_alive(), 10, "GUI quits")
        if {row["pid"] for row in self.fake_agents()} != fakes:
            raise Failure("quitting changed agent processes")
        self.launch()
        time.sleep(3.0)
        return {"gui_may_exit": True, "screenshot": self.shot("restored-after-quit")}

    def usage(self):
        """Resident memory (KiB) and CPU percent of the GUI and all services."""
        rows = {row["pid"] for row in self.services()}
        pids = [self.gui.pid] + sorted(rows)
        result = subprocess.run(
            ["ps", "-o", "pid=,rss=,pcpu=", "-p", ",".join(map(str, pids))],
            capture_output=True,
            text=True,
            check=False,
        )
        gui = {"rss": 0, "cpu": 0.0}
        services = {"rss": 0, "cpu": 0.0, "count": 0}
        for line in result.stdout.splitlines():
            pid, rss, cpu = line.split()
            target = gui if int(pid) == self.gui.pid else services
            target["rss"] += int(rss)
            target["cpu"] += float(cpu)
            if target is services:
                services["count"] += 1
        return gui, services

    def soak(self, minutes, agents=32, categories=4):
        """Many bursting agents across categories, sampled over time."""
        names = [name for name in FAKE_NAMES]
        per_category = agents // categories
        for category in range(categories):
            if category:
                self.new_category(f"Soak {category + 1}")
            for index in range(per_category):
                self.new_agent(names[(category + index) % len(names)])
        created = time.monotonic()
        samples = []
        shots = [self.shot("soak-start")]
        deadline = created + minutes * 60
        step = 0
        while time.monotonic() < deadline:
            step += 1
            # Walk the agents and categories the way a supervisor would.
            for _ in range(3):
                self.next_agent()
            if step % 4 == 0:
                self.keys.combo("ctrl+shift+Down", 0.3)
            gui, services = self.usage()
            samples.append(
                {
                    "seconds": round(time.monotonic() - created),
                    "gui_rss_kib": gui["rss"],
                    "gui_cpu": gui["cpu"],
                    "services": services["count"],
                    "services_rss_kib": services["rss"],
                    "services_cpu": services["cpu"],
                    "agents": len(self.fake_agents()),
                }
            )
            if step % 20 == 0:
                shots.append(self.shot(f"soak-{step}"))
            if not self.gui_alive():
                raise Failure("GUI exited during soak")
            time.sleep(15)
        first, last = samples[0], samples[-1]
        return {
            "agents": agents,
            "minutes": minutes,
            "samples": samples,
            "gui_rss_growth_kib": last["gui_rss_kib"] - first["gui_rss_kib"],
            "services_rss_growth_kib": last["services_rss_kib"]
            - first["services_rss_kib"],
            "screenshots": shots,
        }

    def cleanup(self):
        if self.config is not None:
            CONFIG.write_bytes(self.config)
        if self.gui_alive():
            os.killpg(self.gui.pid, signal.SIGKILL)
        for row in self.services():
            try:
                os.kill(row["pid"], signal.SIGKILL)
            except ProcessLookupError:
                pass
        for row in self.fake_agents():
            try:
                os.kill(row["pid"], signal.SIGKILL)
            except ProcessLookupError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build/qa" / time.strftime("breakit-%Y%m%d-%H%M%S"),
    )
    parser.add_argument(
        "--keep", action="store_true", help="reuse an existing QA runtime"
    )
    parser.add_argument("--only", nargs="*", help="scenario names to run")
    parser.add_argument(
        "--soak", type=float, default=0, help="minutes of 32-agent soak"
    )
    args = parser.parse_args()
    run = Run(args.output.resolve(), args.keep)
    run.prepare()
    scenarios = [
        ("empty", run.s_empty),
        ("create", run.s_create),
        ("type_and_switch", run.s_type_and_switch),
        ("finish_pulse", run.s_finish_pulse),
        ("flood_while_typing", run.s_flood_while_typing),
        ("categories", run.s_categories),
        ("close_running", run.s_close_running),
        ("close_hung", run.s_close_hung),
        ("crash_then_close", run.s_crash_then_close),
        ("gui_kill_restore", run.s_gui_kill_restore),
        ("rapid_fire", run.s_rapid_fire),
        ("resizes", run.s_resizes),
        ("service_kill", run.s_service_kill),
        ("quit_restore", run.s_quit_restore),
    ]
    if args.soak:
        os.environ["LAPIS_FAKE_BURST"] = "20"
        scenarios = [("soak", lambda: run.soak(args.soak))]
    try:
        run.launch()
        for name, function in scenarios:
            if args.only and name not in args.only:
                continue
            run.scenario(name, function)
    finally:
        run.cleanup()
        passed = sum(1 for result in run.results if result["passed"])
        receipt = {
            "passed": passed,
            "total": len(run.results),
            "results": run.results,
            "gui_warnings": run.warnings()[:200],
        }
        (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2))
        print(
            f"{passed}/{len(run.results)} scenarios passed; {args.output}/receipt.json"
        )
    return 0 if run.results and passed == len(run.results) else 1


if __name__ == "__main__":
    sys.exit(main())
