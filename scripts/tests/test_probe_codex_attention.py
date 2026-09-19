"""No-turn capability classification must not turn unexpected failures into passes."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock

from scripts import probe_codex_attention as probe


class ProbeTests(unittest.IsolatedAsyncioTestCase):
    async def test_initialize_matches_live_schema(self):
        client = AsyncMock()
        client.rpc.return_value = {
            "userAgent": "fixture",
            "codexHome": "/private",
            "platformFamily": "unix",
            "platformOs": "macos",
        }
        await probe.initialize(client)
        client.send.assert_awaited_once_with({"method": "initialized"})
        client.rpc.return_value = {"platformOS": "wrong spelling"}
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            await probe.initialize(client)

    async def test_thread_identity_must_match(self):
        client = AsyncMock()
        client.rpc.return_value = {"thread": {"id": "other"}}
        with self.assertRaisesRegex(RuntimeError, "different thread"):
            await probe.inspect_thread(client, "expected", "thread/read", [])

    async def test_specific_unavailable_case(self):
        client = AsyncMock()
        client.rpc.side_effect = probe.RpcError(
            "thread/resume", -32600, "no rollout found for thread id thread"
        )
        phases = []
        self.assertFalse(
            await probe.inspect_thread(client, "thread", "thread/resume", phases)
        )
        self.assertEqual(phases[0]["status"], "unavailable_for_zero_turn_fixture")
        client.rpc.side_effect = probe.RpcError("thread/resume", -32603)
        with self.assertRaises(probe.RpcError):
            await probe.inspect_thread(client, "thread", "thread/resume", [])
        client.rpc.side_effect = probe.RpcError(
            "thread/resume", -32600, "invalid parameters"
        )
        with self.assertRaises(probe.RpcError):
            await probe.inspect_thread(client, "thread", "thread/resume", [])

    async def test_dead_child_does_not_count_as_transport_pass(self):
        receipt = {"phases": [], "passed": False}
        with self.assertRaisesRegex(RuntimeError, "exited before listening"):
            await probe.run_probe(Path("/usr/bin/false"), receipt)
        self.assertFalse(receipt["passed"])


class ReceiptTests(unittest.TestCase):
    def test_failure_writes_json_and_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "failure.json"
            self.assertEqual(
                probe.main(["--codex", "/no-such-codex", "--output", str(output)]), 1
            )
            receipt = json.loads(output.read_text())
            self.assertFalse(receipt["passed"])
            self.assertEqual(receipt["turns_started"], 0)


if __name__ == "__main__":
    unittest.main()
