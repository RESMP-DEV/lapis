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
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESKTOP_BUILD = ROOT / "build" / "desktop"
GHOSTTY_RUNS = ROOT / "build" / "terminal-probe" / "reproduce" / "ghostty" / "runs"
RUNTIME_DIR = ROOT / "runtime"
DEFAULT_SOCKET = RUNTIME_DIR / "desktop-v6.sock"
LAVAPIPE_ICDS = (
    Path("/usr/share/vulkan/icd.d/lvp_icd.json"),
    Path("/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"),
)


def desktop_binary_path():
    """CMake emits a macOS bundle and a plain executable on Linux."""
    directory = DESKTOP_BUILD / "apps" / "desktop"
    if sys.platform == "darwin":
        directory /= "lapis_desktop.app/Contents/MacOS"
    return directory / "lapis_desktop"


def default_screen():
    """Normal launch leaves display placement to the OS and saved geometry."""
    return ""


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
    screen = os.environ.get("LAPIS_SCREEN", default_screen())
    overrides = {
        **llvm_environment(),
        "LAPIS_GHOSTTY_PREFIX": str(ghostty_prefix()),
        "LAPIS_SCREEN": screen,
    }
    prefix = linux_qt_prefix()
    if prefix is not None:
        prefixes = os.environ.get("CMAKE_PREFIX_PATH", "").split(os.pathsep)
        prefixes = [value for value in prefixes if value]
        if str(prefix) not in prefixes:
            prefixes.append(str(prefix))
        overrides["CMAKE_PREFIX_PATH"] = os.pathsep.join(prefixes)
    return overrides


def llvm_environment():
    """Use the complete Linux LLVM bundle unless the caller selected another."""
    if sys.platform != "linux" or "LAPIS_LLVM_BIN" in os.environ:
        return {}
    directory = Path("/usr/lib/llvm-22/bin")
    if all(
        (directory / name).is_file() and os.access(directory / name, os.X_OK)
        for name in ("clang++", "clangd", "clang-format", "clang-tidy")
    ):
        return {"LAPIS_LLVM_BIN": str(directory)}
    return {}


def linux_qt_prefix():
    """Find the project-local pinned Linux SDK without changing shell settings."""
    prefix = ROOT / "build/deps/qt/6.11.2/gcc_64"
    if sys.platform == "linux" and (prefix / "lib/cmake/Qt6/Qt6Config.cmake").is_file():
        return prefix
    return None


def qt_qmake():
    """Query explicit Linux SDKs first, then the local SDK and PATH tools."""
    if sys.platform == "linux":
        prefixes = [
            Path(value)
            for value in os.environ.get("CMAKE_PREFIX_PATH", "").split(os.pathsep)
            if value
        ]
        local = linux_qt_prefix()
        if local is not None:
            prefixes.append(local)
        for prefix in prefixes:
            qmake = prefix / "bin/qmake"
            if qmake.is_file() and os.access(qmake, os.X_OK):
                return str(qmake)
        return shutil.which("qmake6") or shutil.which("qmake")
    return shutil.which("qmake")


def run_python_script(name, arguments, *, needs_ghostty=True):
    """Run a sibling repository script with the resolved environment."""
    script = Path(__file__).resolve().parent / name
    if not script.is_file():
        raise SetupError(f"Missing script: {script}")
    result = subprocess.run(
        [sys.executable, str(script), *arguments],
        cwd=ROOT,
        env={**os.environ, **(environment() if needs_ghostty else llvm_environment())},
        check=False,
    )
    return result.returncode


def desktop_binary():
    binary = desktop_binary_path()
    if not binary.is_file():
        raise SetupError(
            f"The desktop app is not built: {binary}\n"
            "Build it with: python3 scripts/lapis.py build"
        )
    return binary


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


