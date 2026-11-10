import base64
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


def load_relay():
    path = (
        Path(__file__).parents[2]
        / "services"
        / "session"
        / "src"
        / "terminal_hook_relay.py"
    )
    spec = importlib.util.spec_from_file_location("lapis_terminal_hook_relay", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


relay = load_relay()


class RemoteHookRelayTests(unittest.TestCase):
    def test_background_values_remain_json_objects(self):
        self.assertEqual(
            relay.work("background_tasks", [{"status": "running"}, 0]),
            [{"status": "running"}, {}],
        )
        self.assertEqual(relay.work("session_crons", [{"id": "cron"}, 0]), [{}, {}])
        self.assertEqual(relay.work("background_tasks", 0), {})
        self.assertEqual(relay.work("session_crons", [0] * 65), {})

    def test_invalid_background_lists_remain_schema_drift(self):
        source = {
            "hook_event_name": "Stop",
            "session_id": "session",
            "background_tasks": 0,
            "session_crons": [{}] * 65,
        }
        frames = []

        def capture_write(descriptor, view):
            frames.append(bytes(view))
            return len(view)

        with patch.object(os, "write", side_effect=capture_write):
            with patch.dict(
                os.environ,
                {
                    "LAPIS_HOOK_NONCE": "0123456789abcdef" * 2,
                    "LAPIS_HOOK_TTY": "/dev/null",
                },
            ):
                relay.relay("claude", json.dumps(source))

        payload = frames[0].split(b";", 3)[3].rstrip(b"\x07")
        event = json.loads(base64.b64decode(payload))
        self.assertEqual(event["background_tasks"], {})
        self.assertEqual(event["session_crons"], {})

    def test_largest_legitimate_frame_fits_the_terminal_hook_limit(self):
        source = {key: "x" * 256 for key in relay.K}
        source["background_tasks"] = [{"status": "running"}] * 64
        source["session_crons"] = [{"id": "private", "schedule": "* * * * *"}] * 64
        frames = []

        def capture_write(descriptor, view):
            frames.append(bytes(view))
            return len(view)

        with patch.object(os, "write", side_effect=capture_write):
            with patch.dict(
                os.environ,
                {
                    "LAPIS_HOOK_NONCE": "0123456789abcdef" * 2,
                    "LAPIS_HOOK_TTY": "/dev/null",
                },
            ):
                relay.relay("claude", json.dumps(source))
        self.assertEqual(len(frames), 1)
        self.assertLessEqual(len(frames[0]), 24 * 1024)

    def test_nonce_requires_exactly_32_lowercase_hex_digits(self):
        self.assertTrue(relay.valid_nonce("0123456789abcdef" * 2))
        self.assertFalse(relay.valid_nonce("0123456789ABCDEF" * 2))
        self.assertFalse(relay.valid_nonce("0123456789abcdef"))
        self.assertFalse(relay.valid_nonce("0123456789abcdefg0123456789abcdef"))

    def test_tty_must_be_a_real_character_device(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "not-a-tty"
            path.write_text("", encoding="utf-8")
            with patch.object(os, "write") as write:
                with patch.dict(
                    os.environ,
                    {
                        "LAPIS_HOOK_NONCE": "0123456789abcdef" * 2,
                        "LAPIS_HOOK_TTY": str(path),
                    },
                ):
                    relay.relay("codex", json.dumps({"type": "agent-turn-complete"}))
            write.assert_not_called()

    def test_frame_is_retried_after_a_partial_write(self):
        sizes = []
        returned = []

        def short_then_complete(descriptor, view):
            sizes.append(len(view))
            count = len(view) - 1 if len(sizes) == 1 else len(view)
            returned.append(count)
            return count

        with patch.object(os, "write", side_effect=short_then_complete) as write:
            with patch.dict(
                os.environ,
                {
                    "LAPIS_HOOK_NONCE": "0123456789abcdef" * 2,
                    "LAPIS_HOOK_TTY": "/dev/null",
                },
            ):
                relay.relay("codex", "{}")
        self.assertEqual(write.call_count, 2)
        self.assertEqual(returned[0], sizes[0] - 1)
        self.assertEqual(sizes[1], sizes[0] - returned[0])
        self.assertLess(sizes[1], sizes[0])

    def test_notify_thread_identity_crosses_the_terminal(self):
        frames = []

        def capture_write(descriptor, view):
            frames.append(bytes(view))
            return len(view)

        source = {
            "type": "agent-turn-complete",
            "thread-id": "019a0000-0000-7000-8000-000000000001",
            "turn-id": "t1",
            "input-messages": ["private prompt"],
            "last-assistant-message": "private reply",
        }
        with patch.object(os, "write", side_effect=capture_write):
            with patch.dict(
                os.environ,
                {
                    "LAPIS_HOOK_NONCE": "0123456789abcdef" * 2,
                    "LAPIS_HOOK_TTY": "/dev/null",
                },
            ):
                relay.relay("codex", json.dumps(source))
        payload = frames[0].split(b";", 3)[3].rstrip(b"\x07")
        self.assertEqual(
            json.loads(base64.b64decode(payload)),
            {
                "type": "agent-turn-complete",
                "thread-id": "019a0000-0000-7000-8000-000000000001",
                "turn-id": "t1",
            },
        )

    def test_top_level_notify_without_tomllib(self):
        parse = relay.top_level_notify
        self.assertEqual(
            parse('model = "x"\nnotify = ["notify-send", "-a", \'lapis\']\n'),
            ["notify-send", "-a", "lapis"],
        )
        self.assertEqual(
            parse('notify = [\n  "sh", # the shell\n  "-c",\n  "echo \\"hi\\"",\n]\n'),
            ["sh", "-c", 'echo "hi"'],
        )
        self.assertEqual(
            parse('notify = [\n  "sh",\n  "-c"\n]\n'),
            ["sh", "-c"],
        )
        self.assertEqual(
            parse('notify = [\n  "sh",\n  "-c" # flag\n]\n'),
            ["sh", "-c"],
        )
        self.assertIsNone(parse('notify = [\n  "sh"\n  "-c"\n]\n'))
        self.assertEqual(parse("notify = []\n"), [])
        self.assertEqual(parse('"notify" = ["quoted-key"]\n'), ["quoted-key"])
        self.assertEqual(parse("'notify' = ['literal-key']\n"), ["literal-key"])
        self.assertEqual(
            parse('matrix = [\n  [1, 2],\n]\nnotify = ["after-a-nested-array"]\n'),
            ["after-a-nested-array"],
        )
        self.assertEqual(
            parse('text = """\nnot [a-table]\n"""\nnotify = ["after-a-string"]\n'),
            ["after-a-string"],
        )
        self.assertEqual(
            parse('notify = ["\\U0001F600 notify"]\n'),
            ["😀 notify"],
        )
        self.assertIsNone(parse('notify = ["invalid\\xescape"]\n'))
        self.assertIsNone(parse('[profiles.x]\nnotify = ["inside-a-table"]\n'))
        self.assertIsNone(parse('notify = "not-an-array"\n'))
        self.assertIsNone(parse('notify = ["unterminated"\n'))
        self.assertIsNone(parse('model = "x"\n'))

    def test_chain_runs_the_users_notify_without_tomllib(self):
        with tempfile.TemporaryDirectory() as home:
            Path(home, "config.toml").write_text(
                'notify = ["user-notify", "--flag"]\n[tui]\nnotify = ["no"]\n',
                encoding="utf-8",
            )
            real_import = __import__

            def no_tomllib(name, *args, **kwargs):
                if name == "tomllib":
                    raise ImportError(name)
                return real_import(name, *args, **kwargs)

            with patch.dict(os.environ, {"CODEX_HOME": home}):
                with patch("builtins.__import__", side_effect=no_tomllib):
                    with patch.object(os, "execvp") as execvp:
                        relay.chain('{"type":"agent-turn-complete"}')
        execvp.assert_called_once_with(
            "user-notify",
            ["user-notify", "--flag", '{"type":"agent-turn-complete"}'],
        )

    def test_effective_notify_layers_the_selected_profile(self):
        with tempfile.TemporaryDirectory() as home:
            Path(home, "config.toml").write_text(
                'notify = ["base-notify"]\n', encoding="utf-8"
            )
            Path(home, "work.config.toml").write_text(
                'notify = ["work-notify"]\n', encoding="utf-8"
            )
            with patch.dict(
                os.environ, {"CODEX_HOME": home, "LAPIS_CODEX_PROFILE": "work"}
            ):
                self.assertEqual(
                    relay.effective_notify(home),
                    ["work-notify"],
                )

            Path(home, "work.config.toml").unlink()
            with patch.dict(
                os.environ, {"CODEX_HOME": home, "LAPIS_CODEX_PROFILE": "work"}
            ):
                self.assertEqual(
                    relay.effective_notify(home),
                    ["base-notify"],
                )

            with patch.dict(
                os.environ, {"CODEX_HOME": home, "LAPIS_CODEX_PROFILE": "../evil"}
            ):
                self.assertEqual(
                    relay.effective_notify(home),
                    ["base-notify"],
                )

    def test_effective_notify_allows_a_profile_only_codex_home(self):
        with tempfile.TemporaryDirectory() as home:
            Path(home, "work.config.toml").write_text(
                'notify = ["profile-notify", "--flag"]\n', encoding="utf-8"
            )
            with patch.dict(
                os.environ, {"CODEX_HOME": home, "LAPIS_CODEX_PROFILE": "work"}
            ):
                self.assertEqual(
                    relay.effective_notify(home),
                    ["profile-notify", "--flag"],
                )
                with patch.object(os, "execvp") as execvp:
                    relay.chain('{"type":"agent-turn-complete"}')
        execvp.assert_called_once_with(
            "profile-notify",
            ["profile-notify", "--flag", '{"type":"agent-turn-complete"}'],
        )


if __name__ == "__main__":
    unittest.main()
