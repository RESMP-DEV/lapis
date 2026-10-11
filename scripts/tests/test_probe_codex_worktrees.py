"""Regressions for the Codex P2 probe's sanitized evidence boundary."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from probe_codex_worktrees import sanitized_events


class SanitizedEventTests(unittest.TestCase):
    def test_thread_identities_are_stable_and_private_values_removed(self):
        events = [
            {
                "method": "thread/started",
                "thread": "private-original",
                "ephemeral": False,
            },
            {
                "method": "thread/started",
                "thread": "private-second",
                "ephemeral": False,
            },
            {
                "method": "turn/completed",
                "thread": "private-second",
                "status": "completed",
            },
            {
                "method": "thread/started",
                "thread": "private-temporary",
                "ephemeral": True,
            },
        ]
        self.assertEqual(
            sanitized_events(events, "private-original", "private-second"),
            [
                {"method": "thread/started", "thread": "original", "ephemeral": False},
                {"method": "thread/started", "thread": "second", "ephemeral": False},
                {
                    "method": "turn/completed",
                    "thread": "second",
                    "status": "completed",
                },
                {"method": "thread/started", "thread": "other", "ephemeral": True},
            ],
        )

    def test_event_accounting_is_bounded(self):
        events = [
            {"method": "thread/status/changed", "thread": "private-original"}
            for _ in range(300)
        ]
        self.assertEqual(
            len(sanitized_events(events, "private-original", "private-second")),
            256,
        )


if __name__ == "__main__":
    unittest.main()
