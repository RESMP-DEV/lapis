"""Build the lapis Android app and run its JVM unit tests with Gradle.

Locates the Android SDK (ANDROID_HOME/ANDROID_SDK_ROOT, ~/Library/Android/sdk,
then apps/android/local.properties) and the Gradle wrapper in apps/android,
then runs, in order:

    ./gradlew :app:assembleDebug
    ./gradlew :app:testDebugUnitTest

Both stream to the console; logs and JUnit XML land under build/android/.
When the SDK or the Gradle toolchain is missing the script skips with an
explicit message and exit code 3 instead of failing.

    uv run --no-project python scripts/check_android.py [--device]

Exit codes: 0 green; 1 a check failed or a --device check failed; 3 skipped
(the Android SDK or Gradle is not installed here). With --device, the debug
APK is installed and launched on one attached adb device for live checks.
"""

import argparse
import os
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "apps" / "android"
BUILD = ROOT / "build" / "android"
APK = PROJECT / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
TEST_RESULTS = PROJECT / "app" / "build" / "test-results" / "testDebugUnitTest"

SKIP_EXIT = 3


def tool(name: str, *arguments: str, cwd: Path | None = None, env: dict | None = None):
    """Runs a tool, streaming its output; returns the completed process."""
    print(f"$ {name} {' '.join(arguments)}", flush=True)
    return subprocess.run(
        [name, *arguments],
        cwd=cwd,
        env=env,
        check=False,
    )


def find_sdk() -> Path | None:
    """The Android SDK, from the environment, the default location, or
    local.properties."""
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(name)
        if value and (Path(value) / "platform-tools").is_dir():
            return Path(value)
    default = Path.home() / "Library" / "Android" / "sdk"
    if (default / "platform-tools").is_dir():
        return default
    local = PROJECT / "local.properties"
    if local.is_file():
        for line in local.read_text().splitlines():
            if line.startswith("sdk.dir="):
                candidate = Path(line.removeprefix("sdk.dir=").strip())
                if (candidate / "platform-tools").is_dir():
                    return candidate
    return None


def find_gradle() -> list[str] | None:
    """The wrapper when present, else a system gradle; None when neither."""
    wrapper = PROJECT / "gradlew"
    if wrapper.is_file():
        return [str(wrapper)]
    if (
        subprocess.run(["which", "gradle"], capture_output=True, check=False).returncode
        == 0
    ):
        return ["gradle"]
    return None


def java_environment() -> dict | None:
    """Environment for Gradle. Honors JAVA_HOME; on macOS falls back to a
    JDK >= 17 through java_home when the default java is older or missing
    (AGP requires 17+). The user's default JVM is never changed."""
    env = {**os.environ}
    java_home = env.get("JAVA_HOME")
    if java_home and _java_major(Path(java_home)) >= 17:
        return env
    if _java_major(None) >= 17:
        env.pop("JAVA_HOME", None)
        return env
    if sys.platform == "darwin":
        home = subprocess.run(
            ["/usr/libexec/java_home", "-v", "21"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()
        if home and _java_major(Path(home)) >= 17:
            print(f"using JAVA_HOME={home} for this run only")
            env["JAVA_HOME"] = home
            return env
    print(
        "no JDK 17+ found (AGP 9 requires it); set JAVA_HOME to a JDK 17+",
        file=sys.stderr,
    )
    return None


def _java_major(java_home: Path | None) -> int:
    if java_home:
        binary = java_home / "bin" / "java"
    else:
        found = subprocess.run(
            ["which", "java"], capture_output=True, text=True, check=False
        ).stdout.strip()
        if not found:
            return 0
        binary = Path(found)
    try:
        output = subprocess.run(
            [str(binary), "-version"], capture_output=True, text=True, check=False
        ).stderr
    except OSError:
        return 0
    for token in output.split():
        # `java -version` prints the version quoted, e.g. "25.0.1".
        version = token.strip('"')
        if version[:1].isdigit():
            try:
                return int(version.split(".")[0])
            except ValueError:
                return 0
    return 0


def gradle(gradle_command: list[str], env: dict, *tasks: str) -> int:
    """One Gradle invocation, streamed; returns its exit code."""
    started = time.monotonic()
    process = tool(
        gradle_command[0],
        *gradle_command[1:],
        *tasks,
        "--console=plain",
        cwd=PROJECT,
        env=env,
    )
    seconds = time.monotonic() - started
    print(f"gradle {' '.join(tasks)}: exit {process.returncode} in {seconds:.1f}s")
    return process.returncode


def summarize_tests() -> str | None:
    """One line from the JUnit XML: how many ran, failed, skipped."""
    if not TEST_RESULTS.is_dir():
        return None
    total = failures = errors = skipped = 0
    for result in sorted(TEST_RESULTS.glob("*.xml")):
        try:
            suite = ElementTree.parse(result).getroot()
        except ElementTree.ParseError:
            continue
        total += int(suite.get("tests", 0))
        failures += int(suite.get("failures", 0))
        errors += int(suite.get("errors", 0))
        skipped += int(suite.get("skipped", 0))
    return (
        f"tests completed: {total}, failures: {failures}, errors: {errors}, "
        f"skipped: {skipped}"
    )


def device_checks(sdk: Path) -> int:
    """Installs and launches the fresh APK on one attached device."""
    adb = sdk / "platform-tools" / "adb"
    devices = subprocess.run(
        [str(adb), "devices"], capture_output=True, text=True, check=False
    ).stdout.splitlines()
    attached = [
        line.split()[0]
        for line in devices[1:]
        if line.strip() and line.split()[1] == "device"
    ]
    if not attached:
        print(
            "no adb device attached; start one (or an emulator) and rerun "
            "with --device",
            file=sys.stderr,
        )
        return 1
    serial = attached[0]
    if tool(str(adb), "-s", serial, "install", "-r", str(APK)).returncode != 0:
        return 1
    if (
        tool(
            str(adb),
            "-s",
            serial,
            "shell",
            "am",
            "start",
            "-n",
            "dev.lapis.remote/dev.lapis.remote.ui.MainActivity",
        ).returncode
        != 0
    ):
        return 1
    print(f"installed and launched on {serial}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--device",
        action="store_true",
        help="after a green build, install and launch on one attached adb device",
    )
    arguments = parser.parse_args()

    sdk = find_sdk()
    if sdk is None:
        print(
            "Android SDK not found; skipping Android checks "
            f"(exit {SKIP_EXIT}). Install the SDK or set ANDROID_HOME.",
        )
        return SKIP_EXIT
    gradle_command = find_gradle()
    if gradle_command is None:
        print(
            "no gradlew in apps/android and no gradle on PATH; skipping "
            f"Android checks (exit {SKIP_EXIT}).",
        )
        return SKIP_EXIT
    env = java_environment()
    if env is None:
        return SKIP_EXIT if not os.environ.get("JAVA_HOME") else 1

    BUILD.mkdir(parents=True, exist_ok=True)
    print(f"Android SDK: {sdk}")

    if gradle(gradle_command, env, ":app:assembleDebug") != 0:
        print("assembleDebug failed", file=sys.stderr)
        return 1
    if gradle(gradle_command, env, ":app:testDebugUnitTest") != 0:
        print("testDebugUnitTest failed", file=sys.stderr)
        return 1
    summary = summarize_tests()
    if summary:
        print(summary)
    print(f"APK: {APK}")

    if arguments.device:
        return device_checks(sdk)
    return 0


if __name__ == "__main__":
    sys.exit(main())
