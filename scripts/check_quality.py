"""Run non-GUI repository quality checks and write an aggregate receipt."""

import argparse
import json
import os
import re
import shlex
import shutil
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

if __package__:
    from . import check_cpp
else:
    import check_cpp

ROOT = Path(__file__).resolve().parents[1]
RECEIPT_SCHEMA_VERSION = 1
MAX_WORKERS = 4
REVISION_PATTERN = re.compile(r"[0-9a-f]{40}")
PYTHON_TEST_PROGRAM = """import sys, unittest
suite = unittest.defaultTestLoader.discover('scripts/tests')
if not suite.countTestCases():
    sys.exit('No Python tests discovered')
sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
"""


def check_instruction_link(root):
    path = root / "CLAUDE.md"
    target = root / "AGENTS.md"
    if not path.is_symlink():
        return _contract_result(
            "claude-instructions-symlink",
            False,
            "CLAUDE.md must be a symbolic link to AGENTS.md",
        )
    try:
        link_target = os.readlink(path)
    except OSError as error:
        return _contract_result(
            "claude-instructions-symlink",
            False,
            f"Unable to inspect CLAUDE.md symlink: {error}",
        )
    if link_target != "AGENTS.md":
        return _contract_result(
            "claude-instructions-symlink",
            False,
            f"CLAUDE.md must have literal relative target AGENTS.md, got {link_target!r}",
        )
    if not target.is_file():
        return _contract_result(
            "claude-instructions-symlink",
            False,
            "CLAUDE.md target AGENTS.md is missing",
        )
    return _contract_result("claude-instructions-symlink", True, None)


def check_sindexer_ignore(root):
    path = root / ".gitignore"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        return _contract_result(
            "sindexer-ignore",
            False,
            f"Unable to read root .gitignore: {error}",
        )
    if ".sindexer/" not in lines:
        return _contract_result(
            "sindexer-ignore",
            False,
            "Root .gitignore must contain the exact line .sindexer/",
        )
    return _contract_result("sindexer-ignore", True, None)


def run_quality_checks(
    root, report_dir, tools, jobs=MAX_WORKERS, python_executable=sys.executable
):
    tasks = [
        lambda: check_instruction_link(root),
        lambda: check_sindexer_ignore(root),
        lambda: _run_guarded(
            "git-diff-check",
            [tools.get("git"), "diff", "--check", "HEAD"],
            report_dir,
            root,
        ),
        lambda: _run_guarded(
            "ruff-check",
            [
                tools.get("ruff"),
                "check",
                "--config",
                root / "ruff.toml",
                "scripts",
            ],
            report_dir,
            root,
        ),
        lambda: _run_guarded(
            "ruff-format-check",
            [
                tools.get("ruff"),
                "format",
                "--check",
                "--config",
                root / "ruff.toml",
                "scripts",
            ],
            report_dir,
            root,
        ),
        lambda: _run_guarded(
            "python-unittests",
            [
                python_executable,
                "-c",
                PYTHON_TEST_PROGRAM,
            ],
            report_dir,
            root,
        ),
    ]
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        return list(executor.map(lambda task: task(), tasks))


def find_source_revision(root, report_dir, git_executable):
    if not git_executable:
        result = _command_error(
            "source-revision",
            ["git", "rev-parse", "HEAD"],
            report_dir,
            "git is not available on PATH",
            root,
        )
        return None, result
    result = _run_bounded(
        "source-revision",
        [git_executable, "rev-parse", "HEAD"],
        report_dir,
        root,
    )
    if not result["passed"]:
        return None, result
    try:
        output = (
            (report_dir / "source-revision.log").read_text(encoding="utf-8").strip()
        )
    except (OSError, UnicodeError) as error:
        result["passed"] = False
        result["diagnostic"] = f"Unable to read source revision output: {error}"
        return None, result
    revision = output.splitlines()[0] if output else ""
    if not REVISION_PATTERN.fullmatch(revision):
        result["passed"] = False
        result["diagnostic"] = (
            "git rev-parse HEAD did not return a 40-character hexadecimal revision"
        )
        return None, result
    return revision, result


