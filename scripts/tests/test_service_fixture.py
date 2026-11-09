"""Behavioral regressions for the shared session-service fixture helpers."""

import socket
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from check_cli_launch import ATTENTION_RETRY, CheckError, WireClient, frame
from service_fixture import View, cleanup_service


class StreamSocket:
    def __init__(self, chunks, *, expire_first=False):
        self.chunks = list(chunks)
        self.now = 0
        self.expire_first = expire_first
        self.closed = False

    def settimeout(self, timeout):
        pass

    def recv(self, size):
        if not self.chunks:
            raise socket.timeout
        if self.expire_first:
            self.expire_first = False
            self.now += 1
        return self.chunks.pop(0)

    def close(self):
        self.closed = True


def client_for(stream):
    client = WireClient.__new__(WireClient)
    client.socket = stream
    client.buffer = bytearray()
    client.attachment = b"a" * 40
    client.cached_snapshot = None
    return client


class ViewTests(unittest.IsolatedAsyncioTestCase):
    async def test_unrelated_failure_with_deadline_text_remains_fatal(self):
        stream = StreamSocket([])
        client = client_for(stream)
        failure = CheckError("Frame deadline expired")
        with patch.object(client, "receive", side_effect=failure):
            view = View(client)
            with self.assertRaises(CheckError) as raised:
                await view.wait(lambda: False, 2)
            self.assertIs(raised.exception, failure)
            with self.assertRaises(CheckError):
                await view.close()
        self.assertTrue(stream.closed)

    async def test_partial_frame_deadline_preserves_and_completes_frame(self):
        payload = b"a" * 40 + b"exact-retry-token"
        encoded = frame(ATTENTION_RETRY, payload)
        stream = StreamSocket([encoded[:2], encoded[2:]], expire_first=True)
        client = client_for(stream)
        # Only the wire reader's clock advances past its first deadline. The
        # second receive must resume the same buffered frame, without data loss.
        with patch(
            "check_cli_launch.time", SimpleNamespace(monotonic=lambda: stream.now)
        ):
            view = View(client)
            try:
                await view.wait(lambda: view.retry is not None, 2)
                self.assertEqual(view.retry, b"exact-retry-token")
                self.assertEqual(client.buffer, b"")
            finally:
                await view.close()
        self.assertTrue(stream.closed)

    async def test_malformed_frame_and_eof_remain_fatal(self):
        for data, error, message in [
            (bytes(4), CheckError, "Invalid frame size"),
            (b"", EOFError, "Service disconnected"),
        ]:
            with self.subTest(message=message):
                stream = StreamSocket([data])
                view = View(client_for(stream))
                with self.assertRaisesRegex(error, message):
                    await view.wait(lambda: False, 2)
                with self.assertRaises(error):
                    await view.close()
                self.assertTrue(stream.closed)


class CleanupTests(unittest.IsolatedAsyncioTestCase):
    async def test_primary_failure_survives_cleanup_failures(self):
        for stop_fails, groups_fail in [(True, False), (False, True), (True, True)]:
            with self.subTest(stop_fails=stop_fails, groups_fail=groups_fail):
                service = Mock()
                service.stop.side_effect = (
                    RuntimeError("stop failed") if stop_fails else None
                )
                wait = AsyncMock(side_effect=TimeoutError() if groups_fail else None)
                receipt = {}
                original = CheckError("qualification failed")
                with patch("service_fixture.wait_groups_gone", wait):
                    with self.assertRaises(CheckError) as raised:
                        try:
                            raise original
                        finally:
                            await cleanup_service(
                                service, {42}, receipt, sys.exception()
                            )
                self.assertIs(raised.exception, original)
                expected = []
                if stop_fails:
                    expected.append("service cleanup: stop failed")
                if groups_fail:
                    expected.append("process-group cleanup: TimeoutError")
                self.assertEqual(receipt["cleanup_errors"], expected)
                self.assertEqual(original.__notes__, expected)
                service.stop.assert_called_once_with()
                wait.assert_awaited_once_with({42})

    async def test_cleanup_failure_after_success_fails_qualification(self):
        for stage in ("stop", "groups"):
            with self.subTest(stage=stage):
                service = Mock()
                service.stop.side_effect = (
                    RuntimeError("stop failed") if stage == "stop" else None
                )
                wait = AsyncMock(
                    side_effect=TimeoutError() if stage == "groups" else None
                )
                receipt = {}
                with patch("service_fixture.wait_groups_gone", wait):
                    with self.assertRaises(CheckError) as raised:
                        await cleanup_service(service, {42}, receipt, None)
                self.assertEqual(str(raised.exception), receipt["cleanup_errors"][0])
                service.stop.assert_called_once_with()
                wait.assert_awaited_once_with({42})

    async def test_successful_cleanup_records_reaped_processes(self):
        for groups in (set(), {42}):
            with self.subTest(groups=groups):
                service = Mock()
                service.process.poll.return_value = 0
                receipt = {}
                with patch(
                    "service_fixture.wait_groups_gone", new_callable=AsyncMock
                ) as wait:
                    await cleanup_service(service, groups, receipt, None)
                self.assertTrue(receipt["service_reaped"])
                self.assertNotIn("cleanup_errors", receipt)
                service.stop.assert_called_once_with()
                if groups:
                    wait.assert_awaited_once_with(groups)
                    self.assertTrue(receipt["owned_process_groups_cleaned"])
                else:
                    wait.assert_not_awaited()
                    self.assertNotIn("owned_process_groups_cleaned", receipt)


if __name__ == "__main__":
    unittest.main()
