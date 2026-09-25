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


class BuildOnlyTests(unittest.TestCase):
    def test_build_only_never_discovers_network_or_devices(self):
        for host in (None, "gateway.example"):
            argv = ["install_ios_app.py", "--build-only"]
            if host:
                argv += ["--host", host]
            with (
                self.subTest(host=host),
                patch.object(install.sys, "argv", argv),
                patch.object(install, "build") as build,
                patch.object(install, "mac_host") as mac_host,
                patch.object(install, "reachable_phone") as phone,
            ):
                self.assertEqual(install.main(), 0)
                build.assert_called_once_with(host or "")
                mac_host.assert_not_called()
                phone.assert_not_called()


if __name__ == "__main__":
    unittest.main()
