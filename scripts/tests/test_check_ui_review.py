"""Behavioral orchestration tests for the background UI review runner."""

import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock, patch

from scripts import check_ui_review

UI_STDOUT = " ".join(check_ui_review.UI_BACKGROUND_MARKERS) + "\n"
TERMINAL_STDOUT = " ".join(check_ui_review.TERMINAL_BACKGROUND_MARKERS) + "\n"


class FixedRunner:
    def __init__(self, outputs):
        self.outputs = outputs
        self.calls = []

    def __call__(self, command, **kwargs):
        command = [str(part) for part in command]
        output = self.outputs[min(len(self.calls), len(self.outputs) - 1)]
        self.calls.append((command, kwargs))
        return subprocess.CompletedProcess(command, 0, output, "")

    def save_ui_captures(self, command, **kwargs):
        environment = kwargs.get("env")
        if environment and environment.get("LAPIS_ATTENTION_CAPTURE"):
            Path(environment["LAPIS_ATTENTION_CAPTURE"]).write_bytes(b"png")
            Path(environment["LAPIS_SETTINGS_CAPTURE"]).write_bytes(b"png")
            Path(
                environment["LAPIS_WORKSPACE_CAPTURE_PREFIX"] + "1400x960.png"
            ).write_bytes(b"png")
        return self(command, **kwargs)


class UiReviewOrchestrationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ui-review-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = self.root / "report"
        self.report.mkdir()
        progress = patch.object(check_ui_review, "progress", Mock())
        progress.start()
        self.addCleanup(progress.stop)

    def test_fixture_order_always_selects_explicit_background(self):
        runner = FixedRunner([UI_STDOUT, UI_STDOUT, TERMINAL_STDOUT])
        with patch.object(check_ui_review, "ROOT", self.root):
            results = check_ui_review.run_fixtures(
                check_ui_review.BUILD_DIR, self.report, runner=runner.save_ui_captures
            )

        self.assertEqual(
            [result["name"] for result in results],
            [
                "ui-full-background",
                "ui-shortcuts-background",
                "terminal-input-background",
            ],
        )
        self.assertEqual(
            [command[1:] for command, _ in runner.calls],
            [
                ["--background"],
                ["--background", "--shortcuts-only"],
                ["--background"],
            ],
        )
        for result in results:
            self.assertTrue(result["passed"], result)

    def test_capture_hooks_are_isolated_and_recorded_without_new_launches(self):
        runner = FixedRunner([UI_STDOUT, UI_STDOUT, TERMINAL_STDOUT])
        stale = self.report / "captures" / "ui-full-background" / "workspace-stale.png"
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b"stale")

        with patch.object(check_ui_review, "ROOT", self.root):

            def save_captures(command, **kwargs):
                environment = kwargs.get("env")
                if environment and environment.get("LAPIS_ATTENTION_CAPTURE"):
                    Path(environment["LAPIS_ATTENTION_CAPTURE"]).write_bytes(b"png")
                    Path(environment["LAPIS_SETTINGS_CAPTURE"]).write_bytes(b"png")
                    Path(
                        environment["LAPIS_WORKSPACE_CAPTURE_PREFIX"] + "1400x960.png"
                    ).write_bytes(b"png")
                return runner(command, **kwargs)

            results = check_ui_review.run_fixtures(
                check_ui_review.BUILD_DIR, self.report, runner=save_captures
            )

        self.assertEqual(len(runner.calls), 3, str(results))
        self.assertFalse(stale.exists())
        self.assertEqual(
            results[0]["captures"],
            [
                "report/captures/ui-full-background/attention.png",
                "report/captures/ui-full-background/settings.png",
                "report/captures/ui-full-background/workspace-1400x960.png",
            ],
        )
        self.assertEqual(results[1]["captures"], [])
        self.assertEqual(results[2]["captures"], [])
        for _, kwargs in runner.calls[1:]:
            self.assertEqual(
                {
                    name: kwargs["env"][name]
                    for name in check_ui_review.CAPTURE_VARIABLES
                },
                dict.fromkeys(check_ui_review.CAPTURE_VARIABLES, ""),
            )
        self.assertTrue(
            all(result["passed"] for result in results),
            [result.get("diagnostic") for result in results],
        )

    def test_marker_failure_is_retained_without_a_native_fallback(self):
        runner = FixedRunner(["not the fixture\n"])
        with patch.object(check_ui_review, "ROOT", self.root):
            results = check_ui_review.run_fixtures(
                check_ui_review.BUILD_DIR, self.report, runner=runner
            )

        self.assertEqual(len(results), 3)
        self.assertFalse(any(result["passed"] for result in results))
        self.assertEqual([result["exit_code"] for result in results], [0, 0, 0])
        for result in results:
            self.assertIn("background PASS markers", result["diagnostic"])

    def test_nonzero_fixture_exit_fails_and_preserves_its_log(self):
        def failing_runner(command, **_kwargs):
            return subprocess.CompletedProcess(command, 2, "partial output\n", "")

        with patch.object(check_ui_review, "ROOT", self.root):
            results = check_ui_review.run_fixtures(
                check_ui_review.BUILD_DIR, self.report, runner=failing_runner
            )

        self.assertFalse(results[0]["passed"])
        self.assertEqual(results[0]["exit_code"], 2)
        self.assertTrue((self.report / "ui-full-background.log").is_file())

    def test_build_commands_use_the_shared_configure_and_only_two_targets(self):
        with patch.object(
            check_ui_review, "configure_command", return_value=["shared-configure"]
        ) as shared:
            configure, build = check_ui_review.build_commands(
                {"clang++": "/llvm/clang++", "cmake": "/bin/cmake"}, 5
            )

        shared.assert_called_once_with(
            {"clang++": "/llvm/clang++", "cmake": "/bin/cmake"}, "desktop"
        )
        self.assertEqual(configure, ["shared-configure"])
        self.assertEqual(
            build,
            [
                "/bin/cmake",
                "--build",
                "--preset",
                "desktop",
                "--parallel",
                "5",
                "--target",
                check_ui_review.UI_TARGET,
                check_ui_review.TERMINAL_TARGET,
            ],
        )
        joined = " ".join([*configure, *build])
        for forbidden in ("ctest", "clang-tidy", "cppcheck", "--native"):
            self.assertNotIn(forbidden, joined)

    def review_with_fixture_runner(self, runner, fixture_cases=None):
        inventory = (
            fixture_cases
            if fixture_cases is not None
            else check_ui_review.fixture_cases(check_ui_review.BUILD_DIR)
        )
        with (
            patch.object(check_ui_review, "ROOT", self.root),
            patch.object(
                check_ui_review,
                "source_metadata",
                return_value=("0" * 40, False),
            ),
            patch.object(
                check_ui_review,
                "toolchain",
                return_value={"clang++": "/llvm/clang++", "cmake": "/bin/cmake"},
            ),
            patch.object(
                check_ui_review,
                "configure_command",
                return_value=["shared-configure"],
            ),
            patch.object(
                check_ui_review, "prepare_report_directory", return_value=self.report
            ),
            patch.object(
                check_ui_review,
                "fixture_cases",
                return_value=inventory,
            ),
        ):
            stream = io.StringIO()
            with redirect_stdout(stream):
                exit_code = check_ui_review.review(
                    jobs=2, json_output=True, runner=runner
                )
        return exit_code, json.loads(stream.getvalue())

    def test_json_review_writes_a_failure_receipt_with_every_serial_outcome(self):
        def fixture_runner(command, **kwargs):
            if command[0].endswith("terminal_input_tests"):
                return subprocess.CompletedProcess(command, 3, "failed fixture\n", "")
            output = (
                UI_STDOUT
                if command[0].endswith("ui_preview_tests")
                else "configure or build\n"
            )
            return subprocess.CompletedProcess(command, 0, output, "")

        exit_code, receipt = self.review_with_fixture_runner(fixture_runner)

        self.assertEqual(exit_code, 1)
        self.assertFalse(receipt["passed"])
        self.assertTrue(receipt["configure"]["passed"])
        self.assertTrue(receipt["build"]["passed"])
        self.assertEqual(
            [case["name"] for case in receipt["test_cases"]],
            [
                "ui-full-background",
                "ui-shortcuts-background",
                "terminal-input-background",
            ],
        )
        self.assertEqual(receipt["test_cases"][-1]["exit_code"], 3)
        self.assertFalse(receipt["scope"]["native_gpu_exercised"])
        self.assertFalse(receipt["scope"]["native_os_input_exercised"])
        self.assertEqual(
            receipt,
            json.loads((self.report / "receipt.json").read_text()),
        )

    def test_empty_fixture_inventory_is_refused_rather_than_passing_vacuously(self):
        exit_code, receipt = self.review_with_fixture_runner(
            FixedRunner([]), fixture_cases=()
        )

        self.assertEqual(exit_code, 1)
        self.assertFalse(receipt["passed"])
        self.assertIn("empty UI-review fixture inventory", receipt["error"])
        self.assertEqual(receipt["test_cases"], [])


if __name__ == "__main__":
    unittest.main()
