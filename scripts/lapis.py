"""One-command entry points for lapis; resolves the build environment itself.

Every `just` recipe and documented command goes through here so contributors do
not export LAPIS_GHOSTTY_PREFIX, locate Qt, or remember desktop binary paths by
hand. Configuration stays in one place:

    python3 scripts/lapis.py <command> [arguments]

Run `python3 scripts/lapis.py` with no arguments to list the commands.
"""

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESKTOP_BUILD = ROOT / "build" / "desktop"
DESKTOP_BINARY = (
    DESKTOP_BUILD
    / "apps"
    / "desktop"
    / "lapis_desktop.app"
    / "Contents"
    / "MacOS"
    / "lapis_desktop"
)
GHOSTTY_RUNS = ROOT / "build" / "terminal-probe" / "reproduce" / "ghostty" / "runs"
RUNTIME_DIR = ROOT / "runtime"
DEFAULT_SOCKET = RUNTIME_DIR / "desktop-v6.sock"
# Window tests belong on the laptop panel, not a large external display. Override
# with LAPIS_SCREEN, or pass --screen to the app directly.
DEFAULT_SCREEN = "built-in"
VALUE_OPTIONS = frozenset(
    {
        "--capture",
        "--capture-delay",
        "--cwd",
        "--qml",
        "--scenario",
        "--screen",
        "--socket",
        "--trace",
        "--workspace",
    }
)


class SetupError(RuntimeError):
    """A missing dependency that the contributor must install or bootstrap."""


def _run(command, *, check=True, **kwargs):
    """Run a command from the repository root, echoing it for copy/paste."""
    printable = " ".join(str(part) for part in command)
    print(f"$ {printable}", flush=True)
    return subprocess.run(
        [str(part) for part in command], cwd=ROOT, check=check, **kwargs
    )


def valid_ghostty_prefix(candidate):
    """Match CMake's installed artifacts and successful pinned-source receipt."""
    try:
        receipt = json.loads((candidate.parent / "reports/receipt.json").read_text())
        sources = json.loads(
            (ROOT / "tools/terminal_probe/ghostty/sources.json").read_text()
        )
        return (
            (candidate / "include/ghostty/vt.h").is_file()
            and (candidate / "lib/libghostty-vt.a").is_file()
            and receipt.get("engine") == "ghostty"
            and receipt.get("passed") is True
            and receipt.get("sources") == sources
        )
    except (OSError, ValueError, AttributeError):
        return False


def ghostty_prefix():
    """Return the newest successful pinned build, or explain how to create one."""
    configured = os.environ.get("LAPIS_GHOSTTY_PREFIX")
    if configured:
        candidate = Path(configured).resolve()
        if not valid_ghostty_prefix(candidate):
            raise SetupError(
                f"LAPIS_GHOSTTY_PREFIX has missing artifacts or an invalid receipt: {candidate}\n"
                "Rebuild it with: python3 scripts/lapis.py bootstrap"
            )
        return candidate
    if GHOSTTY_RUNS.is_dir():
        prefixes = [
            run / "prefix"
            for run in GHOSTTY_RUNS.iterdir()
            if valid_ghostty_prefix(run / "prefix")
        ]
        if prefixes:
            return max(
                prefixes,
                key=lambda prefix: (
                    (prefix.parent / "reports/receipt.json").stat().st_mtime_ns,
                    str(prefix),
                ),
            )
    raise SetupError(
        "Ghostty VT is not bootstrapped.\n"
        "Build the pinned dependency once with: python3 scripts/lapis.py bootstrap"
    )


def environment():
    """Environment overrides required by CMake and the desktop app."""
    screen = os.environ.get("LAPIS_SCREEN", DEFAULT_SCREEN)
    return {
        "LAPIS_GHOSTTY_PREFIX": str(ghostty_prefix()),
        "LAPIS_SCREEN": screen,
    }


def run_python_script(name, arguments, *, needs_ghostty=True):
    """Run a sibling repository script with the resolved environment."""
    script = Path(__file__).resolve().parent / name
    if not script.is_file():
        raise SetupError(f"Missing script: {script}")
    result = subprocess.run(
        [sys.executable, str(script), *arguments],
        cwd=ROOT,
        env={**os.environ, **(environment() if needs_ghostty else {})},
        check=False,
    )
    return result.returncode


def desktop_binary():
    if not DESKTOP_BINARY.is_file():
        raise SetupError(
            f"The desktop app is not built: {DESKTOP_BINARY}\n"
            "Build it with: python3 scripts/lapis.py build"
        )
    return DESKTOP_BINARY


