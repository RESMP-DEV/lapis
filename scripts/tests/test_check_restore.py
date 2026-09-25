"""Restore probe uses shared wire framing and isolated model listeners."""

import json
import os
import plistlib
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from scripts import check_restore as restore
from scripts import restore_at_login as login


class RestoreProbeTests(unittest.TestCase):
    def test_installed_agent_retains_custom_data_locations(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            desktop = root / "desktop"
            desktop.touch()
            plist = root / "agent.plist"
            paths = {
                "CODEX_HOME": str(root / "custom codex"),
                "CLAUDE_CONFIG_DIR": str(root / "custom claude"),
                "LAPIS_HISTORY_ROOT": str(root / "custom history"),
            }
            with (
                patch.dict(
                    os.environ,
                    {
                        **paths,
                        "PATH": "/fixture/.venv/bin:/usr/bin",
                        "ANTHROPIC_AUTH_TOKEN": "fixture-only",
                    },
                    clear=True,
                ),
                patch.object(login, "DESKTOP", desktop),
                patch.object(login, "PLIST", plist),
                patch.object(login, "LOG", root / "restore.log"),
                patch.object(login, "launchctl"),
                patch("builtins.print"),
            ):
                login.install()
            env = plistlib.loads(plist.read_bytes())["EnvironmentVariables"]
            self.assertEqual(env, {**paths, "PATH": "/usr/bin"})
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(login.environment(), {})

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
