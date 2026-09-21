"""Behavioral regressions for the managed-service attention stream."""

import socket
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from check_cli_launch import ATTENTION_RETRY, CheckError, WireClient, frame
from check_service_attention import View, exercise


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


class DiagnosticTests(unittest.IsolatedAsyncioTestCase):
    async def test_missing_codex_has_clear_diagnostic(self):
        with patch("check_service_attention.shutil.which", return_value=None):
            with self.assertRaisesRegex(CheckError, "codex is not installed"):
                await exercise(object(), {})


if __name__ == "__main__":
    unittest.main()
