"""Tests for scripts/lapis_accounts.py: config merging and token hiding."""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

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


class SignInTest(unittest.TestCase):
    def test_validation_precedes_auth_and_destinations_are_per_plan(self):
        config = {
            "usage": {"machines": ["usage-host"]},
            "accounts": {
                "claude": [
                    None,
                    {"name": None},
                    {"name": "../outside", "email": "a@example.test"},
                    {"name": "missing-email", "email": None},
                    {
                        "name": "first",
                        "email": "a@example.test",
                        "home": "first-host",
                        "machines": None,
                    },
                    {
                        "name": "second",
                        "email": "b@example.test",
                        "machines": ["second-host"],
                    },
                ],
                "codex": [{"name": "unrelated", "machines": ["codex-only"]}],
            },
        }
        with (
            tempfile.TemporaryDirectory() as folder,
            patch.object(accounts, "ACCOUNTS", Path(folder)),
            patch.object(accounts, "command_add_claude") as add,
        ):
            accounts.command_sign_in(config, ask=lambda _: "")
            self.assertEqual(
                [(call.args[1], call.args[3]) for call in add.call_args_list],
                [
                    ("first", ["usage-host", "first-host"]),
                    ("second", ["usage-host", "second-host"]),
                ],
            )
            add.reset_mock()
            accounts.command_sign_in(config, [], ask=lambda _: "")
            self.assertEqual([call.args[3] for call in add.call_args_list], [[], []])
        with (
            patch.object(accounts, "load_config", return_value=config),
            patch.object(accounts, "command_sign_in") as sign_in,
        ):
            self.assertEqual(accounts.main(["sign-in"]), 0)
            self.assertIsNone(sign_in.call_args.args[1])
            self.assertEqual(accounts.main(["sign-in", "--to"]), 0)
            self.assertEqual(sign_in.call_args.args[1], [])

    def test_every_plan_without_a_token_is_offered_in_turn(self):
        config = {
            "usage": {"machines": ["devbox"]},
            "accounts": {
                "claude": [
                    {"name": "first", "email": "a@example.com", "home": "local"},
                    {"name": "second", "email": "b@example.com", "home": "gpubox"},
                    {"name": "third", "email": "c@example.com", "home": "gpubox"},
                ],
                "codex": [
                    {"name": "x", "email": "x@example.com", "machines": ["spare"]}
                ],
            },
        }
        self.assertEqual(
            accounts.plan_machines(config, config["accounts"]["claude"][1]),
            ["devbox", "gpubox"],
        )
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "claude").mkdir()
            (root / "claude" / "first.token").write_text("kept\n")
            added, asked = [], []

            def ask(prompt):
                asked.append(prompt)
                return "n" if "second" in prompt else ""

            with (
                patch.object(accounts, "ACCOUNTS", root),
                patch.object(
                    accounts,
                    "command_add_claude",
                    lambda config, name, email, hosts: added.append(
                        (name, email, hosts)
                    ),
                ),
            ):
                accounts.command_sign_in(config, ["gpubox"], ask)
        self.assertEqual(len(asked), 2)  # first already has a token
        self.assertEqual(added, [("third", "c@example.com", ["gpubox"])])


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


class SetupFailureTest(unittest.TestCase):
    def test_exec_and_child_failures_keep_their_cause(self):
        code = (
            "import importlib.util, sys; "
            f"s=importlib.util.spec_from_file_location('a', {str(Path(accounts.__file__))!r}); "
            "a=importlib.util.module_from_spec(s); s.loader.exec_module(a); "
            "\ntry: a.read_setup_token()\n"
            "except a.Failure as e: print(str(e), file=sys.stderr); sys.exit(1)\n"
        )
        cases = (
            (None, "could not start claude setup-token"),
            ("#!/missing-interpreter\n", "No such file or directory"),
            ("#!/bin/sh\nexit 3\n", "exited with status 3"),
        )
        for script, diagnostic in cases:
            with self.subTest(script=script), tempfile.TemporaryDirectory() as folder:
                if script is not None:
                    program = Path(folder) / "claude"
                    program.write_text(script)
                    program.chmod(0o700)
                result = subprocess.run(
                    [sys.executable, "-c", code],
                    env={**os.environ, "PATH": folder},
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(diagnostic, result.stderr)
                self.assertNotIn("Traceback", result.stdout + result.stderr)
                self.assertNotIn("printed no token", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
