"""CLI selection must stay inert until a valid, explicitly enabled case runs."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import check_cli_launch as runner


class CaseSelectionTests(unittest.TestCase):
    def invoke(self, *arguments):
        with patch.object(sys, "argv", ["check_cli_launch.py", *arguments]):
            return runner.main()

    def test_listing_needs_no_build_runtime_or_external_process(self):
        output = io.StringIO()
        with (
            contextlib.redirect_stdout(output),
            patch.object(runner.tempfile, "TemporaryDirectory") as runtime,
            patch.object(runner.tempfile, "mkdtemp") as artifacts,
            patch.object(runner.subprocess, "Popen") as process,
        ):
            self.assertEqual(self.invoke("--list-cases", "--build-dir", "/missing"), 0)
        cases = json.loads(output.getvalue())
        self.assertIn("identity-boundaries", [row["case"] for row in cases])
        self.assertEqual(len(cases), len({row["case"] for row in cases}))
        runtime.assert_not_called()
        artifacts.assert_not_called()
        process.assert_not_called()

    def test_invalid_or_disabled_cases_fail_before_creating_artifacts(self):
        for case in ("unknown", "gui", "codex-terminal"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                receipt = Path(directory) / "receipt.json"
                receipt.write_text('{"passed": true}')
                with (
                    contextlib.redirect_stderr(io.StringIO()),
                    patch.object(runner.tempfile, "mkdtemp") as artifacts,
                    self.assertRaises(SystemExit) as failure,
                ):
                    self.invoke("--case", case, "--output", str(receipt))
                self.assertEqual(failure.exception.code, 2)
                self.assertFalse(receipt.exists())
                artifacts.assert_not_called()

    def test_selection_runs_only_the_requested_action(self):
        with (
            patch.object(
                runner, "Service", side_effect=runner.CheckError("selected fixture")
            ) as service,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            results = runner.exercise(
                Path("/missing"), None, None, False, cases={"identity-boundaries"}
            )
        self.assertEqual(service.call_count, 1)
        self.assertEqual(len(results), 1)
        self.assertIn("identity generations", results[0]["name"])
        self.assertFalse(results[0]["passed"])
        self.assertIn("selected fixture", results[0]["error"])

    def test_selected_non_codex_case_does_not_inspect_the_codex_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "receipt.json"

            class FixtureDirectory:
                def __init__(self, *args, **kwargs):
                    pass

                def __enter__(self):
                    return root / "runtime"

                def __exit__(self, *_):
                    return False

            with (
                contextlib.redirect_stdout(io.StringIO()),
                patch.object(runner.tempfile, "TemporaryDirectory", FixtureDirectory),
                patch.object(
                    runner.tempfile,
                    "mkdtemp",
                    return_value=str(root / "artifacts"),
                ),
                patch.object(
                    runner,
                    "exercise",
                    return_value=[{"case": "literal-resize-exit", "passed": False}],
                ) as exercise,
                patch.object(runner.shutil, "which", return_value="/missing-codex"),
                patch.object(
                    runner.Path,
                    "resolve",
                    lambda self, *, strict=False: self,
                ),
                patch.object(sys, "argv", []),
            ):
                argv = [
                    "check_cli_launch.py",
                    "--case",
                    "literal-resize-exit",
                    "--codex",
                ]
                with patch.object(
                    sys, "argv", [*argv, "/missing-codex", "--output", str(receipt)]
                ):
                    self.assertEqual(runner.main(), 1)

            exercise.assert_called_once()
            self.assertEqual(
                exercise.call_args.kwargs["cases"], {"literal-resize-exit"}
            )
            data = json.loads(receipt.read_text())
            self.assertNotIn("codex", data)
            self.assertEqual(data["selected_cases"], ["literal-resize-exit"])