def private_runtime_dir():
    """The service requires a socket parent owned by the user with mode 0700."""
    try:
        try:
            RUNTIME_DIR.mkdir(mode=0o700, parents=True)
        except FileExistsError:
            pass
        runtime_stat = os.lstat(RUNTIME_DIR)
    except OSError as error:
        raise SetupError(
            f"Cannot create or inspect private runtime directory {RUNTIME_DIR}: {error}"
        ) from error
    validate_runtime_directory(runtime_stat)
    return RUNTIME_DIR


def validate_runtime_directory(runtime_stat):
    """Apply the same read-only runtime contract to launch and doctor."""
    if not stat.S_ISDIR(runtime_stat.st_mode):
        raise SetupError(f"Runtime path is not a directory: {RUNTIME_DIR}")
    if runtime_stat.st_uid != os.geteuid():
        raise SetupError(
            f"Runtime directory is owned by UID {runtime_stat.st_uid}, "
            f"not current UID {os.geteuid()}: {RUNTIME_DIR}"
        )
    if stat.S_IMODE(runtime_stat.st_mode) != 0o700:
        raise SetupError(f"Runtime directory must have mode 0700: {RUNTIME_DIR}")


def has_positional_program(arguments):
    """Mirror the desktop parser's treatment of bare program arguments."""
    expecting_value = False
    for argument in arguments:
        if expecting_value:
            expecting_value = False
            continue
        if argument == "--":
            return False
        if argument.startswith("--"):
            name, separator, _ = argument.partition("=")
            expecting_value = not separator and name in VALUE_OPTIONS
            continue
        if argument.startswith("-") and argument != "-":
            continue
        return True
    return False


def launch(arguments):
    """Exec the desktop app, passing through any extra arguments."""
    arguments = list(arguments)
    separator = arguments.index("--") if "--" in arguments else len(arguments)
    app_arguments = arguments[:separator]
    separator_program = separator < len(arguments) - 1
    if not any(a == "--screen" or a.startswith("--screen=") for a in app_arguments):
        screen = os.environ.get("LAPIS_SCREEN", DEFAULT_SCREEN)
        if screen:
            arguments[:0] = ["--screen", screen]
    command = [desktop_binary(), *arguments]
    if (
        not separator_program
        and "--ui-preview" not in app_arguments
        and not any(
            a in ("--socket", "--workspace")
            or a.startswith(("--socket=", "--workspace="))
            for a in app_arguments
        )
    ):
        private_runtime_dir()
        explicit_session = any(
            a
            in (
                "--new-session",
                "--discover",
                "--cwd",
                "--codex",
                "--claude",
                "--smoke-input",
            )
            or a.startswith("--cwd=")
            for a in app_arguments
        ) or has_positional_program(app_arguments)
        command[1:1] = (
            ["--socket", str(DEFAULT_SOCKET)]
            if explicit_session
            else ["--workspace", str(RUNTIME_DIR / "workspace-v1.json")]
        )
    os.environ.update(environment())
    return _run(command, check=False).returncode


