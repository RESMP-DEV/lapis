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
        bounds="[16,200][892,280]" clickable="true" />
  <node index="2" text="" resource-id="terminal" class="android.view.View"
        package="dev.lapis.remote"
        content-desc="&#8250; ping from phone&#10;echo: ping from phone"
        bounds="[0,120][908,2000]" clickable="false" />
  <node index="3" text="Settings" resource-id="settings"
        class="android.widget.TextView" package="dev.lapis.remote"
        content-desc="" bounds="[700,80][860,140]" clickable="false" />
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


if __name__ == "__main__":
    unittest.main()
