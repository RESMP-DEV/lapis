"""Verification failures must preserve evidence and remain failures."""

import os
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


class BuildSelectionTests(unittest.TestCase):
    def test_selective_toolchain_does_not_require_analyzers(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        llvm = Path(temporary.name)
        compiler = llvm / "clang++"
        compiler.write_text("")
        compiler.chmod(0o700)
        with (
            patch.dict(os.environ, {"LAPIS_LLVM_BIN": str(llvm)}),
            patch.object(check_cpp.shutil, "which", return_value="/path/cmake"),
        ):
            tools = check_cpp.toolchain(("clang++", "cmake"))

        self.assertEqual(
            tools,
            {"clang++": str(compiler), "cmake": "/path/cmake"},
        )

    def test_desktop_configure_reuses_the_pinned_build_selection(self):
        tools = {"clang++": "/llvm/clang++", "cmake": "/bin/cmake"}
        with (
            patch.dict(
                os.environ,
                {"LAPIS_GHOSTTY_PREFIX": "/prefix"},
            ),
            patch.object(check_cpp.shutil, "which", return_value="/path/ccache"),
            patch.object(
                check_cpp.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, "/sdk\n"),
            ) as xcrun,
        ):
            configure = check_cpp.configure_command(tools, "desktop")

        self.assertEqual(
            configure,
            [
                "/bin/cmake",
                "--preset",
                "desktop",
                "-DCMAKE_CXX_COMPILER=/llvm/clang++",
                "-DLAPIS_GHOSTTY_PREFIX=/prefix",
                "-DCMAKE_CXX_COMPILER_LAUNCHER=/path/ccache",
                *(["-DCMAKE_OSX_SYSROOT=/sdk"] if sys.platform == "darwin" else []),
            ],
        )
        if sys.platform == "darwin":
            xcrun.assert_called_once_with(
                ["xcrun", "--show-sdk-path"],
                capture_output=True,
                text=True,
                check=True,
                timeout=15,
            )


if __name__ == "__main__":
    unittest.main()
