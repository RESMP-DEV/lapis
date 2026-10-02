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
FOCUSED_REPORT_DIRNAME = "quality-focused"
REVISION_PATTERN = re.compile(r"[0-9a-f]{40}")
PYTHON_CHECK_DIRECTORIES = ("apps", "scripts", "tools")
TEST_MODULE_PATTERN = re.compile(r"test_.*\.py")
PYTHON_TEST_PROGRAM = """import sys, unittest
suite = unittest.defaultTestLoader.discover('scripts/tests')
if not suite.countTestCases():
    sys.exit('No Python tests discovered')
sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
"""


class SelectionError(ValueError):
    """A focused test or source selector does not name a repository path."""

    def __init__(self, message, validation_class):
        super().__init__(message)
        # Receipts record this safe class, never selector text or host paths.
        self.validation_class = validation_class


def parse_test_selectors(root, selectors):
    """Resolve focused test selectors to scripts/tests module names."""
    modules = []
    for selector in selectors:
        module = _test_module_name(root, selector)
        if module not in modules:
            modules.append(module)
    return modules


def _test_module_name(root, selector):
    text = selector.strip()
    if not text:
        raise SelectionError("test selector must not be empty", "invalid-test-selector")
    looks_like_path = (
        Path(text).is_absolute() or "/" in text or "\\" in text or text.endswith(".py")
    )
    if looks_like_path:
        relative, resolved = _resolve_selector(root, text, "test")
        _require_focused_test_file(text, relative, resolved, dotted=False)
        return ".".join(relative.with_suffix("").parts)
    parts = text.split(".")
    if not all(part.isidentifier() for part in parts):
        raise SelectionError(
            f"test selector {text!r} must be a dotted module name or a .py path",
            "invalid-test-selector",
        )
    candidate = root.joinpath(*parts).with_suffix(".py")
    try:
        resolved = candidate.resolve()
        relative = resolved.relative_to(root.resolve())
    except (OSError, ValueError):
        raise SelectionError(
            f"test module {text!r} does not map to a Python file inside the repository",
            "invalid-test-selector",
        ) from None
    _require_focused_test_file(text, relative, resolved, dotted=True)
    return text


def _require_focused_test_file(selector, relative, resolved, *, dotted):
    if not resolved.is_file():
        if dotted:
            raise SelectionError(
                f"test module {selector!r} does not map to an existing Python file",
                "invalid-test-selector",
            )
        raise SelectionError(
            f"test path {selector!r} must name an existing Python module file",
            "invalid-test-selector",
        )
    if resolved.suffix != ".py":
        raise SelectionError(
            f"test path {selector!r} must name a Python module file",
            "invalid-test-selector",
        )
    parts = relative.parts
    if len(parts) < 3 or parts[:2] != ("scripts", "tests"):
        raise SelectionError(
            f"test selector {selector!r} must name a module under scripts/tests, "
            f"got {relative.as_posix()!r}",
            "invalid-test-selector",
        )
    if not TEST_MODULE_PATTERN.fullmatch(resolved.name):
        raise SelectionError(
            f"test selector {selector!r} must name a test_*.py module, "
            f"got {resolved.name!r}",
            "invalid-test-selector",
        )


def parse_source_selectors(root, selectors):
    """Resolve focused source selectors to repository paths for Ruff."""
    paths = []
    for selector in selectors:
        relative, resolved = _resolve_selector(root, selector, "path")
        if resolved.is_dir():
            rendered = relative.as_posix()
        elif resolved.is_file() and resolved.suffix == ".py":
            rendered = relative.as_posix()
        elif resolved.is_file():
            raise SelectionError(
                f"path selector {selector!r} must name a Python file or a directory",
                "invalid-path-selector",
            )
        else:
            raise SelectionError(
                f"path selector {selector!r} does not name an existing file or directory",
                "invalid-path-selector",
            )
        if rendered not in paths:
            paths.append(rendered)
    return paths


