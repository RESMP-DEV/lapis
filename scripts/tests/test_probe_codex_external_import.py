"""Focused regressions for the isolated external-agent import probe."""

import json
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
from unittest.mock import AsyncMock, patch

from scripts import probe_codex_external_import as probe


def session_item() -> dict[str, object]:
    home = Path("/private/source-home")
    return {
        "itemType": "SESSIONS",
        "description": "/private/source to /private/target",
        "cwd": None,
        "details": {
            "plugins": [],
            "skills": [],
            "sessions": [
                {
                    "path": str(home / ".claude/projects/source/session.jsonl"),
                    "cwd": str(home / "project"),
                }
            ],
            "mcpServers": [],
            "hooks": [],
            "subagents": [],
            "commands": [],
            "memory": [],
        },
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
        home = Path("/private/source-home")
        self.assertEqual(probe.session_item([session_item()], home), session_item())
        for malformed in (None, [None]):
            with self.assertRaisesRegex(RuntimeError, "Malformed Codex detection"):
                probe.session_item(malformed, home)
        for items in ([], [session_item(), session_item()]):
            with self.assertRaisesRegex(RuntimeError, "exactly one session migration"):
                probe.session_item(items, home)
        malformed = session_item()
        malformed["details"]["sessions"] = []
        with self.assertRaisesRegex(RuntimeError, "exactly one detected session"):
            probe.session_item([malformed], home)
        carrying_skills = session_item()
        carrying_skills["details"]["skills"] = [{"name": "other"}]
        with self.assertRaisesRegex(RuntimeError, "non-session migration"):
            probe.session_item([carrying_skills], home)
        unknown_detail = session_item()
        unknown_detail["details"]["future"] = []
        with self.assertRaisesRegex(RuntimeError, "unknown migration detail"):
            probe.session_item([unknown_detail], home)
        missing_detail = session_item()
        missing_detail["details"].pop("plugins")
        missing_detail["details"].pop("skills")
        with self.assertRaisesRegex(
            RuntimeError, r"missing migration details: plugins, skills"
        ):
            probe.session_item([missing_detail], home)
        unknown_and_missing = session_item()
        unknown_and_missing["details"].pop("plugins")
        unknown_and_missing["details"]["future"] = []
        with self.assertRaisesRegex(RuntimeError, "unknown migration detail"):
            probe.session_item([unknown_and_missing], home)
        memory_absent = session_item()
        memory_absent["details"].pop("memory")
        expected_memory_absent = deepcopy(memory_absent)
        self.assertEqual(
            probe.session_item([memory_absent], home), expected_memory_absent
        )
        nonempty_memory = session_item()
        nonempty_memory["details"]["memory"] = ["not-a-session"]
        with self.assertRaisesRegex(RuntimeError, "non-session migration class"):
            probe.session_item([nonempty_memory], home)
        escaping_path = session_item()
        escaping_path["details"]["sessions"][0]["path"] = "/private/other/session.jsonl"
        with self.assertRaisesRegex(RuntimeError, "escapes the disposable home"):
            probe.session_item([escaping_path], home)
        escaping_cwd = session_item()
        escaping_cwd["details"]["sessions"][0]["cwd"] = "/private/other/project"
        with self.assertRaisesRegex(RuntimeError, "escapes the disposable home"):
            probe.session_item([escaping_cwd], home)

    async def test_import_request_is_sessions_only(self):
        request = probe.import_item(session_item(), provider_id="fixture")
        self.assertEqual(
            [item["itemType"] for item in request["migrationItems"]], ["SESSIONS"]
        )
        self.assertEqual(request["providerId"], "fixture")
        self.assertEqual(
            probe.import_item(session_item())["providerId"], probe.PROVIDER_ID
        )
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
        history["data"].append({"importId": "other", "providerId": probe.PROVIDER_ID})
        self.assertEqual(probe.import_history(history, "import"), 1)
        with self.assertRaisesRegex(RuntimeError, "one persisted thread"):
            probe.imported_thread({"data": []}, "thread")
        with self.assertRaisesRegex(RuntimeError, "Malformed persisted-thread"):
            probe.imported_thread({"data": [None]}, "thread")
        with self.assertRaisesRegex(RuntimeError, "did not match"):
            probe.imported_thread(threads, "other")
        with self.assertRaisesRegex(RuntimeError, "one matching import"):
            probe.import_history({"data": []}, "import")
        with self.assertRaisesRegex(RuntimeError, "Malformed import history"):
            probe.import_history({"data": [None]}, "import")

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
            self.assertEqual(receipt["fixture"]["transcripts"], 0)
            self.assertFalse(receipt["fixture"]["session_title_present"])
            self.assertFalse(receipt["isolation"]["cleanup_complete"])
            self.assertEqual(
                receipt["error"], "Probe failed without recording source error text"
            )
            self.assertNotIn(str(Path(directory)), output.read_text())

    def test_teardown_failure_never_remains_a_pass(self):
        async def teardown(binary, receipt):
            receipt["passed"] = True
            receipt["isolation"]["cleanup_complete"] = True
            raise RuntimeError("teardown failed")

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "teardown.json"
            with patch.object(probe, "run_probe", teardown):
                self.assertEqual(
                    probe.main(["--codex", "/usr/bin/true", "--output", str(output)]),
                    1,
                )
            receipt = json.loads(output.read_text())
            self.assertFalse(receipt["passed"])
            self.assertFalse(receipt["isolation"]["cleanup_complete"])


if __name__ == "__main__":
    unittest.main()
