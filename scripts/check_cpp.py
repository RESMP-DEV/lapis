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

ROOT = Path(__file__).resolve().parents[1]
CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".hxx", ".ipp", ".mm"}


def toolchain():
    """Use one LLVM installation without changing the user's shell PATH."""
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
    for name in (
        "clang++",
        "clang-tidy",
        "clang-format",
        "clangd",
        "cppcheck",
        "cmake",
        "ctest",
    ):
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


def run(label, command, log_dir, *, expect_failure=None, cwd=ROOT):
    """Save diagnostics; expected-failure probes require a specific diagnostic."""
    start = time.monotonic()
    try:
        result = subprocess.run(
            [str(arg) for arg in command],
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=300,
            check=False,
        )
        output = result.stdout
        passed = result.returncode == 0
        if expect_failure:
            passed = result.returncode != 0 and expect_failure in output
        return_code = result.returncode
    except subprocess.TimeoutExpired:
        output = "Command exceeded the 300-second verification timeout.\n"
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
        "passed": passed,
        "exit_code": return_code,
        "expected_diagnostic": expect_failure,
        "elapsed_seconds": round(time.monotonic() - start, 3),
        "log": str(log_path.relative_to(ROOT)),
    }


def write_receipt(path, tools, results, scope):
    versions = {}
    for name, executable in tools.items():
        version = subprocess.run(
            [executable, "--version"],
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.splitlines()[0]
        versions[name] = {"path": executable, "version": version}
    receipt = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform,
        "scope": scope,
        "tools": versions,
        "passed": bool(results) and all(result["passed"] for result in results),
        "checks": results,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt["passed"]


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
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    tools = toolchain()
    log_dir = ROOT / "build" / "reports" / args.mode
    results = []
    if args.mode == "format":
        names = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split("\0")
        sources = sorted({name for name in names if Path(name).suffix in CPP_SUFFIXES})
        if not sources:
            raise RuntimeError("No C++ sources to format")
        result = run("format", [tools["clang-format"], "-i", *sources], log_dir)
        return 0 if result["passed"] else 1

    configure = [
        tools["cmake"],
        "--preset",
        args.mode,
        f"-DCMAKE_CXX_COMPILER={tools['clang++']}",
    ]
    if shutil.which("ccache"):
        configure.append(f"-DCMAKE_CXX_COMPILER_LAUNCHER={shutil.which('ccache')}")
    if sys.platform == "darwin":
        sdk = subprocess.run(
            ["xcrun", "--show-sdk-path"],
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.strip()
        configure.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
    result = run("configure", configure, log_dir)
    results.append(result)
    if result["passed"]:
        results.append(
            run(
                "build",
                [
                    tools["cmake"],
                    "--build",
                    "--preset",
                    args.mode,
                    "--parallel",
                    args.jobs,
                ],
                log_dir,
            )
        )
    if all(result["passed"] for result in results):
        tasks = [
            ("ctest", [tools["ctest"], "--preset", args.mode, "--parallel", args.jobs])
        ]
        if args.mode in ("dev", "desktop"):
            database = ROOT / "build" / args.mode / "compile_commands.json"
            entries = json.loads(database.read_text())
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
            ).stdout.split("\0")
            format_sources = sorted(
                {name for name in names if Path(name).suffix in CPP_SUFFIXES}
            )
            tasks.append(
                (
                    "clang-format",
                    [tools["clang-format"], "--dry-run", "--Werror", *format_sources],
                )
            )
            tasks.append(
                (
                    "cppcheck",
                    [
                        tools["cppcheck"],
                        f"--project={analysis_database}",
                        *(["--library=qt"] if args.mode == "desktop" else []),
                        "--enable=warning,performance,portability",
                        "--error-exitcode=1",
                        "--inline-suppr",
                        "--template=gcc",
                    ],
                )
            )
            for index, source in enumerate(sources):
                tasks.append(
                    (
                        f"clang-tidy-{index}",
                        [tools["clang-tidy"], "-p", database.parent, source],
                    )
                )
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [
                pool.submit(run, label, command, log_dir) for label, command in tasks
            ]
            results.extend(future.result() for future in futures)
    passed = write_receipt(
        log_dir / "receipt.json",
        tools,
        results,
        f"{args.mode}: compiled targets and registered CTest cases only; not GUI or performance acceptance.",
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
