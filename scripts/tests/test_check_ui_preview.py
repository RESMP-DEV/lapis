"""Behavioral regressions for bounded UI-preview process cleanup."""

import contextlib
import io
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import check_ui_preview as ui_preview


class UiPreviewCleanupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.artifacts = Path(temporary.name)

    def test_cleanup_kills_group_after_leader_has_exited(self):
        status_read, status_write = os.pipe()
        child_code = (
            "import os,signal,time; "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            f"os.write({status_write}, b'R'); time.sleep(30)"
        )
        os.set_blocking(status_read, False)
        leader_code = (
            "import subprocess,sys; "
            f"child=subprocess.Popen([sys.executable,'-c',{child_code!r}], "
            f"stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, "
            f"pass_fds=({status_write},)); "
            "print(child.pid, flush=True)"
        )
        leader = subprocess.Popen(
            [sys.executable, "-c", leader_code],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            stdin=subprocess.DEVNULL,
            pass_fds=(status_write,),
            start_new_session=True,
        )
        os.close(status_write)
        child_pid = None
        try:

            def child_is_running():
                try:
                    return os.read(status_read, 1) != b""
                except BlockingIOError:
                    return True

            output, _ = leader.communicate(timeout=5)
            self.assertTrue(output, "leader fixture exited before reporting its child")
            child_pid = int(output)
            self.assertEqual(leader.returncode, 0)
            self.assertTrue(
                select.select([status_read], [], [], 5)[0],
                "child did not install its SIGTERM handler",
            )
            self.assertEqual(os.read(status_read, 1), b"R")
            self.assertTrue(child_is_running())
            self.assertEqual(os.getpgid(child_pid), leader.pid)

            ui_preview.stop_process_group(
                leader, RuntimeError("fixture operation was interrupted")
            )
            deadline = time.monotonic() + 2
            while child_is_running() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(
                child_is_running(),
                "TERM-resistant child survived its exited group leader",
            )
        finally:
            if child_is_running():
                try:
                    os.killpg(leader.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if leader.poll() is None:
                leader.kill()
                leader.wait(timeout=5)
            os.close(status_read)

    def test_timeout_preserves_partial_output_in_result_and_log(self):
        with patch.object(ui_preview, "TIMEOUT_SECONDS", 0.2):
            result = ui_preview.run(
                "timeout-fixture",
                Path(sys.executable),
                [
                    "-c",
                    "import time; print('fixture partial output', flush=True); "
                    "time.sleep(30)",
                ],
                self.artifacts,
            )
        self.assertFalse(result.passed)
        self.assertIsNone(result.exit_code)
        self.assertIn("fixture partial output", result.output)
        self.assertIn("Timed out after 0.2 seconds", result.output)
        log = (self.artifacts / "timeout-fixture.log").read_text()
        self.assertIn("fixture partial output", log)

    def test_cleanup_failure_preserves_the_original_exception_identity(self):
        original_error = RuntimeError("command dispatch failed")

        class ExplodingProcess:
            pid = 1234

            def communicate(self, timeout=None):
                raise original_error

        cleaned_errors = []

        def record_and_fail(process, original):
            cleaned_errors.append((process, original))
            original.cleanup_failure = "canonical cleanup could not reap"
            raise original

        with (
            patch.object(
                ui_preview.subprocess, "Popen", return_value=ExplodingProcess()
            ),
            patch.object(ui_preview, "stop_process_group", record_and_fail),
            self.assertRaises(RuntimeError) as raised,
        ):
            ui_preview.run(
                "interrupted", Path(sys.executable), ["-c", "unused"], self.artifacts
            )

        self.assertIs(raised.exception, original_error)
        self.assertIs(cleaned_errors[0][1], original_error)
        self.assertEqual(cleaned_errors[0][0].pid, 1234)
        self.assertIn("canonical cleanup", original_error.cleanup_failure)

    def test_missing_binary_replaces_a_stale_receipt_before_any_launch(self):
        binary = self.artifacts / "missing-desktop"
        artifacts = self.artifacts / "receipts"
        artifacts.mkdir()
        receipt = artifacts / "receipt.json"
        receipt.write_text('{"passed": true}\n', encoding="utf-8")

        with (
            contextlib.redirect_stderr(io.StringIO()),
            patch.object(ui_preview.subprocess, "Popen") as process,
            patch.object(
                sys,
                "argv",
                [
                    "check_ui_preview.py",
                    "--binary",
                    str(binary),
                    "--artifacts",
                    str(artifacts),
                ],
            ),
        ):
            self.assertEqual(ui_preview.main(), 2)

        process.assert_not_called()
        self.assertFalse(receipt.exists())


if __name__ == "__main__":
    unittest.main()
