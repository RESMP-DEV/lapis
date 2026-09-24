"""Run the lapis iPhone app's UI tests in the iOS Simulator against real services.

Starts, all disposable and on this Mac:
- a session service running tools/qa/fake_agent.py ("echo agent"),
- optionally real Codex (managed mode) and Claude Code agents against
  scripts/fake_models.py, so no model usage is spent ("codex fake",
  "claude fake"),
- a registry entry whose service is not running ("parked"),
- the gateway (apps/remote/lapis_remote.py) on 127.0.0.1 with --allow-local.

Then it runs the LapisUITests on a headless simulator (no Simulator window)
and exports the screenshots to build/ios/screens/.

The app and its UI tests are compiled directly (swiftc, actool, codesign) and
run with `xcodebuild test-without-building`, not through Xcode's build
service, which is faster and does not depend on the generated project.

    uv run --no-project python scripts/check_ios_remote.py [--codex] [--claude]
"""

import argparse
import json
import plistlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / "build" / "desktop" / "services" / "session" / "lapis_session_service"
APP_SOURCES = ROOT / "apps" / "ios" / "Lapis"
TEST_SOURCES = ROOT / "apps" / "ios" / "LapisUITests"
BUILD = ROOT / "build" / "ios"
PRODUCTS = BUILD / "sim"
DEVICE = "iPhone 17 Pro"
PORT = 7350
TARGET = "arm64-apple-ios17.0-simulator"
APP_ID = "dev.lapis.remote"
RUNNER_ID = "dev.lapis.remote.uitests.xctrunner"

sys.path.insert(0, str(ROOT / "apps" / "remote"))
import lapis_remote  # noqa: E402

CODEX_CONFIG = """model = "lapis-fake"
model_provider = "lapis_fake"
approval_policy = "on-request"

[model_providers.lapis_fake]
name = "lapis fake"
base_url = "http://127.0.0.1:43110/v1"
wire_api = "responses"
requires_openai_auth = false
supports_websockets = false
request_max_retries = 0
stream_max_retries = 0

[features]
apps = false
plugins = false
multi_agent = false

[analytics]
enabled = false

[projects.{directory}]
trust_level = "trusted"
"""


