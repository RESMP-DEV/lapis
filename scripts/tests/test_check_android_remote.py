"""Unit tests for the Android remote harness's device-free verdict logic."""

import importlib.util
import sys
import unittest
from pathlib import Path

_MODULE_PATH = (
    Path(__file__).resolve().parents[2] / "scripts" / "check_android_remote.py"
)
_spec = importlib.util.spec_from_file_location("check_android_remote", _MODULE_PATH)
check_android_remote = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = check_android_remote
_spec.loader.exec_module(check_android_remote)


class SettingsRestoreVerdictTest(unittest.TestCase):
    def test_clean_pass_without_retry_is_uneventful(self):
        self.assertEqual(
            check_android_remote.settings_restore_verdict(None, None, None),
            (None, None),
        )

    def test_clean_pass_with_retry_adds_receipt_note(self):
        verdict, note = check_android_remote.settings_restore_verdict(
            None, None, "DeviceError: dump timed out"
        )
        self.assertIsNone(verdict)
        self.assertIn("first attempt raised DeviceError: dump timed out", note)
        self.assertIn("settle-and-retry then restored the switch", note)

    def test_probe_failure_with_retry_stays_skip_and_names_retry(self):
        verdict, note = check_android_remote.settings_restore_verdict(
            None,
            check_android_remote.SETTINGS_SWITCH_NEVER_APPEARED,
            "DeviceError: dump timed out",
        )
        self.assertIsNone(note)
        self.assertTrue(verdict.startswith("SKIPPED: "))
        self.assertIn("after a first restore attempt that raised DeviceError", verdict)

    def test_effect_failure_with_retry_fails_and_combines_problems(self):
        verdict, note = check_android_remote.settings_restore_verdict(
            "the stage did not open",
            "the settings switch did not flip after the tap",
            "DeviceError: dump timed out",
        )
        self.assertIsNone(note)
        self.assertIn("the stage did not open; also,", verdict)
        self.assertIn("the settings-toggle restore did not confirm:", verdict)
        self.assertIn("after a first restore attempt that raised DeviceError", verdict)


if __name__ == "__main__":
    unittest.main()
