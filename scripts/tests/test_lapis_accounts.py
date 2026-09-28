"""Tests for scripts/lapis_accounts.py: config merging and token hiding."""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "lapis_accounts", Path(__file__).resolve().parents[1] / "lapis_accounts.py"
)
accounts = importlib.util.module_from_spec(SPEC)
sys.modules["lapis_accounts"] = accounts
SPEC.loader.exec_module(accounts)

TOKEN = b"sk-ant-oat01-" + b"A" * 90 + b"-bcdefAA"


class PlanTest(unittest.TestCase):
    def test_names_come_from_the_email(self):
        self.assertEqual(accounts.plan_name("someone@gmail.com"), "someone-gmail")
        self.assertEqual(accounts.plan_name("a.b+c@example.co.uk"), "a.b-c-example")

    def test_merge_adds_then_updates_by_email(self):
        config = {}
        accounts.merge_plan(config, "claude", "mac", "Me@Example.com", "local", [])
        accounts.merge_plan(
            config, "claude", "other-name", "me@example.com", "devbox", ["buildbox"]
        )
        plans = config["accounts"]["claude"]
        self.assertEqual(len(plans), 1)
        self.assertEqual(plans[0]["home"], "local", "a plan's home is not moved")
        self.assertEqual(plans[0]["machines"], ["buildbox"])
        self.assertEqual(plans[0]["email"], "me@example.com")

    def test_machines_are_this_mac_and_the_usage_hosts(self):
        config = {"usage": {"machines": ["devbox", "-oProxyCommand=x", "buildbox"]}}
        self.assertEqual(accounts.machines(config), ["local", "devbox", "buildbox"])

    def test_config_is_replaced_whole_and_keeps_other_keys(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "lapis.json"
            path.write_text('{"theme": "night"}')
            config = accounts.load_config(path)
            accounts.merge_plan(
                config, "codex", "one", "one@example.com", None, ["local"]
            )
            accounts.save_config(config, path)
            saved = accounts.load_config(path)
            self.assertEqual(saved["theme"], "night")
            self.assertEqual(saved["accounts"]["codex"][0]["machines"], ["local"])


class TokenTest(unittest.TestCase):
    def test_a_token_is_never_shown(self):
        show, keep = accounts.masked_output(
            b"Your token: " + TOKEN + b"\r\nStore it.\r\n"
        )
        self.assertNotIn(TOKEN, show)
        self.assertIn(b"[kept by lapis]", show)
        self.assertEqual(keep, b"")

    def test_a_token_split_across_reads_is_held_until_its_line_ends(self):
        show, keep = accounts.masked_output(b"Your token: " + TOKEN[:30])
        self.assertEqual(show, b"Your token: ")
        show, keep = accounts.masked_output(keep + TOKEN[30:] + b"\r\n")
        self.assertNotIn(TOKEN[20:], show)
        self.assertEqual(keep, b"")

    def test_a_possible_start_is_held_and_other_text_is_not(self):
        show, keep = accounts.masked_output(b"Paste the code > ")
        self.assertEqual(show, b"Paste the code > ")
        show, keep = accounts.masked_output(b"done. sk-an")
        self.assertEqual((show, keep), (b"done. ", b"sk-an"))


if __name__ == "__main__":
    unittest.main()
