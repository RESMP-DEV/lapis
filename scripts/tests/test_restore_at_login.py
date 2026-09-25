"""Focused regressions for the login helper's generated launch environment."""

import os
import plistlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import restore_at_login as login


class RestoreLoginTests(unittest.TestCase):
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
            environment = plistlib.loads(plist.read_bytes())["EnvironmentVariables"]
            self.assertEqual(environment, {**paths, "PATH": "/usr/bin"})
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(login.environment(), {})


if __name__ == "__main__":
    unittest.main()
