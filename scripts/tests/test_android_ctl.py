"""Unit tests for tools/qa/android_ctl.py's device-free logic: input-text
escaping, dump parsing, selectors, bounds math, keycodes."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "qa"))

import android_ctl  # noqa: E402

DUMP = """<?xml version='1.0' encoding='UTF-8'?>
<hierarchy rotation="0">
  <node index="0" text="" resource-id="" class="android.widget.FrameLayout"
        package="dev.lapis.remote" content-desc="" bounds="[0,0][908,2316]"
        clickable="false" />
  <node index="1" text="echo agent" resource-id="agent-echo agent"
        class="android.view.View" package="dev.lapis.remote" content-desc=""
        bounds="[16,200][892,280]" clickable="true" checked="true" />
  <node index="2" text="" resource-id="terminal" class="android.view.View"
        package="dev.lapis.remote"
        content-desc="&#8250; ping from phone&#10;echo: ping from phone"
        bounds="[0,120][908,2000]" clickable="false" />
  <node index="3" text="Settings" resource-id="settings"
        class="android.widget.TextView" package="dev.lapis.remote"
        content-desc="" bounds="[700,80][860,140]" clickable="false"
        checked="false" />
  <node index="4" text="echo agent" resource-id="" class="android.widget.TextView"
        package="dev.lapis.remote" content-desc="" bounds="[80,220][700,260]"
        clickable="false" />
