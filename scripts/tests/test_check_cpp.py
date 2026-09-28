"""Verification failures must preserve evidence and remain failures."""

import os
import json
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


class ReceiptFreshnessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.logs = self.root / "build" / "reports"
        root_patch = patch.object(check_cpp, "ROOT", self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)

    def invoke(self, mode, *arguments):
        argv = patch.object(sys, "argv", ["check_cpp.py", mode, *arguments])
        with argv:
            return check_cpp.main()

    def test_invalid_jobs_replaces_success_without_starting_tools(self):
        directory = self.logs / "dev"
        directory.mkdir(parents=True)
        (directory / "receipt.json").write_text('{"passed": true}\n')
        with patch.object(check_cpp, "toolchain") as toolchain:
            self.assertEqual(self.invoke("dev", "--jobs", "0"), 2)
        toolchain.assert_not_called()
        receipt = self.read_receipt("dev")
        self.assertFalse(receipt["passed"])
        self.assertEqual(receipt["checks"][0]["check"], "arguments")
        self.assertIn("--jobs must be positive", receipt["checks"][0]["diagnostic"])

    def test_toolchain_startup_failure_replaces_a_stale_receipt(self):
        self.logs.mkdir(parents=True)
        (self.logs / "format").mkdir()
        stale = self.logs / "format" / "receipt.json"
        stale.write_text('{"passed": true, "checks": []}\n')

        with patch.object(check_cpp, "toolchain", side_effect=RuntimeError("no llvm")):
            exit_code = self.invoke("format")

        self.assertEqual(exit_code, 1)
        receipt = self.read_receipt("format")
        self.assertFalse(receipt["passed"])
        self.assertEqual(
            receipt["scope"], "format: startup failed before C++ checks could run."
        )
        self.assertEqual(receipt["checks"][0]["check"], "toolchain")
        self.assertIn("no llvm", receipt["checks"][0]["diagnostic"])

    def test_format_success_and_failure_both_record_current_receipts(self):
        executable = self.root / "clang-format"
        executable.write_text("# fixture\n", encoding="utf-8")
        tools = {"clang-format": str(executable)}
        listing = subprocess.CompletedProcess(["git"], 0, "src/example.cpp\0", "")
        for passed in (True, False):
            with (
                patch.object(check_cpp, "toolchain", return_value=tools),
                patch.object(
                    check_cpp.subprocess, "run", return_value=listing
                ) as git_run,
                patch.object(
                    check_cpp,
                    "run",
                    return_value={
                        "check": "format",
                        "passed": passed,
                        "exit_code": 0 if passed else 1,
                    },
                ) as format_run,
                patch.object(
                    check_cpp, "_probe_version", return_value=("Fixture 1.0", None)
                ),
            ):
                exit_code = self.invoke("format")

            self.assertEqual(exit_code, 0 if passed else 1)
            receipt = self.read_receipt("format")
            self.assertEqual(receipt["passed"], passed)
            self.assertEqual(
                receipt["scope"],
                "format: tracked C++ sources only; not compilation or test coverage.",
            )
            self.assertEqual(receipt["checks"][0]["check"], "format")
            self.assertEqual(receipt["checks"][0]["passed"], passed)
            self.assertEqual(format_run.call_count, 1)
            self.assertEqual(git_run.call_count, 1)

    def read_receipt(self, mode):
        return json.loads((self.logs / mode / "receipt.json").read_text())

    def test_malformed_database_replaces_success_with_analysis_setup_failure(self):
        tools = {
            "clang++": "/llvm/clang++",
            "clang-format": "/llvm/clang-format",
            "clang-tidy": "/llvm/clang-tidy",
            "cppcheck": "/tools/cppcheck",
            "cmake": "/bin/cmake",
            "ctest": "/bin/ctest",
        }
        fixtures = [
            ("invalid-json", "{not-json"),
            ("top-level-shape", '{"entries": []}'),
            ("entry-shape", '["not-an-object"]'),
            ("missing-file", '[{"command": "clang++"}]'),
            ("empty-file", '[{"file": ""}]'),
        ]
        expected_diagnostics = {
            "invalid-json": "Expecting property name enclosed in double quotes",
            "top-level-shape": "top level must be a JSON list; got dict",
            "entry-shape": "entry 0 must be an object; got str",
            "missing-file": 'string "file"; got None',
            "empty-file": "string \"file\"; got ''",
        }
        for label, database_text in fixtures:
            with self.subTest(failure=label):
                database = self.root / "build" / "dev" / "compile_commands.json"
                database.parent.mkdir(parents=True, exist_ok=True)
                database.write_text(database_text)
                receipt = self.logs / "dev" / "receipt.json"
                receipt.parent.mkdir(parents=True, exist_ok=True)
                receipt.write_text('{"passed": true, "checks": []}\n')

                with (
                    patch.object(check_cpp, "toolchain", return_value=tools),
                    patch.object(
                        check_cpp,
                        "run",
                        return_value={"check": "fixture", "passed": True},
                    ),
                    patch.object(
                        check_cpp, "_probe_version", return_value=("Fixture 1.0", None)
                    ),
                ):
                    exit_code = self.invoke("dev")

                self.assertEqual(exit_code, 1)
                result = self.read_receipt("dev")
                self.assertFalse(result["passed"])
                failures = [
                    check
                    for check in result["checks"]
                    if check["check"] == "analysis-setup"
                ]
                self.assertEqual(len(failures), 1)
                self.assertFalse(failures[0]["passed"])
                self.assertIn(expected_diagnostics[label], failures[0]["diagnostic"])

    def test_valid_database_schedules_the_intended_analyzers(self):
        tools = {
            "clang++": "/llvm/clang++",
            "clang-format": "/llvm/clang-format",
            "clang-tidy": "/llvm/clang-tidy",
            "cppcheck": "/tools/cppcheck",
            "cmake": "/bin/cmake",
            "ctest": "/bin/ctest",
        }
        database = self.root / "build" / "dev" / "compile_commands.json"
        database.parent.mkdir(parents=True)
        (self.logs / "dev").mkdir(parents=True)
        database.write_text(
            json.dumps(
                [
                    {"file": str(self.root / "apps/desktop/example.cpp")},
                    {"file": str(self.root / "build/generated/moc_example.cpp")},
                ]
            )
        )
        listing = subprocess.CompletedProcess(
            ["git"], 0, "apps/desktop/example.cpp\0apps/desktop/example.h\0", ""
        )

        with (
            patch.object(check_cpp, "toolchain", return_value=tools),
            patch.object(
                check_cpp,
                "run",
                side_effect=lambda label, *_: {
                    "check": label,
                    "passed": True,
                },
            ) as runs,
            patch.object(check_cpp.subprocess, "run", return_value=listing) as git_run,
            patch.object(
                check_cpp, "_probe_version", return_value=("Fixture 1.0", None)
            ),
        ):
            exit_code = self.invoke("dev")

        self.assertEqual(exit_code, 0)
        self.assertTrue(self.read_receipt("dev")["passed"])
        commands = {call.args[0]: call.args[1] for call in runs.call_args_list}
        self.assertEqual(
            set(commands),
            {
                "configure",
                "build",
                "ctest",
                "clang-format",
                "cppcheck",
                "clang-tidy-0",
            },
        )
        source = str(self.root / "apps/desktop/example.cpp")
        format_command = commands["clang-format"]
        self.assertEqual(
            format_command,
            [
                tools["clang-format"],
                "--dry-run",
                "--Werror",
                "apps/desktop/example.cpp",
                "apps/desktop/example.h",
            ],
        )
        cppcheck_command = commands["cppcheck"]
        self.assertEqual(cppcheck_command[0], tools["cppcheck"])
        self.assertIn(
            f"--project={self.logs / 'dev' / 'compile_commands.json'}",
            cppcheck_command,
        )
        tidy_command = commands["clang-tidy-0"]
        self.assertEqual(
            tidy_command,
            [
                tools["clang-tidy"],
                "-p",
                self.root / "build" / "dev",
                source,
            ],
        )
        analysis_database = json.loads(
            (self.logs / "dev" / "compile_commands.json").read_text()
        )
        self.assertEqual(
            [entry["file"] for entry in analysis_database],
            [source],
        )
        git_calls = [
            call for call in git_run.call_args_list if call.args[0][0] == "git"
        ]
        self.assertEqual(len(git_calls), 1)


class VersionProbeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()

    def executable(self, name, body):
        path = self.bin / name
        path.write_text(f"#!/usr/bin/env python3\n{body}\n", encoding="utf-8")
        path.chmod(0o755)
        return path

    def test_failed_version_probe_fails_and_is_recorded_in_receipt(self):
        good = self.executable("good-tool", "print('Fixture 1.2.3')")
        bad = self.executable("bad-tool", "raise SystemExit(7)")
        results = [{"check": "format", "passed": True}]

        passed = check_cpp.write_receipt(
            self.root / "receipt.json",
            {"good": str(good), "bad": str(bad)},
            results,
            "fixture receipt",
        )

        receipt = json.loads((self.root / "receipt.json").read_text())

        self.assertFalse(passed)
        self.assertFalse(receipt["passed"])
        self.assertEqual(receipt["tools"]["good"]["version"], "Fixture 1.2.3")
        self.assertIn("exited with code 7", receipt["tools"]["bad"]["error"])
        failed = receipt["checks"][-1]
        self.assertEqual(failed["check"], "version-probe-bad")
        self.assertFalse(failed["passed"])
        self.assertTrue(receipt["checks"][0]["passed"])


if __name__ == "__main__":
    unittest.main()
