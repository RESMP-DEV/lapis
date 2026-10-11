"""Regressions for the Codex P2 probe's sanitized evidence boundary."""

import sys
import unittest
import json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from probe_codex_worktrees import parse_args, sanitized_events
from check_codex_attention import probe_configuration_arguments


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


class ArgumentTests(unittest.TestCase):
    def test_model_endpoint_is_explicit_and_reaches_provider_configuration(self):
        args = parse_args(["--model-endpoint", "http://127.0.0.1:3900/v1"])
        self.assertEqual(args.model_endpoint, "http://127.0.0.1:3900/v1")
        arguments = probe_configuration_arguments(args.model_endpoint)
        self.assertIn(
            "model_providers.lapis_probe.base_url=" + json.dumps(args.model_endpoint),
            arguments,
        )


if __name__ == "__main__":
    unittest.main()
