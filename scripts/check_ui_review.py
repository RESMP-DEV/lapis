"""Run the routine background UI review fixtures without opening native windows.

This configures the existing ``desktop`` preset, builds only the UI-preview and
terminal-input test targets, and then runs their explicit offscreen/software
modes serially.  It deliberately does not run native GPU, AppKit input, IME or
clipboard acceptance.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

if __package__:
    from .check_cpp import configure_command, toolchain
    from .probe_terminal import run_process
else:
    from check_cpp import configure_command, toolchain
    from probe_terminal import run_process

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build" / "desktop"
REPORTS = ROOT / "build" / "reports" / "ui-review"
PRESET = "desktop"
UI_TARGET = "lapis_ui_preview_tests"
TERMINAL_TARGET = "lapis_terminal_input_tests"
BUILD_TARGETS = (UI_TARGET, TERMINAL_TARGET)
CONFIGURE_TIMEOUT = 600
BUILD_TIMEOUT = 1800
TEST_TIMEOUT = 45
RECEIPT_SCHEMA = "lapis.ui-review/1"
BACKGROUND_SCOPE = {
    "mode": "background",
    "qt_platform": "offscreen",
    "scene_graph": "software",
    "native_gpu_exercised": False,
    "native_os_input_exercised": False,
    "excludes": (
        "native GPU presentation",
        "macOS AppKit keyboard/input routing",
        "system IME",
        "system pasteboard",
    ),
}
UI_BACKGROUND_MARKERS = (
    "ui_preview_test: PASS",
    "offscreen/software; not native input or GPU acceptance",
)
TERMINAL_BACKGROUND_MARKERS = (
    "Background Qt/software mode; native macOS input and GPU not exercised",
    "Qt IME commit/cancel",
)
CAPTURE_VARIABLES = (
    "LAPIS_WORKSPACE_CAPTURE_PREFIX",
    "LAPIS_ATTENTION_CAPTURE",
    "LAPIS_SETTINGS_CAPTURE",
)


def progress(message):
    """Keep progress off stdout so ``--json`` output stays machine-readable."""
    print(f"ui-review: {message}", file=sys.stderr, flush=True)


def source_metadata():
    """Identify the tested worktree without recording user content."""
    git = shutil.which("git")
    if not git:
        raise RuntimeError("git is required to identify the UI-review receipt")
    revision = subprocess.run(
        [git, "rev-parse", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    if revision.returncode != 0:
        raise RuntimeError(
            "Could not identify HEAD: " + (revision.stderr or revision.stdout).strip()
        )
    status = subprocess.run(
        [git, "status", "--porcelain"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    if status.returncode != 0:
        raise RuntimeError(
            "Could not inspect worktree state: "
            + (status.stderr or status.stdout).strip()
        )
    return revision.stdout.strip(), bool(status.stdout.strip())


def build_commands(tools, jobs):
    """Build the two fixture targets and their dependencies only."""
    configure = configure_command(tools, PRESET)
    build = [
        tools["cmake"],
        "--build",
        "--preset",
        PRESET,
        "--parallel",
        str(jobs),
        "--target",
        *BUILD_TARGETS,
    ]
    return configure, build


def fixture_cases(build_dir):
    """Return the fixed serial inventory with mandatory background mode."""
    cases = (
        {
            "name": "ui-full-background",
            "command": [build_dir / "apps" / "desktop" / UI_TARGET, "--background"],
            "markers": UI_BACKGROUND_MARKERS,
            "captures": True,
        },
        {
            "name": "ui-shortcuts-background",
            "command": [
                build_dir / "apps" / "desktop" / UI_TARGET,
                "--background",
                "--shortcuts-only",
            ],
            "markers": UI_BACKGROUND_MARKERS,
            "captures": False,
        },
        {
            "name": "terminal-input-background",
            "command": [
                build_dir / "apps" / "desktop" / TERMINAL_TARGET,
                "--background",
            ],
            "markers": TERMINAL_BACKGROUND_MARKERS,
            "captures": False,
        },
    )
    return cases


def prepare_captures(name, report_dir):
    """Give capture hooks a fresh private directory owned by this case."""
    directory = report_dir / "captures" / name
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    environment = dict(os.environ)
    environment.update(
        {
            "LAPIS_WORKSPACE_CAPTURE_PREFIX": str(directory / "workspace-"),
            "LAPIS_ATTENTION_CAPTURE": str(directory / "attention.png"),
            "LAPIS_SETTINGS_CAPTURE": str(directory / "settings.png"),
        }
    )
    return environment, directory


def without_captures():
    """Prevent inherited hook paths from escaping the per-case capture contract."""
    environment = dict(os.environ)
    environment.update(dict.fromkeys(CAPTURE_VARIABLES, ""))
    return environment


def png_artifacts(directory):
    """List generated captures with repository-relative receipt paths."""
    return [
        str(path.relative_to(ROOT))
        for path in sorted(directory.rglob("*.png"))
        if path.is_file()
    ]


def _text(value):
    if value is None:
        return ""
    return (
        value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value
    )


def run_step(
    name,
    command,
    report_dir,
    *,
    timeout,
    runner=run_process,
    markers=None,
    environment=None,
    captures_dir=None,
):
    """Run one bounded subprocess group, capture its log and retain its result."""
    command = [str(part) for part in command]
    log_path = report_dir / f"{name}.log"
    started = time.monotonic()
    progress(f"{name}: started")
    timed_out = False
    cleanup_failure = None
    try:
        result = runner(
            command,
            cwd=ROOT,
            env=environment or dict(os.environ),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        output = result.stdout or ""
        exit_code = result.returncode
    except subprocess.TimeoutExpired as error:
        output = _text(getattr(error, "output", ""))
        timed_out = True
        exit_code = None
        cleanup_failure = getattr(error, "cleanup_failure", None)
        output += f"\nCommand exceeded the {timeout}-second UI-review timeout.\n"
    except OSError as error:
        output = f"Could not run {command[0]}: {error}\n"
        exit_code = None

    log_path.write_text(output, encoding="utf-8")
    passed = exit_code == 0
    diagnostic = None
    captures = png_artifacts(captures_dir) if captures_dir is not None else []
    if passed and markers:
        if not all(marker in output for marker in markers):
            passed = False
            diagnostic = (
                "fixture exited successfully without its background PASS markers"
            )
    if passed and captures_dir is not None and not captures:
        passed = False
        diagnostic = "capture hooks produced no PNG artifacts"
    if timed_out:
        diagnostic = "command timed out"
    elif exit_code is None and diagnostic is None:
        diagnostic = f"command could not be executed: {output.strip()}"
    elif exit_code not in (0, None) and diagnostic is None:
        diagnostic = f"exit code {exit_code}"

    result = {
        "name": name,
        "command": command,
        "cwd": str(ROOT),
        "passed": passed,
        "timed_out": timed_out,
        "exit_code": exit_code,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "log": str(log_path.relative_to(ROOT)),
        "captures": captures,
    }
    if cleanup_failure:
        result["cleanup_failure"] = cleanup_failure
    if diagnostic:
        result["diagnostic"] = diagnostic
        progress(f"FAIL {name}: {log_path.relative_to(ROOT)}")
        progress(output[-6000:].rstrip())
    else:
        progress(f"PASS {name}: {log_path.relative_to(ROOT)}")
    return result


def run_fixtures(build_dir, report_dir, *, runner=run_process):
    """Run all fixture cases serially, even when an earlier case fails."""
    results = []
    for case in fixture_cases(build_dir):
        if "--background" not in case["command"][1:]:
            raise RuntimeError(
                f"{case['name']} did not select explicit background mode"
            )
        captures_dir = None
        environment = None
        if case["captures"]:
            environment, captures_dir = prepare_captures(case["name"], report_dir)
        else:
            environment = without_captures()
        results.append(
            run_step(
                case["name"],
                case["command"],
                report_dir,
                timeout=TEST_TIMEOUT,
                runner=runner,
                markers=case["markers"],
                environment=environment,
                captures_dir=captures_dir,
            )
        )
    return results


def prepare_report_directory():
    """Create one ignored, private report directory per invocation."""
    REPORTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return Path(tempfile.mkdtemp(prefix=f"{stamp}-", dir=REPORTS))


def review(*, jobs=None, json_output=False, runner=run_process):
    """Configure, build and review; always retain a receipt on bounded failure."""
    arguments_jobs = min(os.cpu_count() or 1, 8) if jobs is None else jobs
    report_dir = prepare_report_directory()
    receipt_path = report_dir / "receipt.json"
    receipt = {
        "schema": RECEIPT_SCHEMA,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform,
        "preset": PRESET,
        "build_dir": str(BUILD_DIR.resolve()),
        "report_directory": str(report_dir.relative_to(ROOT)),
        "receipt": str(receipt_path.relative_to(ROOT)),
        "source_revision": None,
        "source_dirty": None,
        "scope": BACKGROUND_SCOPE,
        "passed": False,
        "configure": None,
        "build": None,
        "test_cases": [],
    }
    try:
        receipt["source_revision"], receipt["source_dirty"] = source_metadata()
        inventory = fixture_cases(BUILD_DIR)
        if not inventory:
            raise RuntimeError("Refusing an empty UI-review fixture inventory")
        tools = toolchain(("clang++", "cmake"))
        if arguments_jobs < 1:
            raise RuntimeError("--jobs must be positive")

        configure_args, build_command = build_commands(tools, arguments_jobs)
        configure = run_step(
            "configure",
            configure_args,
            report_dir,
            timeout=CONFIGURE_TIMEOUT,
            runner=runner,
        )
        receipt["configure"] = configure
        if configure["passed"]:
            receipt["build"] = run_step(
                "build",
                build_command,
                report_dir,
                timeout=BUILD_TIMEOUT,
                runner=runner,
            )
        if receipt["build"] is not None and receipt["build"]["passed"]:
            receipt["test_cases"] = run_fixtures(BUILD_DIR, report_dir, runner=runner)

        expected_names = [case["name"] for case in inventory]
        observed_names = [case["name"] for case in receipt["test_cases"]]
        receipt["passed"] = (
            configure["passed"]
            and receipt["build"] is not None
            and receipt["build"]["passed"]
            and bool(expected_names)
            and len(receipt["test_cases"]) == len(expected_names)
            and observed_names == expected_names
            and all(case["passed"] for case in receipt["test_cases"])
        )
    except Exception as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
        progress(f"error: {error}")
    finally:
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")

    if json_output:
        print(json.dumps(receipt, indent=2), flush=True)
    else:
        state = "PASS" if receipt["passed"] else "FAIL"
        print(f"{state} ui-review: {receipt_path.relative_to(ROOT)}", flush=True)
    return 0 if receipt["passed"] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="write only JSON to stdout")
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(os.cpu_count() or 1, 8),
        help="parallel build jobs (default: at most 8)",
    )
    arguments = parser.parse_args(argv)
    return review(jobs=arguments.jobs, json_output=arguments.json)


if __name__ == "__main__":
    raise SystemExit(main())
