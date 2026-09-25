"""Device discovery diagnostics without building or installing an app."""

import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from scripts import install_ios_app as install


class DiscoveryTests(unittest.TestCase):
    def discover(self, payload, code=0):
        def run(command, **kwargs):
            path = Path(command[-1])
            self.addCleanup(lambda: self.assertFalse(path.exists()))
            path.write_text(
                payload if isinstance(payload, str) else json.dumps(payload)
            )
            return subprocess.CompletedProcess(
                command, code, "", "fixture failure" if code else ""
            )

        with patch.object(install.subprocess, "run", side_effect=run):
            return install.reachable_phone()

    def test_none_one_and_multiple_phones(self):
        def phone(identifier):
            return {
                "identifier": identifier,
                "hardwareProperties": {"deviceType": "iPhone"},
                "connectionProperties": {"tunnelState": "connected"},
            }

        self.assertIsNone(self.discover({"result": {"devices": []}}))
        self.assertEqual(self.discover({"result": {"devices": [phone("one")]}}), "one")
        with self.assertRaisesRegex(SystemExit, "Multiple iPhones.*--device"):
            self.discover({"result": {"devices": [phone("one"), phone("two")]}})

    def test_failed_or_malformed_discovery_has_a_diagnostic(self):
        with self.assertRaisesRegex(SystemExit, "devicectl list devices failed"):
            self.discover("", code=1)
        for payload in (
            "",
            {},
            {"result": {"devices": None}},
            {"result": {"devices": [1]}},
        ):
            with (
                self.subTest(payload=payload),
                self.assertRaisesRegex(SystemExit, "readable device list"),
            ):
                self.discover(payload)
        for error, text in (
            (subprocess.TimeoutExpired("fixture", 60), "timed out"),
            (FileNotFoundError("fixture"), "Could not run"),
        ):
            with patch.object(install.subprocess, "run", side_effect=error):
                with self.assertRaisesRegex(SystemExit, text):
                    install.reachable_phone()


if __name__ == "__main__":
    unittest.main()
