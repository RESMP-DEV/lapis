"""The token counter lapis runs on other machines: the desktop ledger's rules."""

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "apps" / "desktop" / "src" / "count_tokens.py"
sys.path.insert(0, str(SCRIPT.parent))
sys.dont_write_bytecode = True  # no __pycache__ beside the app's sources
import count_tokens  # noqa: E402

NOW = "2026-09-24T18:00:00.000Z"
HOUR = "2026-09-24T18"
EARLIER = "2026-09-23T18:00:00.000Z"


def codex_total(when, total, last=None):
    def usage(values):
        return {
            "input_tokens": values[0],
            "cached_input_tokens": values[1],
            "cache_write_input_tokens": 0,
            "output_tokens": values[2],
        }

    line = {
        "timestamp": when,
        "type": "event_msg",
        "payload": {
            "type": "token_count",
            "info": {
                "total_token_usage": usage(total),
                "last_token_usage": usage(last or total),
            },
        },
    }
    return json.dumps(line, separators=(",", ":")) + "\n"


def codex_model(when, model):
    line = {
        "timestamp": when,
        "type": "turn_context",
        "payload": {"cwd": "/w", "model": model},
    }
    return json.dumps(line, separators=(",", ":")) + "\n"


def claude_line(when, ident, request, model, values):
    line = {
        "parentUuid": "p",
        "message": {
            "model": model,
            "id": ident,
            "type": "message",
            "role": "assistant",
            "content": [{"type": "text", "text": "ok"}],
            "usage": {
                "input_tokens": values[0],
                "cache_creation_input_tokens": values[1],
                "cache_read_input_tokens": values[2],
                "output_tokens": values[3],
            },
        },
        "requestId": request,
        "type": "assistant",
        "timestamp": when,
    }
    return json.dumps(line, separators=(",", ":")) + "\n"


class CountTokensTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory()
        self.addCleanup(self.home.cleanup)
        self.codex = Path(self.home.name, ".codex", "sessions")
        self.claude = Path(self.home.name, ".claude", "projects")

    def write(self, path, text, append=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a" if append else "w", encoding="utf-8") as target:
            target.write(text)

    def test_counts_like_the_desktop_ledger(self):
        # Running totals become each call's use, by the turn's model; a
        # repeated report adds nothing; a fork counts from its own calls.
        self.write(
            self.codex / "2026/09/24/rollout-a.jsonl",
            codex_model(NOW, "gpt-6-astra")
            + codex_total(NOW, (1000, 600, 50))
            + codex_total(NOW, (1000, 600, 50))
            + codex_total(NOW, (2500, 1600, 120))
            + codex_model(NOW, "gpt-6-sol")
            + codex_total(NOW, (4000, 3000, 200)),
        )
        self.write(
            self.codex / "2026/09/24/rollout-fork.jsonl",
            codex_model(NOW, "gpt-6-luna")
            + codex_total(NOW, (900000, 800000, 9000), last=(50, 0, 5))
            + codex_total(NOW, (900100, 800000, 9010)),
        )
        # A message's lines count once at their largest; a resumed session
        # repeats a message; a line still being written waits.
        partial = claude_line(NOW, "msg_3", "req_3", "claude-fable-5-1", (3, 0, 0, 3))
        self.write(
            self.claude / "p/s1.jsonl",
            claude_line(NOW, "msg_1", "req_1", "claude-opus-5-5", (10, 100, 1000, 1))
            + claude_line(NOW, "msg_1", "req_1", "claude-opus-5-5", (10, 100, 1000, 5))
            + claude_line(NOW, "x", "y", "<synthetic>", (7, 0, 0, 7))
            + claude_line(EARLIER, "msg_0", "req_0", "claude-fable-5-1", (1, 0, 0, 1))
            + partial[:60],
        )
        self.write(
            self.claude / "p/s2.jsonl",
            claude_line(NOW, "msg_1", "req_1", "claude-opus-5-5", (10, 100, 1000, 5)),
        )
        since = time.mktime(time.strptime("2026-09-24", "%Y-%m-%d")) - 86400 * 2
        totals = count_tokens.count(since, str(self.codex), str(self.claude))
        self.assertEqual(totals["codex"]["gpt-6-astra"][HOUR], [900, 1600, 0, 120])
        self.assertEqual(totals["codex"]["gpt-6-sol"][HOUR], [100, 1400, 0, 80])
        self.assertEqual(totals["codex"]["gpt-6-luna"][HOUR], [150, 0, 0, 15])
        self.assertEqual(totals["claude"]["claude-opus-5-5"][HOUR], [10, 1000, 100, 5])
        self.assertEqual(
            totals["claude"]["claude-fable-5-1"]["2026-09-23T18"], [1, 0, 0, 1]
        )
        self.assertNotIn("<synthetic>", totals["claude"])

    def test_leaves_out_what_is_older_than_the_window(self):
        self.write(self.codex / "rollout-old.jsonl", codex_total(EARLIER, (10, 0, 1)))
        old = time.time() - 90 * 86400
        os.utime(self.codex / "rollout-old.jsonl", (old, old))
        self.write(
            self.claude / "p/s.jsonl",
            claude_line(EARLIER, "m", "r", "claude-opus-5-5", (1, 0, 0, 1)),
        )
        since = time.mktime(time.strptime("2026-09-24", "%Y-%m-%d"))
        totals = count_tokens.count(since, str(self.codex), str(self.claude))
        self.assertEqual(totals, {"codex": {}, "claude": {}})

    def test_runs_as_lapis_sends_it(self):
        # `python3 - SINCE` with the file on standard input, homes from the
        # environment.
        self.write(
            self.codex / "rollout-r.jsonl",
            codex_model(NOW, "m") + codex_total(NOW, (5000, 4000, 100)),
        )
        since = time.mktime(time.strptime("2026-09-20", "%Y-%m-%d"))
        output = subprocess.run(
            [sys.executable, "-", str(since)],
            input=SCRIPT.read_bytes(),
            capture_output=True,
            check=True,
            env={
                **os.environ,
                "HOME": self.home.name,
                "CODEX_HOME": "",
                "CLAUDE_CONFIG_DIR": "",
            },
        ).stdout
        self.assertEqual(
            json.loads(output),
            {"codex": {"m": {HOUR: [1000, 4000, 0, 100]}}, "claude": {}},
        )


if __name__ == "__main__":
    unittest.main()
