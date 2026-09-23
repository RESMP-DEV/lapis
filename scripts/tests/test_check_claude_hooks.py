"""Behavioral boundaries for Claude's live qualification harness."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_cli_launch import CheckError
from check_claude_hooks import main, stable_notice, terminal_notice


class NoticeTests(unittest.TestCase):
    def request(self):
        return {
            "id": "r",
            "epoch": 1,
            "revision": 2,
            "submitted": False,
            "choices": [],
            "details": {
                "adapter": "claude-code",
                "responseLocation": "terminal",
                "observationOnly": True,
                "toolName": "Bash",
            },
        }

    def test_requires_terminal_only_metadata_without_private_content(self):
        terminal_notice(self.request(), "Bash")
        for update in (
            {"choices": ["allow"]},
            {"submitted": True},
            {"details": {**self.request()["details"], "toolInput": {}}},
        ):
            with self.subTest(update=update), self.assertRaises(CheckError):
                terminal_notice({**self.request(), **update}, "Bash")

    def test_missing_notice_reports_observation_loss(self):
        with self.assertRaisesRegex(CheckError, "Pending notice disappeared"):
            stable_notice(self.request(), None)

    def test_reconnection_preserves_exact_request_token(self):
        before = self.request()
        stable_notice(before, before.copy())
        for field in ("epoch", "revision", "id", "submitted"):
            with self.subTest(field=field), self.assertRaises(CheckError):
                stable_notice(before, {**before, field: "changed"})


class EntryPointTests(unittest.TestCase):
    def test_main_runs_event_loop_and_does_not_launder_failures(self):
        for failure in (
            None,
            TimeoutError("fixture timed out"),
            RuntimeError("cleanup failed"),
        ):
            with (
                self.subTest(failure=failure),
                tempfile.TemporaryDirectory() as temporary,
            ):
                output = Path(temporary) / "receipt.json"
                fixture = AsyncMock(side_effect=failure)
                with (
                    patch("check_claude_hooks.exercise", fixture),
                    patch("builtins.print"),
                ):
                    result = main(["--output", str(output)])
                fixture.assert_awaited_once()
                receipt = json.loads(output.read_text())
                self.assertEqual(result, int(failure is not None))
                self.assertEqual(receipt["passed"], failure is None)

    def test_main_does_not_launder_keyboard_interrupt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "receipt.json"
            fixture = AsyncMock(side_effect=KeyboardInterrupt)
            with (
                patch("check_claude_hooks.exercise", fixture),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                main(["--output", str(output)])
            fixture.assert_awaited_once()
            self.assertFalse(output.exists())