def _resolve_selector(root, selector, kind):
    text = selector.strip()
    if not text:
        raise SelectionError(
            f"{kind} selector must not be empty",
            "invalid-test-selector" if kind == "test" else "invalid-path-selector",
        )
    candidate = Path(text)
    if not candidate.is_absolute():
        candidate = root / candidate
    try:
        resolved = candidate.resolve()
        relative = resolved.relative_to(root.resolve())
    except (OSError, ValueError):
        raise SelectionError(
            f"{kind} selector {selector!r} must name a path inside the repository",
            "invalid-test-selector" if kind == "test" else "invalid-path-selector",
        ) from None
    return relative, resolved


def focused_scope(test_modules, source_paths):
    parts = ["Focused quality run; not full-suite coverage."]
    if source_paths:
        parts.append("Ruff check/format scoped to " + ", ".join(source_paths) + ".")
    else:
        parts.append("No source paths selected, so Ruff check/format did not run.")
    if test_modules:
        parts.append("Unittest modules: " + ", ".join(test_modules) + ".")
    else:
        parts.append("No unittest modules selected, so Python unit tests did not run.")
    return " ".join(parts)


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
    root,
    report_dir,
    tools,
    jobs=MAX_WORKERS,
    python_executable=sys.executable,
    *,
    focused=False,
    test_modules=None,
    source_paths=None,
):
    test_modules = list(test_modules or [])
    source_paths = list(source_paths or [])
    tasks = [
        lambda: check_instruction_link(root),
        lambda: check_sindexer_ignore(root),
        lambda: _run_guarded(
            "git-diff-check",
            [tools.get("git"), "diff", "--check", "HEAD"],
            report_dir,
            root,
        ),
    ]
    if not focused or source_paths:
        # Focused mode omits Ruff when no paths were selected instead of
        # falling back to full-directory discovery.
        targets = source_paths or list(PYTHON_CHECK_DIRECTORIES)
        tasks.extend(_ruff_tasks(tools, root, report_dir, targets))
    if not focused or test_modules:
        # Focused mode omits unittest when no modules were selected instead of
        # falling back to full-suite discovery.
        if test_modules:
            tasks.append(
                lambda: _run_guarded(
                    "python-unittests",
                    [python_executable, "-m", "unittest", "-v", *test_modules],
                    report_dir,
                    root,
                )
            )
        else:
            tasks.append(
                lambda: _run_guarded(
                    "python-unittests",
                    [python_executable, "-c", PYTHON_TEST_PROGRAM],
                    report_dir,
                    root,
                )
            )
    return _run_tasks(tasks, jobs)


def _ruff_tasks(tools, root, report_dir, targets):
    return [
        lambda: _run_guarded(
            "ruff-check",
            [tools.get("ruff"), "check", "--config", root / "ruff.toml", *targets],
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
                *targets,
            ],
            report_dir,
            root,
        ),
    ]


def _run_tasks(tasks, jobs):
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


def write_receipt(
    path, source_revision, results, scope, *, source_dirty=None, selection=None
):
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
    if selection is not None:
        receipt["selection"] = selection
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
    parser.add_argument(
        "--focused",
        action="store_true",
        help="run scoped checks instead of the full non-GUI gate",
    )
    parser.add_argument(
        "--test",
        action="append",
        default=[],
        dest="tests",
        metavar="MODULE_OR_PATH",
        help="focused only, repeatable: run this scripts/tests module by dotted name or test_*.py path",
    )
    parser.add_argument(
        "--path",
        action="append",
        default=[],
        dest="paths",
        metavar="PATH",
        help="focused only, repeatable: scope Ruff check/format to this Python file or directory",
    )
    arguments = parser.parse_args()
    if arguments.jobs < 1 or arguments.jobs > MAX_WORKERS:
        parser.error(f"--jobs must be between 1 and {MAX_WORKERS}")
    if arguments.focused:
        return _run_focused(parser, arguments)
    if arguments.tests or arguments.paths:
        parser.error("--test and --path require --focused")

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


