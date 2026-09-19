"""Decision and event-routing regressions for the live qualification harness."""

import asyncio
import unittest
from unittest.mock import AsyncMock

from scripts.check_codex_attention import (
    Client,
    approved_fixture,
    matches,
    resume_snapshot,
)


class FakeTransport:
    def __init__(self):
        self.incoming = asyncio.Queue()
        self.outgoing = asyncio.Queue()
        self.closed = False

    async def receive_json(self):
        message = await self.incoming.get()
        if isinstance(message, Exception):
            raise message
        return message

    async def send_json(self, message):
        await self.outgoing.put(message)

    async def close(self):
        self.closed = True


class DecisionTests(unittest.TestCase):
    def test_exact_command_only(self):
        for command in (
            'python3 -c "print(123456789)"',
            "/bin/bash -lc 'python3 -c \"print(123456789)\"'",
            "/opt/homebrew/bin/bash -c 'python3 -c \"print(123456789)\"'",
        ):
            self.assertTrue(approved_fixture(command))
        for command in (
            None,
            'python3 -c "print(123456789)"; touch unexpected',
            'python3 -c "print(123456789); import os"',
            'python3 -c "print(123456789)" > unexpected',
            "/untrusted/bash -c 'python3 -c \"print(123456789)\"'",
            'python3 -c "',
        ):
            self.assertFalse(approved_fixture(command))

    def test_completion_requires_same_thread_turn_and_typed_request(self):
        event = {
            "method": "serverRequest/resolved",
            "params": {"threadId": "a", "requestId": 1},
        }
        self.assertTrue(matches(event, event["method"], "a", request_id=1))
        self.assertFalse(matches(event, event["method"], "b", request_id=1))
        self.assertFalse(matches(event, event["method"], "a", request_id="1"))
        self.assertFalse(matches(event, event["method"], "a", request_id=True))
        event = {
            "method": "turn/completed",
            "params": {"threadId": "a", "turn": {"id": "old"}},
        }
        self.assertFalse(matches(event, event["method"], "a", turn="new"))


class RoutingTests(unittest.IsolatedAsyncioTestCase):
    async def test_resolution_wins_over_late_replay_with_typed_ids(self):
        client = AsyncMock()
        numeric = {
            "method": "item/tool/requestUserInput",
            "id": 1,
            "params": {"threadId": "t"},
        }
        textual = {**numeric, "id": "1"}
        resolved = {
            "method": "serverRequest/resolved",
            "params": {"threadId": "t", "requestId": 1},
        }
        client.rpc.side_effect = [
            {},
            ({"thread": {"id": "t"}}, [resolved, numeric, textual]),
        ]
        _, pending = await resume_snapshot(client, "t")
        self.assertEqual(pending, [textual])

    async def test_rpc_boundary_excludes_later_events(self):
        transport = FakeTransport()
        client = Client(transport)
        try:
            call = asyncio.create_task(
                client.rpc("thread/read", {}, capture_events=True)
            )
            sent = await transport.outgoing.get()
            before = {"method": "before"}
            after = {"method": "after"}
            for message in (before, {"id": sent["id"], "result": {}}, after):
                await transport.incoming.put(message)
            self.assertEqual(await call, ({}, [before]))
            self.assertEqual(await client.event(lambda _: True, timeout=1), after)
        finally:
            await client.close()

    async def test_server_request_cannot_satisfy_client_rpc(self):
        transport = FakeTransport()
        client = Client(transport)
        try:
            call = asyncio.create_task(client.rpc("fixture", {}))
            sent = await transport.outgoing.get()
            request = {
                "id": sent["id"],
                "method": "item/tool/requestUserInput",
                "params": {},
            }
            await transport.incoming.put(request)
            observed = await client.event(lambda m: m == request, timeout=1)
            self.assertEqual(observed, request)
            self.assertFalse(call.done())
            await transport.incoming.put({"id": sent["id"], "result": {"ok": True}})
            self.assertEqual(await call, {"ok": True})
        finally:
            await client.close()
        self.assertTrue(transport.closed)

    async def test_source_failure_wakes_pending_rpc_and_event_waiter(self):
        transport = FakeTransport()
        client = Client(transport)
        try:
            call = asyncio.create_task(client.rpc("fixture", {}))
            await transport.outgoing.get()
            await transport.incoming.put(EOFError("fixture disconnected"))
            with self.assertRaisesRegex(RuntimeError, "transport failed"):
                await call
            with self.assertRaisesRegex(RuntimeError, "transport failed"):
                await client.event(lambda _: True, timeout=1)
        finally:
            await client.close()

    async def test_event_overflow_fails_closed(self):
        transport = FakeTransport()
        client = Client(transport)
        try:
            for _ in range(1025):
                await transport.incoming.put({"method": "fixture"})
            async with asyncio.timeout(1):
                while client.failure is None:
                    await asyncio.sleep(0)
            self.assertIsInstance(client.failure, asyncio.QueueFull)
            with self.assertRaisesRegex(RuntimeError, "transport failed"):
                await client.send({"id": "stale", "result": {}})
        finally:
            await client.close()


if __name__ == "__main__":
    unittest.main()
