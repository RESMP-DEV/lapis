"""Behavioral regressions for the non-GUI quality runner."""

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
