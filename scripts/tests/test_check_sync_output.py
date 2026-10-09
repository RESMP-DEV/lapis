"""Receipt failures from the synchronized-output probe must stay on disk."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_sync_output as probe


class ReceiptTests(unittest.TestCase):
    def test_service_eof_writes_a_failed_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "receipt.json"

            @contextlib.contextmanager
            def runtime(*arguments, **keywords):
                yield Path(directory)

            with (
                patch.object(
                    sys, "argv", ["check_sync_output.py", "--output", str(output)]
                ),
                patch.object(tempfile, "TemporaryDirectory", runtime),
                patch.object(
                    probe, "exercise", side_effect=EOFError("Service disconnected")
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(probe.main(), 1)
            receipt = json.loads(output.read_text())
            self.assertFalse(receipt["passed"])
            self.assertEqual(receipt["error"], "EOFError: Service disconnected")


if __name__ == "__main__":
    unittest.main()