def command_doctor():
    """Report which pieces of the local environment are ready."""
    rows = []
    prefix = None
    try:
        prefix = ghostty_prefix()
        rows.append(("Ghostty VT", "ok", str(prefix)))
    except SetupError as error:
        lines = str(error).splitlines()
        detail = lines[1] if len(lines) > 1 else str(error)
        rows.append(("Ghostty VT", "missing", detail))
    rows.append(
        (
            "Desktop app",
            "ok" if DESKTOP_BINARY.is_file() else "missing",
            "built"
            if DESKTOP_BINARY.is_file()
            else "run: python3 scripts/lapis.py build",
        )
    )
    qt = (
        subprocess.run(
            ["qmake", "-query", "QT_VERSION"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()
        if shutil.which("qmake")
        else ""
    )
    rows.append(
        (
            "Qt",
            "ok" if qt == "6.11.2" else "check",
            qt or "not found; brew bundle --file Brewfile",
        )
    )
    try:
        runtime_stat = os.lstat(RUNTIME_DIR)
        validate_runtime_directory(runtime_stat)
        rows.append(("runtime/", "ok", "owned directory, mode 0700"))
    except FileNotFoundError:
        rows.append(("runtime/", "ok", "created privately on first launch"))
    except (OSError, SetupError) as error:
        rows.append(("runtime/", "fix", str(error)))
    width = max(len(name) for name, _, _ in rows)
    for name, state, detail in rows:
        print(f"{name.ljust(width)}  {state:8}  {detail}")
    return 0 if all(state == "ok" for _, state, _ in rows) else 1


def command_bootstrap():
    """Build the pinned Ghostty VT dependency, then report readiness."""
    code = run_python_script(
        "probe_terminal.py", ["--engine", "ghostty"], needs_ghostty=False
    )
    if code != 0:
        return code
    try:
        print(f"\nGhostty prefix: {ghostty_prefix()}")
    except SetupError as error:
        print(f"\n{error}", file=sys.stderr)
        return 1
    return command_doctor()


def command_run(arguments):
    return launch(arguments)


def command_gui(arguments):
    """Live shell window with no extra flags for the common case."""
    return launch(arguments)


def command_ui(arguments):
    """Isolated QML fixture for visual iteration."""
    return launch(
        [
            "--ui-preview",
            "--qml",
            str(ROOT / "apps" / "desktop" / "qml" / "Main.qml"),
            *arguments,
        ]
    )


def command_ui_debug(arguments):
    """Isolated fixture under the macOS native debugger."""
    if sys.platform != "darwin":
        raise SetupError("ui-debug requires macOS and xcrun lldb")
    return _run(
        [
            "xcrun",
            "lldb",
            "--",
            desktop_binary(),
            "--ui-preview",
            "--qml",
            ROOT / "apps" / "desktop" / "qml" / "Main.qml",
            *arguments,
        ],
        check=False,
    ).returncode


def command_smoke(arguments):
    """Drive Qt key input through the PTY and capture the window."""
    capture = ROOT / "build" / "window.png"
    return launch(["--smoke-input", "--capture", str(capture), *arguments])


COMMANDS = {
    "quality": lambda a: run_python_script("check_quality.py", a, needs_ghostty=False),
    "check": lambda a: run_python_script("check_cpp.py", ["dev", *a]),
    "asan": lambda a: run_python_script("check_cpp.py", ["asan", *a]),
    "tsan": lambda a: run_python_script("check_cpp.py", ["tsan", *a]),
    "profile": lambda a: run_python_script("check_cpp.py", ["profile", *a]),
    "format": lambda a: run_python_script("check_cpp.py", ["format", *a]),
    "verify-tools": lambda a: run_python_script("verify_cpp_tools.py", a),
    "build": lambda a: run_python_script("check_cpp.py", ["desktop", *a]),
    "cli-check": lambda a: run_python_script("check_cli_launch.py", ["--desktop", *a]),
    "ui-check": lambda a: run_python_script("check_ui_preview.py", a),
    "codex-probe": lambda a: run_python_script("probe_codex.py", a),
    "run": command_run,
    "gui": command_gui,
    "ui": command_ui,
    "ui-debug": command_ui_debug,
    "smoke": command_smoke,
    "doctor": lambda _: command_doctor(),
    "bootstrap": lambda _: command_bootstrap(),
}

DESCRIPTIONS = {
    "quality": "Repository contracts, Python lint/format and unit tests (no GUI)",
    "check": "Compile, lint, format-check and run CTest",
    "asan": "CTest with AddressSanitizer and UndefinedBehaviorSanitizer",
    "tsan": "CTest with ThreadSanitizer",
    "profile": "Optimized build with symbols for profiling",
    "format": "Apply repository C++ formatting",
    "verify-tools": "Prove the tools detect known-bad fixtures",
    "build": "Build the desktop app and run its checks",
    "cli-check": "Dedicated CLI/service/GUI acceptance fixtures",
    "ui-check": "Bounded preview captures and failure cases",
    "codex-probe": "Record content-free evidence from the installed Codex",
    "run": "Open the live shell window (pass extra app flags through)",
    "gui": "Alias for run",
    "ui": "Open the isolated QML fixture for visual iteration",
    "ui-debug": "Open the isolated fixture under lldb",
    "smoke": "Drive Qt input through the PTY and capture the window",
    "doctor": "Report which local dependencies are ready",
    "bootstrap": "Build the pinned Ghostty VT dependency",
}


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("command", nargs="?", help="command to run; see the list below")
    parser.add_argument(
        "arguments", nargs=argparse.REMAINDER, help="passed to the command"
    )
    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        print("\nCommands:")
        width = max(len(name) for name in COMMANDS)
        for name in sorted(COMMANDS):
            print(f"  {name.ljust(width)}  {DESCRIPTIONS[name]}")
        return 0
    handler = COMMANDS.get(args.command)
    if handler is None:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        print(f"Available: {', '.join(sorted(COMMANDS))}", file=sys.stderr)
        return 2
    try:
        return handler(args.arguments)
    except SetupError as error:
        print(f"\n{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
