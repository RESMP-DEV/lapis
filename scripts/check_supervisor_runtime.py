#!/usr/bin/env python3
"""Run the isolated R1 supervisor build and write a focused runtime receipt."""

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

if __package__:
    from .check_cpp import run
else:
    from check_cpp import run

ROOT = Path(__file__).resolve().parents[1]
TARGETS = (
    "lapis_session_service",
    "lapis_supervisor_state_tests",
    "lapis_supervisor_runtime_tests",
    "lapis_supervisor_runtime_session_tests",
)


def source_revision() -> tuple[str, bool]:
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()
    dirty = (
        subprocess.run(
            ["git", "status", "--short"],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
        != ""
    )
    return revision, dirty


def write_failure(path: Path, scope: str, label: str, error: BaseException) -> int:
    result = {
        "check": label,
        "command": [],
        "cwd": str(ROOT),
        "passed": False,
        "timed_out": False,
        "exit_code": None,
        "diagnostic": str(error),
        "elapsed_seconds": 0.0,
        "log": str(path.parent / f"{label}.log"),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    Path(result["log"]).write_text(f"{error}\n")
    receipt = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform,
        "scope": scope,
        "passed": False,
        "checks": [result],
    }
    path.write_text(json.dumps(receipt, indent=2) + "\n")
    return 1


def integration_result(
    test_output: str, test_result: dict[str, object], logs: Path
) -> dict[str, object]:
    """Describe the observed integration without inventing a successful skip."""
    integration_observed = "supervisor-session-service: ok" in test_output
    integration_skipped = "QLocalServer bind unavailable" in test_output
    diagnostic = "Observed supervisor-born service and clients."
    exit_code = 0
    if not integration_observed:
        if integration_skipped:
            diagnostic = (
                "The integration skipped because QLocalServer bind is unavailable "
                "in the current sandbox."
            )
            exit_code = 75
        else:
            diagnostic = (
                "The integration marker was absent; the test step did not pass "
                f"(exit_code={test_result['exit_code']}, "
                f"timed_out={test_result['timed_out']}). See test.log."
            )
            exit_code = test_result["exit_code"]
    return {
        "check": "real-service-integration",
        "command": [],
        "cwd": str(ROOT),
        "passed": integration_observed,
        "timed_out": test_result["timed_out"],
        "exit_code": exit_code,
        "diagnostic": diagnostic,
        "elapsed_seconds": 0.0,
        "log": str((logs / "test.log").relative_to(ROOT)),
        "integration_skipped": integration_skipped,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/reports/r1-supervisor/receipt.json"
    )
    parser.add_argument("--jobs", type=int, default=4)
    arguments = parser.parse_args()
    receipt_path = arguments.output.resolve()
    if not receipt_path.is_relative_to(ROOT):
        parser.error("--output must name a receipt inside the repository checkout")
    receipt_path.unlink(missing_ok=True)
    logs = receipt_path.parent
    scope = (
        "Focused supervisor-runtime build and tests in build/r1-runtime. It does not prove "
        "launchd ownership, GUI integration, package replacement, reboot recovery, load, "
        "latency, or a real-service integration that reports a QLocalServer bind skip."
    )
    os.environ["CCACHE_DISABLE"] = "1"
    commands = (
        ("configure", ("cmake", "--preset", "r1-runtime")),
        (
            "build",
            (
                "cmake",
                "--build",
                "--preset",
                "r1-runtime",
                "--target",
                *TARGETS,
                "--parallel",
                str(arguments.jobs),
            ),
        ),
        (
            "test",
            (
                "ctest",
                "--test-dir",
                "build/r1-runtime",
                "-R",
                "supervisor-(state|runtime|session-service)$",
                "--output-on-failure",
                "--no-tests=error",
                "-V",
            ),
        ),
    )
    try:
        revision, dirty = source_revision()
        results = []
        for label, command in commands:
            results.append(run(label, command, logs, timeout=300))
        test_output = (logs / "test.log").read_text()
        results.append(integration_result(test_output, results[-1], logs))
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        return write_failure(receipt_path, scope, "supervisor-runtime-setup", error)

    passed = all(result["passed"] for result in results)
    receipt = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform,
        "source_revision": revision,
        "source_dirty": dirty,
        "build_directory": str((ROOT / "build/r1-runtime").relative_to(ROOT)),
        "scope": scope,
        "passed": passed,
        "checks": results,
    }
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Receipt: {receipt_path.relative_to(ROOT)}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
