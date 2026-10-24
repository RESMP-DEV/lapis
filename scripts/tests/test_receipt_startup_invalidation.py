"""Interrupted checks must never leave an earlier successful receipt."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_android_remote
from check_claude_hooks import main as claude_main
from check_codex_attention import main as codex_attention_main
from check_history import main as history_main
from probe_codex_attention import main as codex_probe_main
from probe_codex_external_import import main as codex_import_main
from check_service_attention import main as attention_main
from check_sync_output import main as sync_main


class ReceiptInvalidationTests(unittest.TestCase):
    def prepare(self, directory: str):
        output = Path(directory) / "receipt.json"
        output.write_text(json.dumps({"passed": True, "stale": True}))
        return output

    def test_interrupted_sync_check_removes_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            with (
                patch("sys.argv", ["check_sync_output.py", "--output", str(output)]),
                patch("check_sync_output.exercise", side_effect=KeyboardInterrupt),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                sync_main()
            receipt = json.loads(output.read_text())
            self.assertFalse(receipt["passed"])
            self.assertNotIn("stale", receipt)

    def test_interrupted_history_check_removes_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            with (
                patch("sys.argv", ["check_history.py", "--output", str(output)]),
                patch("check_history.exercise", side_effect=KeyboardInterrupt),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                history_main()
            self.assertFalse(output.exists())

    def test_interrupted_claude_check_removes_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            with (
                patch(
                    "check_claude_hooks.bounded_exercise", side_effect=KeyboardInterrupt
                ),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                claude_main(["--output", str(output)])
            self.assertFalse(output.exists())

    def test_interrupted_attention_check_replaces_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            with (
                patch(
                    "sys.argv", ["check_service_attention.py", "--output", str(output)]
                ),
                patch(
                    "check_service_attention.exercise", side_effect=KeyboardInterrupt
                ),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                attention_main()
            self.assertFalse(json.loads(output.read_text())["passed"])
            self.assertNotIn("stale", json.loads(output.read_text()))

    def test_interrupted_live_codex_check_removes_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            argv = ["check_codex_attention.py", "--live-glm", "--output", str(output)]
            with (
                patch("sys.argv", argv),
                patch("check_codex_attention.run", side_effect=KeyboardInterrupt),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                codex_attention_main()
            self.assertFalse(output.exists())

    def test_interrupted_shared_server_probe_removes_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            argv = [
                "probe_codex_attention.py",
                "--codex",
                "codex",
                "--output",
                str(output),
            ]
            with (
                patch("sys.argv", argv),
                patch(
                    "probe_codex_attention.run_probe",
                    side_effect=KeyboardInterrupt,
                ),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                codex_probe_main()
            self.assertFalse(output.exists())

    def test_interrupted_external_import_probe_removes_stale_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.prepare(temporary)
            argv = [
                "probe_codex_external_import.py",
                "--codex",
                "codex",
                "--output",
                str(output),
            ]
            with (
                patch("sys.argv", argv),
                patch(
                    "probe_codex_external_import.run_probe",
                    side_effect=KeyboardInterrupt,
                ),
                patch("builtins.print"),
                self.assertRaises(KeyboardInterrupt),
            ):
                codex_import_main()
            self.assertFalse(output.exists())

    def test_android_check_removes_stale_receipt_before_skipping(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "remote-check.json"
            output.write_text(json.dumps({"passed": True, "stale": True}))
            service = root / "service"
            service.write_text("fixture")
            with (
                patch("sys.argv", ["check_android_remote.py"]),
                patch.object(check_android_remote, "BUILD", root),
                patch.object(check_android_remote, "SERVICE", service),
                patch.object(
                    check_android_remote.android_ctl, "find_sdk", return_value=None
                ),
                patch("builtins.print"),
            ):
                result = check_android_remote.main()
            self.assertEqual(result, check_android_remote.SKIP_EXIT)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
