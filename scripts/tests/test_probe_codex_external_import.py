"""Focused regressions for the isolated external-agent import probe."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock

from scripts import probe_codex_external_import as probe


def session_item() -> dict[str, object]:
    return {
        "itemType": "SESSIONS",
        "description": "/private/source to /private/target",
        "cwd": None,
        "details": {"sessions": [{"path": "/private/source/session.jsonl"}]},
    }


class ProbeTests(unittest.IsolatedAsyncioTestCase):
    async def test_initialize_matches_live_schema_and_confirms_home_contract(self):
        client = AsyncMock()
        client.rpc.return_value = {
            "userAgent": "fixture",
            "codexHome": "/private/other",
            "platformFamily": "unix",
            "platformOs": "macos",
        }
        await probe.initialize(client)
        client.send.assert_awaited_once_with({"method": "initialized"})
        client.rpc.return_value = {"codexHome": "/private/other"}
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            await probe.initialize(client)

    async def test_session_selection_requires_exactly_one_item_and_session(self):
        self.assertEqual(probe.session_item([session_item()]), session_item())
        for items in ([], [session_item(), session_item()]):
            with self.assertRaisesRegex(RuntimeError, "exactly one session migration"):
                probe.session_item(items)
        malformed = {"itemType": "SESSIONS", "details": {"sessions": []}}
        with self.assertRaisesRegex(RuntimeError, "exactly one detected session"):
            probe.session_item([malformed])

    async def test_import_request_is_sessions_only(self):
        request = probe.import_item(session_item(), provider_id="fixture")
        self.assertEqual(
            [item["itemType"] for item in request["migrationItems"]], ["SESSIONS"]
        )
        self.assertEqual(request["providerId"], "fixture")
        self.assertEqual(request["migrationSource"], "claude")

    async def test_completion_identity_and_results_are_strict(self):
        completion = {
            "method": "externalAgentConfig/import/completed",
            "params": {
                "importId": "import",
                "itemTypeResults": [
                    {
                        "itemType": "SESSIONS",
                        "successes": [
                            {"title": probe.EXPECTED_TITLE, "target": "/private/thread"}
                        ],
                        "failures": [],
                    }
                ],
            },
        }
        self.assertEqual(
            probe.completed_result(completion, "import"),
            "/private/thread",
        )
        for mutation in (
            {"params": {"importId": "other"}},
            {"params": {"importId": "import", "itemTypeResults": []}},
            {
                "params": {
                    "importId": "import",
                    "itemTypeResults": [
                        {"itemType": "SESSIONS", "successes": [None], "failures": []}
                    ],
                }
            },
            {
                "params": {
                    "importId": "import",
                    "itemTypeResults": [
                        {
                            "itemType": "SESSIONS",
                            "successes": [{"title": "other"}],
                            "failures": [],
                        }
                    ],
                }
            },
        ):
            with self.assertRaises(RuntimeError):
                probe.completed_result({**completion, **mutation}, "import")

    async def test_thread_and_history_validation_counts_without_content(self):
        threads = {"data": [{"id": "thread", "title": probe.EXPECTED_TITLE}]}
        self.assertEqual(probe.imported_thread(threads, "thread"), "thread")
        history = {
            "data": [
                {
                    "importId": "import",
                    "providerId": "lapis-external-import-probe",
                    "completedAtMs": 1,
                }
            ]
        }
        self.assertEqual(probe.import_history(history, "import"), 1)
        with self.assertRaisesRegex(RuntimeError, "one persisted thread"):
            probe.imported_thread({"data": []}, "thread")
        with self.assertRaisesRegex(RuntimeError, "did not match"):
            probe.imported_thread(threads, "other")
        with self.assertRaisesRegex(RuntimeError, "one matching import"):
            probe.import_history({"data": []}, "import")

    async def test_fixture_is_confined_to_disposable_home(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            project = probe.write_fixture(home)
            self.assertTrue(project.is_relative_to(home))
            self.assertEqual(
                len(list((home / ".claude/projects/fixture").glob("*.jsonl"))), 1
            )

    async def test_dead_child_does_not_count_as_import_pass(self):
        receipt = probe.base_receipt()
        with self.assertRaisesRegex(RuntimeError, "exited before listening"):
            await probe.run_probe(Path("/usr/bin/false"), receipt)
        self.assertFalse(receipt["passed"])
        self.assertFalse(receipt["isolation"]["cleanup_complete"])


class ReceiptTests(unittest.TestCase):
    def test_failure_writes_content_free_receipt_and_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "failure.json"
            self.assertEqual(
                probe.main(["--codex", "/no-such-codex", "--output", str(output)]), 1
            )
            receipt = json.loads(output.read_text())
            self.assertFalse(receipt["passed"])
            self.assertEqual(
                receipt["error"], "Probe failed without recording source error text"
            )
            self.assertNotIn(str(Path(directory)), output.read_text())


if __name__ == "__main__":
    unittest.main()
