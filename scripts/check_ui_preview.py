"""Exercise the desktop binary's isolated UI preview mode."""

import argparse
import json
import os
import subprocess
import sys
import time
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / (
    "build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop"
)
TIMEOUT_SECONDS = 20


class CheckError(RuntimeError):
    pass


class CheckResult:
    def __init__(
        self,
        name: str,
        passed: bool,
        exit_code: int | None,
        elapsed_seconds: float,
        output: str,
    ) -> None:
        self.name = name
        self.passed = passed
        self.exit_code = exit_code
        self.elapsed_seconds = elapsed_seconds
        self.output = output


def save_log(directory: Path, name: str, output: str) -> Path:
    path = directory / f"{name}.log"
    path.write_text(output, encoding="utf-8")
    return path


def run(
    name: str,
    binary: Path,
    arguments: Sequence[str],
    artifacts: Path,
    *,
    expect_success: bool = True,
) -> CheckResult:
    command = [str(binary), *arguments]
    started = time.monotonic()
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    timed_out = False
    try:
        output, _ = process.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        timed_out = True
        stop_process_group(process)
        output, _ = process.communicate(timeout=5)
        output = f"Timed out after {TIMEOUT_SECONDS} seconds\n{output}"
    except BaseException:
        stop_process_group(process)
        raise
    elapsed = time.monotonic() - started
    exit_code = None if timed_out else process.returncode
    passed = not timed_out and (
        process.returncode == 0 if expect_success else process.returncode != 0
    )
    log_path = save_log(artifacts, name, output)
    expected = "success" if expect_success else "failure"
    print(
        f"{'PASS' if passed else 'FAIL'} {name} ({elapsed:.2f}s, "
        f"expected {expected}): {log_path}",
        flush=True,
    )
    return CheckResult(name, passed, exit_code, elapsed, output)


def stop_process_group(process: subprocess.Popen[str]) -> None:
    try:
        os.killpg(process.pid, 15)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, 9)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired as cleanup_error:
            raise CheckError(
                f"Could not reap timed-out process {process.pid}"
            ) from cleanup_error


def capture_ok(image: Path, trace: Path, scenario: str, reduced: bool) -> bool:
    if not image.is_file() or image.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    data = json.loads(trace.read_text())
    before, after = data["before"], data["after"]
    if not data["preview"] or not data["frame_times_ms"]:
        return False
    if reduced and not data["reduced_motion"]:
        return False
    for key in (
        "terminal_owns_focus",
        "pane_height",
        "pane_width",
        "terminal_columns",
        "terminal_rows",
    ):
        if before[key] != after[key]:
            return False
    if not after["terminal_owns_focus"]:
        return False
    first = {card["id"]: card for card in before["cards"]}
    for card in after["cards"]:
        if card["id"] in ("shell", "renderer", "agent") and "width" not in card:
            return False
        expected = card["id"] == "agent" and scenario != "none"
        expected = expected or (card["id"] == "renderer" and scenario == "two")
        if card["pending"] != expected or card.get("cue_running", False):
            return False
        for field in ("x", "y", "width", "height"):
            if (
                field in first[card["id"]]
                and card.get(field) != first[card["id"]][field]
            ):
                return False
    return True


def execute_checks(binary: Path, artifacts: Path) -> list[dict]:
    results = []
    for name, scenario, extra in [
        ("default", "none", []),
        ("compact", "none", ["--compact"]),
        ("arrival", "arrival", []),
        ("two", "two", []),
        ("reduced", "two", ["--reduced-motion"]),
    ]:
        image, trace = artifacts / f"{name}.png", artifacts / f"{name}.json"
        image.unlink(missing_ok=True)
        trace.unlink(missing_ok=True)
        result = run(
            name,
            binary,
            [
                "--ui-preview",
                "--scenario",
                scenario,
                *extra,
                "--capture",
                str(image),
                "--trace",
                str(trace),
                "--capture-delay",
                "2000",
            ],
            artifacts,
        )
        if result.passed:
            result.passed = capture_ok(
                image, trace, scenario, "--reduced-motion" in extra
            )
            if not result.passed:
                print(f"FAIL {name}: capture/focus/geometry/attention assertions")
        results.append(
            {
                "name": name,
                "passed": result.passed,
                "exit_code": result.exit_code,
                "elapsed_seconds": round(result.elapsed_seconds, 3),
            }
        )
    invalid = artifacts / "invalid.qml"
    invalid.write_text("import QtQuick\nWindow { broken syntax ! }\n")
    cases = [
        ("reject-shell-input", ["--smoke-input"], "cannot be combined"),
        ("reject-qml", ["--qml", str(invalid)], "QQmlApplicationEngine failed"),
        (
            "reject-destination",
            [
                "--capture",
                str(artifacts / "missing" / "image.png"),
                "--capture-delay",
                "0",
            ],
            "Window capture failed",
        ),
    ]
    for name, arguments, diagnostic in cases:
        result = run(
            name, binary, ["--ui-preview", *arguments], artifacts, expect_success=False
        )
        result.passed = (
            result.passed and result.exit_code in (1, 2) and diagnostic in result.output
        )
        results.append(
            {
                "name": name,
                "passed": result.passed,
                "exit_code": result.exit_code,
                "elapsed_seconds": round(result.elapsed_seconds, 3),
            }
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument(
        "--artifacts", type=Path, default=ROOT / "build/ui-preview-check"
    )
    args = parser.parse_args()
    try:
        binary, artifacts = args.binary.resolve(), args.artifacts.resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise CheckError(f"Desktop binary is missing or not executable: {binary}")
        artifacts.mkdir(parents=True, exist_ok=True)
        results = execute_checks(binary, artifacts)
        passed = all(item["passed"] for item in results)
        (artifacts / "receipt.json").write_text(
            json.dumps({"passed": passed, "checks": results}, indent=2) + "\n"
        )
        return 0 if passed else 1
    except (OSError, ValueError, KeyError, CheckError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