class Run:
    def __init__(self, runtime):
        self.runtime = runtime
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

    def service(self, identifier, program, arguments, directory, env=None, codex=False):
        endpoint = self.runtime / (identifier + ".sock")
        self.start(
            "service-" + identifier[:8],
            [str(SERVICE)]
            + (["--codex"] if codex else [])
            + [str(endpoint), str(directory), program, *arguments],
            {"LAPIS_HISTORY_ROOT": str(self.runtime / "history"), **(env or {})},
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
                os.killpg(process.pid, signal.SIGKILL)


class MacClient(threading.Thread):
    """Stays attached to an agent the way the desktop does, and answers the phone.

    When the phone's "ping from phone" reaches this screen, it types "pong from
    mac"; the phone's UI test waits for that. It records whether it was ever
    closed, which joining must never cause.
    """

    def __init__(self, agent):
        super().__init__(daemon=True)
        self.session = lapis_remote.WireSession(
            agent, 100, 30, mode=lapis_remote.DISCOVER
        )
        self.stopping = threading.Event()
        self.saw_phone = False
        self.closed = None

    def run(self):
        while not self.stopping.is_set():
            try:
                received = self.session.receive(0.5)
            except (OSError, EOFError, lapis_remote.GatewayError) as error:
                if not self.stopping.is_set():
                    self.closed = str(error)
                return
            if received is None:
                continue
            kind, data = received
            if kind == lapis_remote.STATUS:
                self.closed = lapis_remote.status_message(data)
                return
            snapshot = self.session.accept_snapshot(kind, data)
            if snapshot is None or self.saw_phone:
                continue
            lines = lapis_remote.render_snapshot(snapshot)["lines"]
            screen = "\n".join("".join(run[0] for run in line) for line in lines)
            if "echo: ping from phone" in screen:
                self.saw_phone = True
                self.session.text(b"pong from mac\r")

    def stop(self):
        self.stopping.set()
        self.join(5)
        self.session.close()


def agent(
    identifier, title, category, harness, endpoint, program, arguments, directory
):
    return {
        "id": identifier,
        "title": title,
        "category": category,
        "harness": harness,
        "endpoint": endpoint,
        "program": program,
        "arguments": arguments,
        "directory": str(directory),
    }


def tool(*command):
    result = subprocess.run(
        command, stdin=subprocess.DEVNULL, capture_output=True, text=True
    )
    if result.returncode != 0:
        raise SystemExit(
            f"{command[0]} {command[1]} failed:\n{result.stdout}{result.stderr}"
        )
    return result.stdout.strip()


def sign(path):
    tool("codesign", "--force", "--sign", "-", "--timestamp=none", str(path))


def build_app(sdk):
    """Lapis.app for the simulator, from the same sources and Info.plist."""
    app = PRODUCTS / "Lapis.app"
    shutil.rmtree(app, ignore_errors=True)
    app.mkdir(parents=True)
    sources = sorted(str(path) for path in APP_SOURCES.glob("*.swift"))
    tool(
        "xcrun",
        "swiftc",
        "-target",
        TARGET,
        "-sdk",
        sdk,
        "-parse-as-library",
        "-Onone",
        "-g",
        "-module-name",
        "Lapis",
        *sources,
        "-o",
        str(app / "Lapis"),
    )
    partial = PRODUCTS / "assets.plist"
    tool(
        "xcrun",
        "actool",
        str(APP_SOURCES / "Assets.xcassets"),
        "--compile",
        str(app),
        "--platform",
        "iphonesimulator",
        "--minimum-deployment-target",
        "17.0",
        "--app-icon",
        "AppIcon",
        "--target-device",
        "iphone",
        "--output-partial-info-plist",
        str(partial),
    )
    values = {
        "DEVELOPMENT_LANGUAGE": "en",
        "EXECUTABLE_NAME": "Lapis",
        "PRODUCT_BUNDLE_IDENTIFIER": APP_ID,
        "PRODUCT_NAME": "Lapis",
        "LAPIS_DEFAULT_HOST": "",
    }
    info = plistlib.loads((APP_SOURCES / "Info.plist").read_bytes())
    for key, value in info.items():
        if isinstance(value, str) and value.startswith("$(") and value.endswith(")"):
            info[key] = values[value[2:-1]]
    info.update(plistlib.loads(partial.read_bytes()))
    info.update(
        {
            "CFBundleSupportedPlatforms": ["iPhoneSimulator"],
            "MinimumOSVersion": "17.0",
            "UIDeviceFamily": [1],
            "LSRequiresIPhoneOS": True,
            "DTPlatformName": "iphonesimulator",
        }
    )
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    sign(app)
    return app


def build_ui_tests(sdk, platform):
    """XCTRunner hosting LapisUITests.xctest, as Xcode assembles it."""
    developer = Path(platform) / "Developer"
    runner = PRODUCTS / "LapisUITests-Runner.app"
    shutil.rmtree(runner, ignore_errors=True)
    shutil.copytree(
        developer / "Library/Xcode/Agents/XCTRunner.app", runner, symlinks=True
    )
    frameworks = runner / "Frameworks"
    frameworks.mkdir()
    for framework in (developer / "Library/Frameworks").glob("*.framework"):
        shutil.copytree(framework, frameworks / framework.name, symlinks=True)
    for name in ("XCTestCore", "XCTAutomationSupport", "XCUnit", "XCTestSupport"):
        shutil.copytree(
            developer / f"Library/PrivateFrameworks/{name}.framework",
            frameworks / f"{name}.framework",
            symlinks=True,
        )
    for name in ("libXCTestSwiftSupport.dylib", "lib_TestingInterop.dylib"):
        shutil.copy2(developer / "usr/lib" / name, frameworks / name)
    # The template's executable and identity are placeholders Xcode fills in.
    (runner / "XCTRunner").rename(runner / "LapisUITests-Runner")
    info = plistlib.loads((runner / "Info.plist").read_bytes())
    info.update(
        {
            "CFBundleExecutable": "LapisUITests-Runner",
            "CFBundleIdentifier": RUNNER_ID,
            "CFBundleName": "LapisUITests-Runner",
        }
    )
    (runner / "Info.plist").write_bytes(plistlib.dumps(info))

    bundle = runner / "PlugIns" / "LapisUITests.xctest"
    bundle.mkdir(parents=True)
    tool(
        "xcrun",
        "swiftc",
        "-target",
        TARGET,
        "-sdk",
        sdk,
        "-emit-library",
        "-Xlinker",
        "-bundle",
        "-module-name",
        "LapisUITests",
        "-Onone",
        "-g",
        "-F",
        str(developer / "Library/Frameworks"),
        "-I",
        str(developer / "usr/lib"),
        "-L",
        str(developer / "usr/lib"),
        "-framework",
        "XCTest",
        "-Xlinker",
        "-rpath",
        "-Xlinker",
        "@executable_path/Frameworks",
        "-Xlinker",
        "-rpath",
        "-Xlinker",
        "@loader_path/../../Frameworks",
        *sorted(str(path) for path in TEST_SOURCES.glob("*.swift")),
        "-o",
        str(bundle / "LapisUITests"),
    )
    (bundle / "Info.plist").write_bytes(
        plistlib.dumps(
            {
                "CFBundleExecutable": "LapisUITests",
                "CFBundleIdentifier": "dev.lapis.remote.uitests",
                "CFBundleName": "LapisUITests",
                "CFBundlePackageType": "BNDL",
                "CFBundleShortVersionString": "1.0",
                "CFBundleVersion": "1",
                "CFBundleSupportedPlatforms": ["iPhoneSimulator"],
                "MinimumOSVersion": "17.0",
            }
        )
    )
    for item in sorted(frameworks.iterdir()):
        sign(item)
    sign(bundle)
    sign(runner)
    return runner


def write_xctestrun(environment):
    path = PRODUCTS / "Lapis.xctestrun"
    path.write_bytes(
        plistlib.dumps(
            {
                "LapisUITests": {
                    "TestBundlePath": "__TESTHOST__/PlugIns/LapisUITests.xctest",
                    "TestHostPath": "__TESTROOT__/LapisUITests-Runner.app",
                    "TestHostBundleIdentifier": RUNNER_ID,
                    "UITargetAppPath": "__TESTROOT__/Lapis.app",
                    "UITargetAppBundleIdentifier": APP_ID,
                    "IsUITestBundle": True,
                    "IsXCTRunnerHostedTestBundle": True,
                    "DependentProductPaths": [
                        "__TESTROOT__/Lapis.app",
                        "__TESTROOT__/LapisUITests-Runner.app",
                    ],
                    "EnvironmentVariables": environment,
                    "TestingEnvironmentVariables": environment,
                    "SystemAttachmentLifetime": "deleteOnSuccess",
                    "UserAttachmentLifetime": "keepAlways",
                },
                "__xctestrun_metadata__": {"FormatVersion": 1},
            }
        )
    )
    return path


def simulator(command, check=True):
    return subprocess.run(
        ["xcrun", "simctl", *command], capture_output=True, text=True, check=check
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--codex", action="store_true", help="also run a real Codex agent"
    )
    parser.add_argument(
        "--claude", action="store_true", help="also run a real Claude Code agent"
    )
    parser.add_argument("--only", help="one UI test method, e.g. testCodexAgent")
    args = parser.parse_args()
    if not SERVICE.exists():
        raise SystemExit(f"missing {SERVICE}; build the desktop first")
    sdk = tool("xcrun", "--sdk", "iphonesimulator", "--show-sdk-path")
    platform = tool("xcrun", "--sdk", "iphonesimulator", "--show-sdk-platform-path")
    build_app(sdk)
    build_ui_tests(sdk, platform)

    runtime = Path(tempfile.mkdtemp(prefix="lapis-ios-", dir="/tmp"))
    run = Run(runtime)
    booted_here = False
    try:
        python = sys.executable
        fake = str(ROOT / "tools" / "qa" / "fake_agent.py")
        echo_id = str(uuid.uuid4())
        agents = [
            agent(
                echo_id,
                "echo agent",
                "build",
                "grok",
                run.service(echo_id, python, [fake], ROOT),
                python,
                [fake],
                ROOT,
            )
        ]
        parked = str(uuid.uuid4())
        agents.append(
            agent(
                parked,
                "parked",
                "later",
                "claude",
                str(runtime / (parked + ".sock")),
                "/bin/sh",
                [],
                "/",
            )
        )
        if args.codex or args.claude:
            run.start(
                "fake-models",
                [
                    python,
                    str(ROOT / "scripts" / "fake_models.py"),
                    "--log",
                    str(run.logs / "fake-models.jsonl"),
                ],
            )
        if args.claude:
            claude = shutil.which("claude")
            if claude is None:
                raise SystemExit("claude is not installed")
            config = runtime / "claude-config"
            work = runtime / "claude-work"
            config.mkdir(mode=0o700)
            work.mkdir(mode=0o700)
            version = tool(claude, "--version").split()[0]
            (config / ".claude.json").write_text(
                json.dumps(
                    {
                        "hasCompletedOnboarding": True,
                        "theme": "dark",
                        "lastOnboardingVersion": version,
                        "projects": {
                            str(work.resolve()): {"hasTrustDialogAccepted": True}
                        },
                    }
                )
            )
            claude_id = str(uuid.uuid4())
            agents.append(
                agent(
                    claude_id,
                    "claude fake",
                    "build",
                    "claude",
                    run.service(
                        claude_id,
                        claude,
                        [],
                        work,
                        {
                            "CLAUDE_CONFIG_DIR": str(config),
                            "ANTHROPIC_BASE_URL": "http://127.0.0.1:43110",
                            "ANTHROPIC_AUTH_TOKEN": "lapis-fake",
                            "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
                        },
                    ),
                    claude,
                    [],
                    work,
                )
            )
        if args.codex:
            codex = shutil.which("codex")
            if codex is None:
                raise SystemExit("codex is not installed")
            home = runtime / "codex-home"
            work = runtime / "codex-work"
            home.mkdir(mode=0o700)
            work.mkdir(mode=0o700)
            (home / "config.toml").write_text(
                CODEX_CONFIG.format(directory=json.dumps(str(work.resolve())))
            )
            codex_id = str(uuid.uuid4())
            agents.append(
                agent(
                    codex_id,
                    "codex fake",
                    "build",
                    "codex",
                    run.service(
                        codex_id, codex, [], work, {"CODEX_HOME": str(home)}, codex=True
                    ),
                    codex,
                    [],
                    work,
                )
            )
        registry = runtime / "workspace.json"
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
        run.start(
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
            ],
        )
        time.sleep(1.5)
        echo = next(
            item
            for item in lapis_remote.load_workspace(registry)["agents"]
            if item["id"] == echo_id
        )
        mac = MacClient(echo)
        mac.start()

        state = simulator(["list", "devices", "available", "-j"]).stdout
        devices = [
            device
            for runtime_devices in json.loads(state)["devices"].values()
            for device in runtime_devices
            if device["name"] == DEVICE
        ]
        if not devices:
            raise SystemExit(f"no {DEVICE} simulator")
        device = devices[0]
        if device["state"] != "Booted":
            simulator(["boot", device["udid"]])
            booted_here = True
        simulator(["bootstatus", device["udid"], "-b"])
        simulator(
            [
                "status_bar",
                device["udid"],
                "override",
                "--time",
                "9:41",
                "--batteryLevel",
                "100",
            ],
            check=False,
        )

        stamp = time.strftime("%Y%m%d-%H%M%S")
        results = BUILD / "results" / (stamp + ".xcresult")
        results.parent.mkdir(parents=True, exist_ok=True)
        xctestrun = write_xctestrun(
            {
                "LAPIS_HOST": f"127.0.0.1:{PORT}",
                "LAPIS_ECHO_ID": echo_id,
                "LAPIS_MAC_CLIENT": "1",
            }
        )
        command = [
            "xcodebuild",
            "test-without-building",
            "-xctestrun",
            str(xctestrun),
            "-destination",
            f"id={device['udid']}",
            "-resultBundlePath",
            str(results),
            "-collect-test-diagnostics",
            "never",
        ]
        if args.only:
            command += ["-only-testing", f"LapisUITests/LapisUITests/{args.only}"]
        with (BUILD / "test.log").open("wb") as log:
            outcome = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=log,
            )
        screens = BUILD / "screens" / stamp
        screens.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                "xcrun",
                "xcresulttool",
                "export",
                "attachments",
                "--path",
                str(results),
                "--output-path",
                str(screens),
            ],
            capture_output=True,
        )
        summary = [
            line
            for line in (BUILD / "test.log").read_text(errors="replace").splitlines()
            if "Test Case" in line or "error:" in line or "** TEST" in line
        ]
        print("\n".join(summary[-40:]))
        mac.stop()
        synced = not args.only or args.only == "testSyncedWithTheMac"
        print(
            f"Mac client: saw the phone {mac.saw_phone}, closed {mac.closed or 'never'}"
        )
        print(f"results {results}\nscreens {screens}")
        if mac.closed or (synced and not mac.saw_phone):
            return 1
        return outcome.returncode
    finally:
        run.stop()
        if booted_here:
            simulator(["shutdown", "all"], check=False)
        shutil.rmtree(runtime, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
