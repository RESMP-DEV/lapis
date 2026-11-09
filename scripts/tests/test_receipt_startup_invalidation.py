"""Interrupted checks must never leave an earlier successful receipt."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_claude_hooks import main as claude_main
from check_history import main as history_main


class ReceiptInvalidationTests(unittest.TestCase):
    def prepare(self, directory: str):
        output = Path(directory) / "receipt.json"
        output.write_text(json.dumps({"passed": True, "stale": True}))
        return output

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


if __name__ == "__main__":
    unittest.main()
