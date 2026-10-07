import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


def load_relay():
    path = (
        Path(__file__).parents[2] / "apps" / "desktop" / "src" / "remote_hook_relay.py"
    )
    spec = importlib.util.spec_from_file_location("lapis_remote_hook_relay", path)
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
        self.assertEqual(
            relay.work("session_crons", [{"id": "cron"}, 0]), [{"id": "cron"}, {}]
        )
        self.assertEqual(relay.work("background_tasks", 0), [])
        self.assertEqual(relay.work("session_crons", [0] * 65), [])

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


if __name__ == "__main__":
    unittest.main()