def write_receipt(path, source_revision, results, scope, *, source_dirty=None):
    receipt = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform,
        "source_revision": source_revision,
        "source_dirty": source_dirty,
        "scope": scope,
        "passed": bool(results) and all(result["passed"] for result in results),
        "checks": results,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(MAX_WORKERS, os.cpu_count() or 1),
        help="worker count for independent checks (maximum 4)",
    )
    arguments = parser.parse_args()
    if arguments.jobs < 1 or arguments.jobs > MAX_WORKERS:
        parser.error(f"--jobs must be between 1 and {MAX_WORKERS}")

    report_dir = ROOT / "build" / "reports" / "quality"
    try:
        # Never leave a prior PASS receipt after an interrupted or failed startup.
        (report_dir / "receipt.json").unlink(missing_ok=True)
        tools = {"git": shutil.which("git"), "ruff": shutil.which("ruff")}
        source_revision, revision_result = find_source_revision(
            ROOT, report_dir, tools["git"]
        )
        source_status = _run_guarded(
            "source-status", [tools["git"], "status", "--porcelain"], report_dir, ROOT
        )
        source_dirty = (
            bool((report_dir / "source-status.log").read_text().strip())
            if source_status["passed"]
            else None
        )
        results = [
            revision_result,
            source_status,
            *run_quality_checks(ROOT, report_dir, tools, arguments.jobs),
        ]
        receipt = write_receipt(
            report_dir / "receipt.json",
            source_revision,
            results,
            "Non-GUI quality gate: repository contracts, diff hygiene, Python lint and format, and script unit tests.",
            source_dirty=source_dirty,
        )
        print(
            f"{'PASS' if receipt['passed'] else 'FAIL'} quality: "
            f"{(report_dir / 'receipt.json').relative_to(ROOT)}",
            flush=True,
        )
        return 0 if receipt["passed"] else 1
    except (OSError, RuntimeError) as error:
        print(f"FAIL quality: {error}", file=sys.stderr, flush=True)
        return 1


def _contract_result(name, passed, diagnostic):
    return {
        "check": name,
        "kind": "contract",
        "passed": passed,
        "diagnostic": diagnostic,
    }


def _run_bounded(name, command, report_dir, root):
    executable = command[0]
    if not executable:
        return _command_error(
            name,
            command,
            report_dir,
            f"Missing required executable for {name}",
            root,
        )
    rendered_command = [str(part) for part in command]
    try:
        result = check_cpp.run(name, rendered_command, report_dir, cwd=root)
    except (OSError, RuntimeError) as error:
        return _command_error(
            name,
            rendered_command,
            report_dir,
            f"Unable to execute {name}: {error}",
            root,
        )
    result["command"] = rendered_command
    if not result["passed"]:
        result["diagnostic"] = f"{name} failed with exit code {result['exit_code']}"
    else:
        result["diagnostic"] = None
    return result


def _run_guarded(name, command, report_dir, root):
    try:
        return _run_bounded(name, command, report_dir, root)
    except (OSError, RuntimeError) as error:
        return _command_error(
            name,
            command,
            report_dir,
            f"Unable to execute {name}: {error}",
            root,
        )


def _command_error(name, command, report_dir, diagnostic, root):
    report_dir.mkdir(parents=True, exist_ok=True)
    log_path = report_dir / f"{name}.log"
    rendered_command = [str(part) for part in command]
    message = f"{diagnostic}\nCommand: {shlex.join(rendered_command)}\n"
    log_path.write_text(message, encoding="utf-8")
    return {
        "check": name,
        "kind": "command",
        "passed": False,
        "exit_code": None,
        "command": rendered_command,
        "diagnostic": diagnostic,
        "log": str(log_path.relative_to(root)),
    }


if __name__ == "__main__":
    raise SystemExit(main())