def launch(arguments):
    """Exec the desktop app, passing through any extra arguments."""
    arguments = list(arguments)
    separator = arguments.index("--") if "--" in arguments else len(arguments)
    app_arguments = arguments[:separator]
    has_program = separator < len(arguments) - 1
    if not any(a == "--screen" or a.startswith("--screen=") for a in app_arguments):
        screen = os.environ.get("LAPIS_SCREEN", default_screen())
        if screen:
            arguments[:0] = ["--screen", screen]
    command = [desktop_binary(), *arguments]
    if (
        not has_program
        and ("--development-shell" in app_arguments or "--smoke-input" in app_arguments)
        and "--ui-preview" not in app_arguments
        and not any(a == "--socket" or a.startswith("--socket=") for a in app_arguments)
    ):
        private_runtime_dir()
        command[1:1] = ["--socket", str(DEFAULT_SOCKET)]
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
            "ok" if desktop_binary_path().is_file() else "missing",
            "built"
            if desktop_binary_path().is_file()
            else "run: python3 scripts/lapis.py build",
        )
    )
    qmake = qt_qmake()
    qt = (
        subprocess.run(
            [qmake, "-query", "QT_VERSION"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()
        if qmake
        else ""
    )
    rows.append(
        (
            "Qt",
            "ok" if qt == "6.11.2" else "check",
            qt
            or (
                "not found; install Qt 6.11.2 under build/deps/qt/6.11.2/gcc_64"
                if sys.platform == "linux"
                else "not found; brew bundle --file Brewfile"
            ),
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


def command_linux_gui(arguments):
    """Run isolated software-rendered GUI checks; this is not GPU qualification."""
    if sys.platform != "linux":
        raise SetupError("linux-gui requires Linux")
    xvfb, openbox = shutil.which("xvfb-run"), shutil.which("openbox")
    xprop = shutil.which("xprop")
    if not xvfb or not openbox or not xprop:
        raise SetupError("linux-gui requires xvfb-run and openbox plus xprop")
    icd = next((path for path in LAVAPIPE_ICDS if path.is_file()), None)
    if icd is None:
        raise SetupError("linux-gui requires the Mesa lavapipe Vulkan driver")
    command = list(arguments) or [
        sys.executable,
        str(Path(__file__).resolve()),
        "build",
    ]
    # User arguments remain positional shell arguments, never shell source.
    script = """
"$1" --sm-disable &
wm=$!
xprop=$2
shift 2
cleanup() { kill "$wm" 2>/dev/null || :; wait "$wm" 2>/dev/null || :; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
attempt=0
while :; do
    if ! kill -0 "$wm" 2>/dev/null; then
        echo 'linux-gui: openbox exited before becoming ready; inspect its startup errors' >&2
        exit 1
    fi
    property=$("$xprop" -root _NET_SUPPORTING_WM_CHECK 2>/dev/null)
    case "$property" in *'window id # 0x'*) break ;; esac
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 50 ]; then
        echo 'linux-gui: openbox did not become ready within 2.5 seconds; check Xvfb/openbox setup' >&2
        exit 1
    fi
    sleep 0.05
done
"$@"
status=$?
exit "$status"
"""
    with tempfile.TemporaryDirectory(prefix="lapis-linux-gui-") as runtime:
        env = {
            **os.environ,
            **environment(),
            "XDG_RUNTIME_DIR": runtime,
            "LAPIS_SCREEN": "",
            "QT_QPA_PLATFORM": "xcb",
            "QT_IM_MODULE": "compose",
            "VK_DRIVER_FILES": str(icd),
            "VK_ICD_FILENAMES": str(icd),
            "LIBGL_ALWAYS_SOFTWARE": "1",
            "GALLIUM_DRIVER": "llvmpipe",
        }
        return _run(
            [
                xvfb,
                "--auto-servernum",
                "--server-args=-screen 0 1600x1200x24 -nolisten tcp",
                "/bin/sh",
                "-c",
                script,
                "lapis-linux-gui",
                openbox,
                xprop,
                *command,
            ],
            env=env,
            check=False,
        ).returncode


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
    "linux-gui": command_linux_gui,
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
    "linux-gui": "Run build or supplied argv in isolated Linux software graphics",
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
