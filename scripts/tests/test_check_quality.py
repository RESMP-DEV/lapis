"""Behavioral regressions for the non-GUI quality runner."""

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import check_quality


class RepositoryContractTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "AGENTS.md").write_text("# instructions\n", encoding="utf-8")
        (self.root / ".gitignore").write_text(".sindexer/\n", encoding="utf-8")

    def test_accepts_literal_relative_instruction_symlink(self):
        (self.root / "CLAUDE.md").symlink_to("AGENTS.md")
        result = check_quality.check_instruction_link(self.root)
        self.assertTrue(result["passed"], result)

    def test_rejects_broken_instruction_symlink(self):
        (self.root / "CLAUDE.md").symlink_to("missing.md")
        result = check_quality.check_instruction_link(self.root)
        self.assertFalse(result["passed"])
        self.assertIn("missing", result["diagnostic"])

    def test_rejects_absolute_instruction_symlink(self):
        (self.root / "CLAUDE.md").symlink_to(self.root / "AGENTS.md")
        result = check_quality.check_instruction_link(self.root)
        self.assertFalse(result["passed"])
        self.assertIn("relative", result["diagnostic"])

    def test_accepts_exact_sindexer_ignore_line(self):
        result = check_quality.check_sindexer_ignore(self.root)
        self.assertTrue(result["passed"], result)

    def test_rejects_missing_sindexer_ignore_line(self):
        (self.root / ".gitignore").write_text("/build/\n", encoding="utf-8")
        result = check_quality.check_sindexer_ignore(self.root)
        self.assertFalse(result["passed"])
        self.assertEqual(result["check"], "sindexer-ignore")


class CommandAggregationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = self.root / "reports"
        self.tools = {"git": "/usr/bin/git", "ruff": "/usr/bin/ruff"}

    def test_all_checks_run_and_order_is_stable_when_commands_fail(self):
        def failure(name, command, report_dir, cwd):
            return {
                "check": name,
                "passed": False,
                "exit_code": 2,
                "elapsed_seconds": 0.1,
                "log": str((report_dir / f"{name}.log").relative_to(self.root)),
            }

        with patch.object(check_quality, "_run_bounded", side_effect=failure) as run:
            results = check_quality.run_quality_checks(
                self.root,
                self.report,
                self.tools,
                python_executable="python-under-test",
            )

        self.assertEqual(
            [result["check"] for result in results],
            [
                "claude-instructions-symlink",
                "sindexer-ignore",
                "git-diff-check",
                "ruff-check",
                "ruff-format-check",
                "python-unittests",
            ],
        )
        self.assertEqual(run.call_count, 4)
        self.assertFalse(any(result["passed"] for result in results[2:]))
        unittest_command = next(
            call.args[1]
            for call in run.call_args_list
            if call.args[0] == "python-unittests"
        )
        self.assertIn("python-under-test", unittest_command)

    def test_missing_ruff_is_reported_as_a_failed_command(self):
        result = check_quality._run_bounded(
            "ruff-check", [None, "check"], self.report, self.root
        )
        self.assertFalse(result["passed"])
        self.assertIn("Missing required executable", result["diagnostic"])
        self.assertTrue((self.report / "ruff-check.log").is_file())

    def test_python_commands_cover_every_first_party_directory(self):
        def result(name, command, report_dir, cwd):
            return {
                "check": name,
                "passed": True,
                "exit_code": 0,
                "log": "unused.log",
            }

        with patch.object(check_quality, "_run_bounded", side_effect=result) as run:
            check_quality.run_quality_checks(self.root, self.report, self.tools)

        ruff_commands = {
            call.args[0]: call.args[1]
            for call in run.call_args_list
            if call.args[0] in {"ruff-check", "ruff-format-check"}
        }
        for command in ruff_commands.values():
            self.assertEqual(
                command[-len(check_quality.PYTHON_CHECK_DIRECTORIES) :],
                list(check_quality.PYTHON_CHECK_DIRECTORIES),
            )
            self.assertNotIn("scripts", command[:-3])

    def test_runner_exception_is_reported_and_does_not_stop_aggregation(self):
        def failure(name, command, report_dir, cwd):
            if name == "git-diff-check":
                raise OSError("launcher closed")
            return {
                "check": name,
                "passed": True,
                "exit_code": 0,
                "log": "unused.log",
            }

        with patch.object(check_quality, "_run_bounded", side_effect=failure):
            results = check_quality.run_quality_checks(
                self.root, self.report, self.tools
            )

        self.assertEqual(len(results), 6)
        failed = next(
            result for result in results if result["check"] == "git-diff-check"
        )
        self.assertFalse(failed["passed"])
        self.assertIn("launcher closed", failed["diagnostic"])
        self.assertTrue(results[-1]["passed"])

    def test_aggregate_receipt_fails_but_retains_every_outcome(self):
        results = [
            {"check": "first", "passed": True},
            {"check": "second", "passed": False, "diagnostic": "expected failure"},
            {"check": "third", "passed": True},
        ]
        receipt = check_quality.write_receipt(
            self.report / "receipt.json", "0" * 40, results, "test scope"
        )
        self.assertEqual(receipt["schema_version"], 1)
        self.assertFalse(receipt["passed"])
        self.assertEqual(
            [check["check"] for check in receipt["checks"]],
            ["first", "second", "third"],
        )


class FocusedSelectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "scripts" / "tests").mkdir(parents=True)
        (self.root / "scripts" / "tests" / "test_example.py").write_text(
            "", encoding="utf-8"
        )
        (self.root / "scripts" / "notes.txt").write_text("", encoding="utf-8")
        (self.root / "scripts" / "tool.py").write_text("", encoding="utf-8")
        (self.root / "scripts" / "test_stray.py").write_text("", encoding="utf-8")
        (self.root / "scripts" / "tests" / "helper.py").write_text("", encoding="utf-8")

    def test_dotted_module_selector_maps_to_existing_file(self):
        self.assertEqual(
            check_quality.parse_test_selectors(
                self.root, ["scripts.tests.test_example"]
            ),
            ["scripts.tests.test_example"],
        )

    def test_path_selector_converts_to_module_name(self):
        self.assertEqual(
            check_quality.parse_test_selectors(
                self.root, ["scripts/tests/test_example.py"]
            ),
            ["scripts.tests.test_example"],
        )

    def test_absolute_path_inside_root_converts_to_module_name(self):
        selector = str(self.root / "scripts" / "tests" / "test_example.py")
        self.assertEqual(
            check_quality.parse_test_selectors(self.root, [selector]),
            ["scripts.tests.test_example"],
        )

    def test_duplicate_test_selectors_are_deduplicated(self):
        self.assertEqual(
            check_quality.parse_test_selectors(
                self.root,
                [
                    "scripts.tests.test_example",
                    "scripts/tests/test_example.py",
                ],
            ),
            ["scripts.tests.test_example"],
        )

    def test_missing_dotted_module_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_test_selectors(self.root, ["scripts.tests.missing"])

    def test_module_selector_must_resemble_a_python_module(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_test_selectors(self.root, ["not a module"])

    def test_directory_test_selector_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_test_selectors(self.root, ["scripts/tests"])

    def test_non_test_module_under_scripts_is_rejected(self):
        for selector in ["scripts.tool", "scripts/tool.py"]:
            with self.assertRaises(check_quality.SelectionError) as raised:
                check_quality.parse_test_selectors(self.root, [selector])
            self.assertIn(
                "must name a module under scripts/tests", str(raised.exception)
            )
            self.assertIn("scripts/tool.py", str(raised.exception))

    def test_test_like_filename_outside_scripts_tests_is_rejected(self):
        for selector in ["scripts.test_stray", "scripts/test_stray.py"]:
            with self.assertRaises(check_quality.SelectionError) as raised:
                check_quality.parse_test_selectors(self.root, [selector])
            self.assertIn(
                "must name a module under scripts/tests", str(raised.exception)
            )
            self.assertIn("scripts/test_stray.py", str(raised.exception))

    def test_non_test_file_under_scripts_tests_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError) as raised:
            check_quality.parse_test_selectors(self.root, ["scripts.tests.helper"])
        self.assertIn("must name a test_*.py module", str(raised.exception))
        self.assertIn("helper.py", str(raised.exception))

    def test_selector_outside_root_is_rejected(self):
        outside = self.root.parent / "outside.py"
        outside.write_text("", encoding="utf-8")
        self.addCleanup(outside.unlink)
        for selector in [str(outside), "../outside.py"]:
            with self.assertRaises(check_quality.SelectionError):
                check_quality.parse_test_selectors(self.root, [selector])

    def test_empty_test_selector_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_test_selectors(self.root, ["  "])

    def test_source_selector_accepts_python_file_and_directory(self):
        self.assertEqual(
            check_quality.parse_source_selectors(
                self.root,
                ["scripts/tests/test_example.py", "scripts", "scripts"],
            ),
            ["scripts/tests/test_example.py", "scripts"],
        )

    def test_missing_source_selector_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_source_selectors(self.root, ["scripts/missing.py"])

    def test_non_python_source_file_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_source_selectors(self.root, ["scripts/notes.txt"])

    def test_empty_source_selector_is_rejected(self):
        with self.assertRaises(check_quality.SelectionError):
            check_quality.parse_source_selectors(self.root, [""])

    def test_scope_states_that_selection_is_not_full_coverage(self):
        scope = check_quality.focused_scope(
            ["scripts.tests.test_example"], ["scripts/check_quality.py"]
        )
        self.assertIn("not full-suite coverage", scope)
        self.assertIn("scripts.tests.test_example", scope)
        self.assertIn("scripts/check_quality.py", scope)
        empty = check_quality.focused_scope([], [])
        self.assertIn("not full-suite coverage", empty)
        self.assertIn("Ruff check/format did not run", empty)
        self.assertIn("Python unit tests did not run", empty)


class FocusedCommandTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = self.root / "reports"
        self.tools = {"git": "/usr/bin/git", "ruff": "/usr/bin/ruff"}

    def _commands(self, **kwargs):
        captured = []

        def capture(name, command, report_dir, cwd):
            captured.append((name, [str(part) for part in command]))
            return {
                "check": name,
                "passed": True,
                "exit_code": 0,
                "log": "unused.log",
            }

        with patch.object(check_quality, "_run_bounded", side_effect=capture):
            results = check_quality.run_quality_checks(
                self.root,
                self.report,
                self.tools,
                python_executable="python-under-test",
                **kwargs,
            )
        commands = dict(captured)
        return [result["check"] for result in results], commands

    def test_focused_run_scopes_ruff_and_runs_selected_modules(self):
        names, commands = self._commands(
            focused=True,
            test_modules=["scripts.tests.test_example"],
            source_paths=["scripts/check_quality.py"],
        )
        self.assertEqual(
            names,
            [
                "claude-instructions-symlink",
                "sindexer-ignore",
                "git-diff-check",
                "ruff-check",
                "ruff-format-check",
                "python-unittests",
            ],
        )
        for name in ["ruff-check", "ruff-format-check"]:
            self.assertEqual(commands[name][-1], "scripts/check_quality.py")
            self.assertNotIn("tools", commands[name])
        self.assertEqual(
            commands["python-unittests"],
            [
                "python-under-test",
                "-m",
                "unittest",
                "-v",
                "scripts.tests.test_example",
            ],
        )

    def test_focused_tests_only_run_omits_ruff(self):
        names, commands = self._commands(
            focused=True,
            test_modules=["scripts.tests.test_example"],
        )
        self.assertNotIn("ruff-check", names)
        self.assertNotIn("ruff-format-check", names)
        self.assertIn("python-unittests", names)
        self.assertIn("-m", commands["python-unittests"])

    def test_focused_paths_only_run_omits_unit_tests(self):
        names, commands = self._commands(
            focused=True,
            source_paths=["scripts/check_quality.py"],
        )
        self.assertIn("ruff-check", names)
        self.assertNotIn("python-unittests", names)
        self.assertEqual(commands["ruff-check"][-1], "scripts/check_quality.py")

    def test_unselected_run_keeps_full_discovery_commands(self):
        names, commands = self._commands()
        self.assertEqual(
            names[-3:],
            ["ruff-check", "ruff-format-check", "python-unittests"],
        )
        self.assertEqual(
            commands["ruff-check"][-3:],
            list(check_quality.PYTHON_CHECK_DIRECTORIES),
        )
        self.assertIn("-c", commands["python-unittests"])


class FocusedRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "AGENTS.md").write_text("# instructions\n", encoding="utf-8")
        (self.root / ".gitignore").write_text(".sindexer/\n", encoding="utf-8")
        (self.root / "CLAUDE.md").symlink_to("AGENTS.md")
        (self.root / "scripts" / "tests").mkdir(parents=True)
        (self.root / "scripts" / "tests" / "test_example.py").write_text(
            "", encoding="utf-8"
        )
        self.receipt = (
            self.root / "build" / "reports" / "quality-focused" / "receipt.json"
        )

    @staticmethod
    def _passing(name, command, report_dir, cwd):
        report_dir.mkdir(parents=True, exist_ok=True)
        (report_dir / f"{name}.log").write_text("", encoding="utf-8")
        return {
            "check": name,
            "passed": True,
            "exit_code": 0,
            "log": f"{name}.log",
        }

    def _patches(self, run_quality_checks=None):
        revision_result = {"check": "source-revision", "passed": True}
        patches = [
            patch.object(check_quality, "ROOT", self.root),
            patch.object(
                check_quality,
                "find_source_revision",
                return_value=("a" * 40, revision_result),
            ),
            patch.object(
                check_quality, "_run_guarded", side_effect=type(self)._passing
            ),
            patch.object(
                check_quality, "_run_bounded", side_effect=type(self)._passing
            ),
        ]
        if run_quality_checks is not None:
            patches.append(
                patch.object(
                    check_quality, "run_quality_checks", side_effect=run_quality_checks
                )
            )
        return patches

    def _run_main(self, *arguments, patches=()):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            with patch("sys.argv", ["check_quality.py", *arguments]):
                with contextlib.ExitStack() as stack:
                    for context in patches:
                        stack.enter_context(context)
                    exit_code = check_quality.main()
        return exit_code, stdout.getvalue()

    def test_focused_run_writes_scoped_receipt_without_touching_full_receipt(self):
        exit_code, output = self._run_main(
            "--focused",
            "--test",
            "scripts/tests/test_example.py",
            "--path",
            "scripts/tests/test_example.py",
            patches=self._patches(),
        )
        self.assertEqual(exit_code, 0, output)
        self.assertIn("quality-focused/receipt.json", output)
        self.assertIn("not full-suite coverage", output)
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertTrue(receipt["passed"])
        self.assertEqual(
            receipt["selection"],
            {
                "tests": ["scripts.tests.test_example"],
                "paths": ["scripts/tests/test_example.py"],
            },
        )
        self.assertIn("not full-suite coverage", receipt["scope"])
        self.assertFalse((self.root / "build" / "reports" / "quality").exists())

    def test_focused_run_accepts_dotted_module_selector(self):
        exit_code, _ = self._run_main(
            "--focused",
            "--test",
            "scripts.tests.test_example",
            patches=self._patches(),
        )
        self.assertEqual(exit_code, 0)
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertEqual(
            receipt["selection"]["tests"],
            ["scripts.tests.test_example"],
        )
        self.assertEqual(receipt["selection"]["paths"], [])

    def test_focused_run_replaces_stale_receipt_and_retains_startup_failure(self):
        self.receipt.parent.mkdir(parents=True)
        self.receipt.write_text('{"passed": true}\n', encoding="utf-8")

        def fail(*arguments, **kwargs):
            raise RuntimeError("boom")

        exit_code, output = self._run_main(
            "--focused",
            "--test",
            "scripts.tests.test_example",
            patches=self._patches(run_quality_checks=fail),
        )
        self.assertEqual(exit_code, 1, output)
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertFalse(receipt["passed"])
        self.assertEqual(receipt["checks"][0]["check"], "focused-startup")
        self.assertIn("boom", receipt["checks"][0]["diagnostic"])
        self.assertIn("not full-suite coverage", receipt["scope"])

    def test_invalid_selection_is_rejected_before_any_check_runs(self):
        self.receipt.parent.mkdir(parents=True)
        sentinel = '{"passed": true}\n'
        self.receipt.write_text(sentinel, encoding="utf-8")
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            with self.assertRaises(SystemExit) as raised:
                self._run_main(
                    "--focused",
                    "--test",
                    "scripts.tests.missing",
                    patches=self._patches(),
                )
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("does not map to an existing Python file", stderr.getvalue())
        self.assertEqual(self.receipt.read_text(encoding="utf-8"), sentinel)

    def test_selectors_without_focused_flag_are_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                self._run_main(
                    "--test",
                    "scripts.tests.test_example",
                    patches=self._patches(),
                )
        self.assertEqual(raised.exception.code, 2)

    def test_focused_without_selectors_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                self._run_main("--focused", patches=self._patches())
        self.assertEqual(raised.exception.code, 2)


class FocusedInvocationTests(unittest.TestCase):
    def test_cli_rejects_unknown_test_module_without_running_checks(self):
        with tempfile.TemporaryDirectory() as root:
            result = subprocess.run(
                [
                    sys.executable,
                    check_quality.__file__,
                    "--focused",
                    "--test",
                    "scripts.tests.does_not_exist",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=10,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("does not map to an existing Python file", result.stderr)


class InvocationTests(unittest.TestCase):
    def test_direct_help_works_outside_the_checkout(self):
        with tempfile.TemporaryDirectory() as root:
            result = subprocess.run(
                [sys.executable, check_quality.__file__, "--help"],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=10,
            )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_empty_python_suite_fails(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "scripts/tests").mkdir(parents=True)
            result = subprocess.run(
                [sys.executable, "-c", check_quality.PYTHON_TEST_PROGRAM],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=10,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No Python tests discovered", result.stderr)


if __name__ == "__main__":
    unittest.main()
