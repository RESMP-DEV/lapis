"""Verification failures must preserve evidence and remain failures."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import check_cpp


class VerificationRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.logs = Path(temporary.name)
        root = patch.object(check_cpp, "ROOT", self.logs)
        root.start()
        self.addCleanup(root.stop)

    def test_timeout_retains_real_partial_output_and_is_never_expected_success(self):
        result = check_cpp.run(
            "timeout",
            [
                sys.executable,
                "-c",
                "import time; print('fixture diagnostic', flush=True); time.sleep(30)",
            ],
            self.logs,
            timeout=1,
            expect_failure="fixture diagnostic",
        )
        self.assertFalse(result["passed"])
        self.assertTrue(result["timed_out"])
        log = (self.logs / "timeout.log").read_text()
        self.assertIn("fixture diagnostic", log)
        self.assertIn("verification timeout", log)

    def test_missing_tool_has_a_failed_receipt_and_log(self):
        result = check_cpp.run("missing", [self.logs / "does-not-exist"], self.logs)
        self.assertFalse(result["passed"])
        self.assertIsNone(result["exit_code"])
        self.assertIn("Could not run", (self.logs / "missing.log").read_text())

    def test_expected_failure_needs_nonzero_exit_and_matching_diagnostic(self):
        for code, output, passed in [
            (0, "fault", False),
            (1, "different", False),
            (1, "fault", True),
        ]:
            with (
                self.subTest(code=code, output=output),
                patch.object(
                    check_cpp,
                    "run_process",
                    return_value=subprocess.CompletedProcess(["fixture"], code, output),
                ),
            ):
                result = check_cpp.run(
                    "expected", ["fixture"], self.logs, expect_failure="fault"
                )
                self.assertEqual(result["passed"], passed)


if __name__ == "__main__":
    unittest.main()
