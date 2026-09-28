"""Run lapis C++ checks with a real compilation database and bounded parallelism."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

if __package__:
    from .probe_terminal import run_process
else:
    from probe_terminal import run_process

ROOT = Path(__file__).resolve().parents[1]
CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".hxx", ".ipp", ".mm"}
TOOL_NAMES = (
    "clang++",
    "clang-tidy",
    "clang-format",
    "clangd",
    "cppcheck",
    "cmake",
    "ctest",
)


def toolchain(tools=None):
    """Use one LLVM installation without changing the user's shell PATH."""
    requested = TOOL_NAMES if tools is None else tuple(tools)
    if unknown := set(requested) - set(TOOL_NAMES):
        raise RuntimeError(f"Unknown verification tools: {', '.join(sorted(unknown))}")
    llvm_bin = os.environ.get("LAPIS_LLVM_BIN")
    if not llvm_bin and sys.platform == "darwin" and shutil.which("brew"):
        result = subprocess.run(
            ["brew", "--prefix", "llvm"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        if result.returncode == 0:
            llvm_bin = str(Path(result.stdout.strip()) / "bin")
    tools = {}
    for name in requested:
        candidate = (
            Path(llvm_bin) / name if llvm_bin and name.startswith("clang") else None
        )
        if candidate is not None:
            if not candidate.is_file() or not os.access(candidate, os.X_OK):
                raise RuntimeError(
                    f"Selected LLVM installation is missing executable {candidate}; "
                    "set LAPIS_LLVM_BIN to a complete LLVM bin directory"
                )
            location = str(candidate)
        else:
            location = shutil.which(name)
        if not location:
            raise RuntimeError(f"Missing {name}; see CONTRIBUTING.md")
        tools[name] = location
    return tools


def configure_command(tools, mode):
    """Build the shared CMake configure command for a desktop-capable preset."""
    command = [
        tools["cmake"],
        "--preset",
        mode,
        f"-DCMAKE_CXX_COMPILER={tools['clang++']}",
    ]
    if prefix := os.environ.get("LAPIS_GHOSTTY_PREFIX"):
        command.append(f"-DLAPIS_GHOSTTY_PREFIX={prefix}")
    if ccache := shutil.which("ccache"):
        command.append(f"-DCMAKE_CXX_COMPILER_LAUNCHER={ccache}")
    if sys.platform == "darwin":
        sdk = subprocess.run(
            ["xcrun", "--show-sdk-path"],
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.strip()
        command.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
    return command


def run(label, command, log_dir, *, expect_failure=None, cwd=ROOT, timeout=300):
    """Save diagnostics; expected-failure probes require a specific diagnostic."""
    start = time.monotonic()
    timed_out = False
    try:
        result = run_process(
            [str(arg) for arg in command],
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        output = result.stdout
        passed = result.returncode == 0
        if expect_failure:
            passed = result.returncode != 0 and expect_failure in output
        return_code = result.returncode
    except subprocess.TimeoutExpired as error:
        output = error.output or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        output += f"\nCommand exceeded the {timeout}-second verification timeout.\n"
        if getattr(error, "cleanup_failure", None):
            output += error.cleanup_failure + "\n"
        timed_out = True
        passed = False
        return_code = None
    except OSError as error:
        output = f"Could not run {command[0]}: {error}\n"
        passed = False
        return_code = None
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{label}.log"
    log_path.write_text(output)
    print(
        f"{'PASS' if passed else 'FAIL'} {label}: {log_path.relative_to(ROOT)}",
        flush=True,
    )
    if not passed:
        print(output[-6000:], flush=True)
    return {
        "check": label,
        "command": [str(arg) for arg in command],
        "cwd": str(cwd),
        "passed": passed,
        "timed_out": timed_out,
        "exit_code": return_code,
        "expected_diagnostic": expect_failure,
        "elapsed_seconds": round(time.monotonic() - start, 3),
        "log": str(log_path.relative_to(ROOT)),
    }


def write_receipt(path, tools, results, scope):
    version_results = []
    versions = {}
    for name, executable in tools.items():
        version, error = _probe_version(executable)
        if error is None:
            versions[name] = {"path": executable, "version": version}
        else:
            versions[name] = {"path": executable, "error": error}
            version_results.append(
                {
                    "check": f"version-probe-{name}",
                    "kind": "version-probe",
                    "passed": False,
                    "diagnostic": error,
                }
            )
    checks = [*results, *version_results]
    receipt = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform,
        "scope": scope,
        "tools": versions,
        "passed": bool(checks) and all(result["passed"] for result in checks),
        "checks": checks,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt["passed"]


def _probe_version(executable):
    """Return the first version line or a bounded, actionable probe failure."""
    try:
        result = subprocess.run(
            [executable, "--version"],
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, f"Unable to probe {executable}: {error}"
    if result.returncode != 0:
        output = (result.stderr or result.stdout).strip()
        diagnostic = f"Version probe exited with code {result.returncode}"
        if output:
            diagnostic = f"{diagnostic}: {output}"
        return None, diagnostic
    output = result.stdout.splitlines()
    if not output:
        return None, "Version probe produced no output"
    return output[0], None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "mode",
        choices=("dev", "asan", "tsan", "profile", "desktop", "format"),
        default="dev",
        nargs="?",
    )
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()
    log_dir = ROOT / "build" / "reports" / args.mode
    receipt_path = log_dir / "receipt.json"
    # Every parsed invocation owns receipt freshness, including startup failures.
    receipt_path.unlink(missing_ok=True)
    if args.jobs < 1:
        result = _startup_failure("arguments", "--jobs must be positive", log_dir)
        _write_failure_receipt(
            receipt_path,
            {},
            [result],
            f"{args.mode}: invalid arguments; C++ checks did not run.",
        )
        return 2
    try:
        tools = toolchain()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        receipt_result = _startup_failure("toolchain", error, log_dir)
        return _write_failure_receipt(
            receipt_path,
            {},
            [receipt_result],
            f"{args.mode}: startup failed before C++ checks could run.",
        )
    results = []
    if args.mode == "format":
        try:
            names = subprocess.run(
                ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            ).stdout.split("\0")
            sources = sorted(
                {name for name in names if Path(name).suffix in CPP_SUFFIXES}
            )
            if not sources:
                raise RuntimeError("No C++ sources to format")
            results.append(
                run("format", [tools["clang-format"], "-i", *sources], log_dir)
            )
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            results.append(_startup_failure("format-setup", error, log_dir))
        return _finish(receipt_path, tools, results, args.mode, "format")

    try:
        results.extend(_run_gate(tools, args, log_dir))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        results.append(_startup_failure("check-setup", error, log_dir))
    return _finish(receipt_path, tools, results, args.mode, "gate")


def _run_gate(tools, arguments, log_dir):
    configure = configure_command(tools, arguments.mode)
    results = [run("configure", configure, log_dir)]
    if results[-1]["passed"]:
        results.append(
            run(
                "build",
                [
                    tools["cmake"],
                    "--build",
                    "--preset",
                    arguments.mode,
                    "--parallel",
                    arguments.jobs,
                ],
                log_dir,
            )
        )
    if not all(result["passed"] for result in results):
        return results

    tasks = [
        (
            "ctest",
            [tools["ctest"], "--preset", arguments.mode, "--parallel", arguments.jobs],
        )
    ]
    if arguments.mode in ("dev", "desktop"):
        analysis_failed = False
        try:
            tasks.extend(_analysis_tasks(tools, arguments, log_dir))
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            results.append(_startup_failure("analysis-setup", error, log_dir))
            analysis_failed = True
        if analysis_failed:
            return results
    with ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        futures = [
            pool.submit(run, label, command, log_dir) for label, command in tasks
        ]
        for future in futures:
            results.append(future.result())
    return results


def _analysis_tasks(tools, arguments, log_dir):
    database = ROOT / "build" / arguments.mode / "compile_commands.json"
    entries = json.loads(database.read_text())
    if not isinstance(entries, list):
        raise ValueError(
            "Compilation database top level must be a JSON list; "
            f"got {type(entries).__name__}"
        )
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError(
                f"Compilation database entry {index} must be an object; "
                f"got {type(entry).__name__}"
            )
        source = entry.get("file")
        if not isinstance(source, str) or not source.strip():
            raise ValueError(
                f"Compilation database entry {index} must have a nonempty "
                f'string "file"; got {source!r}'
            )
    # Qt-generated MOC/RCC files are compiler-checked, not hand-maintained
    # source. Analyze first-party translation units with their real flags.
    entries = [
        entry
        for entry in entries
        if not Path(entry["file"]).is_relative_to(ROOT / "build")
    ]
    sources = sorted({entry["file"] for entry in entries})
    analysis_database = log_dir / "compile_commands.json"
    analysis_database.write_text(json.dumps(entries, indent=2) + "\n")
    if not sources:
        raise RuntimeError(
            "Compilation database has no C++ sources; refusing an empty check"
        )
    names = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
        timeout=30,
    ).stdout.split("\0")
    format_sources = sorted(
        {name for name in names if Path(name).suffix in CPP_SUFFIXES}
    )
    return [
        (
            "clang-format",
            [tools["clang-format"], "--dry-run", "--Werror", *format_sources],
        ),
        (
            "cppcheck",
            [
                tools["cppcheck"],
                f"--project={analysis_database}",
                *(["--library=qt"] if arguments.mode == "desktop" else []),
                "--enable=warning,performance,portability",
                "--error-exitcode=1",
                "--inline-suppr",
                "--template=gcc",
            ],
        ),
        *(
            (
                f"clang-tidy-{index}",
                [tools["clang-tidy"], "-p", database.parent, source],
            )
            for index, source in enumerate(sources)
        ),
    ]


def _finish(receipt_path, tools, results, mode, suffix):
    if suffix == "format":
        scope = "format: tracked C++ sources only; not compilation or test coverage."
    else:
        scope = (
            f"{mode}: compiled targets and registered CTest cases only; "
            "not GUI or performance acceptance."
        )
    return 0 if write_receipt(receipt_path, tools, results, scope) else 1


def _write_failure_receipt(path, tools, results, scope):
    return 0 if write_receipt(path, tools, results, scope) else 1


def _startup_failure(label, error, log_dir):
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{label}.log"
    log_path.write_text(f"{error}\n")
    return {
        "check": label,
        "kind": "startup",
        "passed": False,
        "diagnostic": str(error),
        "log": str(log_path.relative_to(ROOT)),
    }


if __name__ == "__main__":
    raise SystemExit(main())
