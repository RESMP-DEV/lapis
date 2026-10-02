"""Restore probe uses shared wire framing and isolated model listeners."""

import json
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch, sentinel

from scripts import check_restore as restore


class RestoreProbeTests(unittest.TestCase):
    def test_idle_screen_keeps_the_cached_shared_wire_snapshot(self):
        client = Mock(cached_snapshot={"text": "retained terminal"})
        client.receive.side_effect = socket.timeout
        self.assertEqual(restore.screen(client, 0.001), "retained terminal")

    def test_record_wait_requires_the_requested_provenance(self):
        advisory = {"agent": "codex", "session_id": "id", "source": "terminal"}
        observed = {**advisory, "source": "observer"}
        with patch.object(restore, "record", side_effect=[advisory, observed]) as read:
            with patch.object(restore.time, "sleep"):
                self.assertEqual(
                    restore.wait_record("unused", "codex", source="observer"), "id"
                )
            self.assertEqual(read.call_count, 2)

    def test_managed_resume_accepts_only_the_complete_owned_pair(self):
        arguments = ["-c", "check_for_update_on_startup=false", "resume", "saved"]
        provenance = {"index": 2, "identity": "saved"}
        restore.require_managed_resume("codex", arguments, provenance, "saved")
        for bad_arguments, bad_provenance in (
            (arguments + ["resume", "saved"], provenance),
            (arguments[:-1] + ["stale"], provenance),
            ([*arguments[:2], "--resume", "saved"], provenance),
            (arguments, {"index": 0, "identity": "saved"}),
            (arguments, {"identity": "saved"}),
            (arguments, {"index": 2, "identity": "stale"}),
        ):
            with self.subTest(arguments=bad_arguments, provenance=bad_provenance):
                with self.assertRaises(restore.Failure):
                    restore.require_managed_resume(
                        "codex", bad_arguments, bad_provenance, "saved"
                    )

    def test_parallel_approvals_ready_requires_both_distinct_requests(self):
        def request(identifier, submitted=False):
            return {
                "id": identifier,
                "reason": "approval",
                "submitted": submitted,
                "details": {"toolName": "Bash"},
            }

        self.assertFalse(restore.parallel_approvals_ready({"requests": []}))
        self.assertFalse(
            restore.parallel_approvals_ready(
                {"requests": [request("same", True), request("same", True)]}
            )
        )
        self.assertFalse(
            restore.parallel_approvals_ready(
                {"requests": [request("same"), request("same")]}
            )
        )
        self.assertTrue(
            restore.parallel_approvals_ready(
                {"requests": [request("one"), request("two")]}
            )
        )

    def test_screen_caches_only_matching_attention_snapshots(self):
        attachment = b"a" * 40
        state = {"attachment": attachment}
        client = Mock(attachment=attachment, cached_snapshot={"text": ""})
        seen_snapshot = False

        def receive(_timeout):
            nonlocal seen_snapshot
            if not seen_snapshot:
                seen_snapshot = True
                return restore.wire.ATTENTION_SNAPSHOT, attachment
            raise socket.timeout

        client.receive.side_effect = receive
        with patch.object(restore, "attention_snapshot", return_value=state):
            self.assertEqual(restore.screen(client, 0.001), "")
        self.assertEqual(client.attention, state)

    def test_screen_rejects_attention_from_another_attachment(self):
        client = Mock(attachment=b"a" * 40, cached_snapshot={"text": ""})
        client.attention = sentinel.cached
        client.receive.side_effect = [
            (restore.wire.ATTENTION_SNAPSHOT, b"b" * 40),
            socket.timeout(),
        ]
        state = {"attachment": b"b" * 40}
        with patch.object(restore, "attention_snapshot", return_value=state):
            with self.assertRaises(restore.Failure):
                restore.screen(client, 0.001)
        self.assertIs(client.attention, sentinel.cached)

    def test_parallel_prompt_names_both_distinct_bash_commands(self):
        lowered = restore.PARALLEL_PROMPT.lower()
        self.assertIn("two distinct bash tool calls", lowered)
        self.assertTrue(
            all(
                command in restore.PARALLEL_PROMPT
                for command in restore.PARALLEL_COMMANDS
            )
        )

    def test_claude_restore_fixture_requests_explicit_parallel_approvals(self):
        self.assertEqual(
            restore.CLAUDE_FIXTURE_SETTINGS,
            {
                "permissions": {
                    "defaultMode": "default",
                    "ask": [
                        "Bash(echo lapis-parallel-one)",
                        "Bash(echo lapis-parallel-two)",
                    ],
                }
            },
        )

    def test_fake_model_announces_its_bound_ephemeral_port(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            ready = root / "ready.json"
            process = subprocess.Popen(
                [
                    sys.executable,
                    str(restore.ROOT / "scripts/fake_models.py"),
                    "--port",
                    "0",
                    "--ready-file",
                    str(ready),
                    "--log",
                    str(root / "requests.jsonl"),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            try:
                deadline = time.monotonic() + 5
                while (
                    not ready.exists()
                    and process.poll() is None
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.01)
                self.assertTrue(
                    ready.exists(), "fake model must bind before publishing readiness"
                )
                port = json.loads(ready.read_text())["port"]
                self.assertGreater(port, 0)
                with socket.create_connection(
                    ("127.0.0.1", port), timeout=2
                ) as connection:
                    connection.sendall(
                        b"GET /models HTTP/1.1\r\nHost: localhost\r\n\r\n"
                    )
                    self.assertIn(b"200 OK", connection.recv(4096))
            finally:
                process.terminate()
                process.communicate(timeout=5)