def _run_focused(parser, arguments):
    report_dir = ROOT / "build" / "reports" / FOCUSED_REPORT_DIRNAME
    receipt_path = report_dir / "receipt.json"
    try:
        # Invalidate first, including selector validation failures: a rejected
        # run must never leave an earlier focused PASS looking current.
        receipt_path.parent.mkdir(parents=True, exist_ok=True)
        receipt_path.unlink(missing_ok=True)
        try:
            test_modules = parse_test_selectors(ROOT, arguments.tests)
            source_paths = parse_source_selectors(ROOT, arguments.paths)
        except SelectionError as error:
            try:
                _write_focused_argument_failure(receipt_path, arguments, error)
            except OSError:
                # Validation still fails, even if the failure receipt is unwritable.
                pass
            parser.error(str(error))
    except (OSError, RuntimeError) as error:
        print(f"FAIL focused quality: {error}", file=sys.stderr, flush=True)
        return 1
    if not test_modules and not source_paths:
        error = SelectionError(
            "--focused requires at least one --test or --path selector",
            "missing-selection",
        )
        _write_focused_argument_failure(receipt_path, arguments, error)
        parser.error(str(error))

    selection = {"tests": test_modules, "paths": source_paths}
    scope = focused_scope(test_modules, source_paths)
    try:
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
            *run_quality_checks(
                ROOT,
                report_dir,
                tools,
                arguments.jobs,
                focused=True,
                test_modules=test_modules,
                source_paths=source_paths,
            ),
        ]
        receipt = write_receipt(
            receipt_path,
            source_revision,
            results,
            scope,
            source_dirty=source_dirty,
            selection=selection,
        )
        print(
            f"{'PASS' if receipt['passed'] else 'FAIL'} focused quality "
            f"(not full-suite coverage): {receipt_path.relative_to(ROOT)}",
            flush=True,
        )
        return 0 if receipt["passed"] else 1
    except (OSError, RuntimeError) as error:
        _write_failure_receipt(receipt_path, scope, selection, error)
        print(f"FAIL focused quality: {error}", file=sys.stderr, flush=True)
        return 1


def _write_focused_argument_failure(path, arguments, error):
    selection = {
        "tests": [f"rejected-test-{index}" for index in range(len(arguments.tests))],
        "paths": [f"rejected-path-{index}" for index in range(len(arguments.paths))],
    }
    scope = focused_scope(selection["tests"], selection["paths"])
    validation_class = error.validation_class
    if validation_class == "invalid-test-selector":
        diagnostic = "A --test selector failed focused-quality validation."
    elif validation_class == "invalid-path-selector":
        diagnostic = "A --path selector failed focused-quality validation."
    else:
        diagnostic = "Focused quality requires at least one --test or --path selector."
    selection["validation"] = {
        "class": validation_class,
        "diagnostic": diagnostic,
        "counts": {
            "test_selectors": len(arguments.tests),
            "path_selectors": len(arguments.paths),
            "invalid_test_selectors": int(validation_class == "invalid-test-selector"),
            "invalid_path_selectors": int(validation_class == "invalid-path-selector"),
        },
    }
    _write_failure_receipt(path, scope, selection, error, diagnostic=diagnostic)


def _write_failure_receipt(path, scope, selection, error, *, diagnostic=None):
    validation_failure = diagnostic if diagnostic is not None else error
    result = {
        "check": "focused-startup",
        "kind": "contract",
        "passed": False,
        "diagnostic": (
            f"Focused quality run failed before checks completed: "
            f"{diagnostic if diagnostic is not None else error}"
        ),
    }
    failure_scope = (
        f"{scope} Argument validation failed: {validation_failure}. "
        "No selected checks ran."
        if isinstance(error, SelectionError)
        else f"{scope} Startup failed, so the selected checks did not complete."
    )
    try:
        write_receipt(
            path,
            None,
            [result],
            failure_scope,
            selection=selection,
        )
    except OSError:
        # The original startup error is still reported on stderr.
        pass


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