</hierarchy>
"""


class EscapeTest(unittest.TestCase):
    def test_spaces_become_percent_s(self):
        self.assertEqual(android_ctl.escape_input_text("hello world"), "'hello%sworld'")

    def test_single_quotes_survive_the_device_shell(self):
        self.assertEqual(
            android_ctl.escape_input_text("it's here"),
            r"'it'\''s%shere'",
        )

    def test_plain_text_is_quoted(self):
        self.assertEqual(android_ctl.escape_input_text("count"), "'count'")

    def test_shell_quote_quotes_everything_once(self):
        self.assertEqual(android_ctl.shell_quote("a b&c"), "'a b&c'")


class DumpTest(unittest.TestCase):
    def setUp(self):
        self.nodes = android_ctl.parse_dump(DUMP)

    def test_nodes_carry_only_present_strings(self):
        by_id = {node.get("id"): node for node in self.nodes}
        self.assertEqual(
            by_id["terminal"]["desc"], "› ping from phone\necho: ping from phone"
        )
        self.assertNotIn("desc", by_id["settings"])
        self.assertEqual(by_id["settings"]["text"], "Settings")
        self.assertTrue(by_id["agent-echo agent"]["clickable"])
        self.assertTrue(by_id["agent-echo agent"]["checked"])
        self.assertFalse(by_id["settings"]["checked"])
        self.assertNotIn("checked", by_id["terminal"])

    def test_select_by_exact_id_text_and_desc(self):
        self.assertEqual(len(android_ctl.select(self.nodes, id="terminal")), 1)
        self.assertEqual(len(android_ctl.select(self.nodes, text="Settings")), 1)
        self.assertEqual(len(android_ctl.select(self.nodes, desc="Settings")), 0)

    def test_select_contains_matches_any_attribute(self):
        matches = android_ctl.select(self.nodes, contains="echo agent")
        self.assertEqual(len(matches), 2)  # the resource-id and the visible text

    def test_select_requires_every_given_selector(self):
        self.assertEqual(
            len(android_ctl.select(self.nodes, id="settings", text="Settings")), 1
        )
        self.assertEqual(
            len(android_ctl.select(self.nodes, id="settings", text="Else")), 0
        )

    def test_bad_bounds_are_dropped(self):
        nodes = android_ctl.parse_dump(
            "<hierarchy><node bounds='nonsense' /></hierarchy>"
        )
        self.assertEqual(nodes, [])


class BoundsTest(unittest.TestCase):
    def test_parse_and_center(self):
        bounds = android_ctl.parse_bounds("[16,200][892,280]")
        self.assertEqual(bounds, (16, 200, 892, 280))
        self.assertEqual(android_ctl.bounds_center(bounds), (454, 240))

    def test_parse_rejects_malformed(self):
        self.assertIsNone(android_ctl.parse_bounds(None))
        self.assertIsNone(android_ctl.parse_bounds("[1,2][3"))


class KeycodeTest(unittest.TestCase):
    def test_names_map_and_unknown_pass_through(self):
        self.assertEqual(android_ctl.keycode("enter"), "66")
        self.assertEqual(android_ctl.keycode("BACK"), "4")
        self.assertEqual(android_ctl.keycode("KEYCODE_DPAD_UP"), "KEYCODE_DPAD_UP")
        self.assertEqual(android_ctl.keycode("187"), "187")


def canned_device(window_dump, activities_dump, ime_dump=None):
    """A Device whose shell() answers canned dumpsys output, so the
    lock/IME probes run against build-variant dumps without hardware.
    Missing dumps coerce to the empty string (a None reply would turn a
    later locked() call into a TypeError), and the stub keeps shell()'s
    real signature so production call sites may pass timeout=."""
    device = android_ctl.Device.__new__(android_ctl.Device)
    replies = {
        "dumpsys window": window_dump or "",
        "dumpsys activity activities": activities_dump or "",
    }
    if ime_dump is not None:
        replies["dumpsys input_method"] = ime_dump
    device.shell = lambda command, timeout=30: replies.get(command, "")
    return device


class LockedTest(unittest.TestCase):
    # Rounds 7-9 made locked() a chain of build-specific string probes;
    # these cases pin each signal shape the parser is known to meet.
    def test_keyguard_flag_alone_means_locked(self):
        device = canned_device("isKeyguardShowing=true", "irrelevant")
        self.assertTrue(device.locked())

    def test_dream_flag_alone_means_locked(self):
        device = canned_device("mDreamingLockscreen=true", "irrelevant")
        self.assertTrue(device.locked())

    def test_aosp_resumed_marker_catches_camel_case_dialer(self):
        device = canned_device(
            "isKeyguardShowing=false mDreamingLockscreen=false",
            "  topResumedActivity=ActivityRecord{1f u0 "
            "com.android.phone/.EmergencyCallActivity t9}",
        )
        self.assertTrue(device.locked())

    def test_one_ui_resumed_lines_catch_the_dialer(self):
        device = canned_device(
            "isKeyguardShowing=false",
            "  Resumed activities in task display areas (from top to bottom):\n"
            "    Resumed: ActivityRecord{565 u0 com.sec.android.app."
            "emergencydialer/emergencydialer.view.EmergencyDialerActivity t7}\n"
            "  ResumedActivity: ActivityRecord{565 u0 com.sec.android.app."
            "emergencydialer/emergencydialer.view.EmergencyDialerActivity t7}",
        )
        self.assertTrue(device.locked())

    def test_emergency_text_outside_resumed_lines_reads_unlocked(self):
        # The 300-char slice after a bare marker substring used to spill
        # into per-task sections; a line-anchored match must not.
        device = canned_device(
            "isKeyguardShowing=false mDreamingLockscreen=false",
            "  topResumedActivity=ActivityRecord{2 u0 dev.lapis.remote/.MainActivity t3}\n"
            "  ...permission android.permission.FOREGROUND_EMERGENCY unrelated",
        )
        self.assertFalse(device.locked())

    def test_systemui_keyguard_component_reads_locked(self):
        # The "keyguard" half of the keyword match catches a SystemUI
        # keyguard activity reported as resumed instead of a dialer.
        device = canned_device(
            "isKeyguardShowing=false",
            "  ResumedActivity: ActivityRecord{7 u0 com.android.systemui/"
            ".keyguard.ui.KeyguardService t2}",
        )
        self.assertTrue(device.locked())

    def test_unlocked_app_and_missing_fields_read_unlocked(self):
        device = canned_device(
            "isKeyguardShowing=false mDreamingLockscreen=false",
            "  topResumedActivity=ActivityRecord{2 u0 dev.lapis.remote/.MainActivity t3}",
        )
        self.assertFalse(device.locked())
        stripped = canned_device("some other output", "short")
        self.assertFalse(stripped.locked())


class ImeShownTest(unittest.TestCase):
    def test_shown_and_hidden_read_from_the_field(self):
        shown = canned_device(None, None, "mInputShown=true mInputView=null")
        self.assertTrue(shown.ime_shown())
        hidden = canned_device(None, None, "mInputShown=false")
        self.assertFalse(hidden.ime_shown())

    def test_missing_field_raises_skipped_not_device_error(self):
        # The runner classifies Skipped before DeviceError and uses the
        # bare message; the prefix contract is what routes the check into
        # the receipt's skipped bucket instead of its failed bucket.
        device = canned_device(None, None, "no ime state here")
        with self.assertRaises(android_ctl.Skipped) as raised:
            device.ime_shown()
        self.assertTrue(str(raised.exception).startswith("SKIPPED:"))
        self.assertIsInstance(raised.exception, android_ctl.DeviceError)


class LaunchExtrasTest(unittest.TestCase):
    def test_launch_forces_the_command_bar_on_by_default(self):
        # The harness must never depend on the stored setting: every launch
        # pins the bar on, so a missing command-bar node in a run is a
        # product regression rather than a runnable-state question.
        device = android_ctl.Device.__new__(android_ctl.Device)
        device.package = "dev.lapis.remote"
        sent: list[str] = []

        def record(command, timeout=30):
            sent.append(command)
            return ""

        device.shell = record
        device.launch(host="127.0.0.1:7351", font=12, fresh=True)
        self.assertEqual(len(sent), 2)  # fresh's force-stop, then the start
        self.assertIn("--es gatewayHost '127.0.0.1:7351'", sent[1])
        self.assertIn("--es terminalFontSize 12", sent[1])
        self.assertIn("--es commandBarEnabled true", sent[1])
        device.launch(command_bar=False)
        self.assertIn("--es commandBarEnabled false", sent[2])
        self.assertNotIn("gatewayHost", sent[2])
        device.launch(command_bar=None)
        self.assertNotIn("commandBarEnabled", sent[3])


if __name__ == "__main__":
    unittest.main()
