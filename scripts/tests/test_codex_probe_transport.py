"""Bounded WebSocket framing and failed-handshake cleanup regressions."""

import asyncio
import base64
import hashlib
import struct
import unittest
from unittest.mock import patch

from scripts import codex_probe_transport as transport


class Writer:
    def __init__(self, callback=None):
        self.output = bytearray()
        self.closed = False
        self.callback = callback

    def write(self, data):
        self.output.extend(data)
        if self.callback:
            self.callback(data)

    async def drain(self):
        pass

    def close(self):
        self.closed = True

    async def wait_closed(self):
        pass


def frame(payload, opcode=1, fin=True):
    first = opcode | (128 if fin else 0)
    if len(payload) < 126:
        header = bytes([first, len(payload)])
    elif len(payload) <= 65535:
        header = bytes([first, 126]) + struct.pack(">H", len(payload))
    else:
        header = bytes([first, 127]) + struct.pack(">Q", len(payload))
    return header + payload


class TransportTests(unittest.IsolatedAsyncioTestCase):
    async def test_control_bound_and_unicode_close(self):
        writer = Writer()
        ws = transport.UnixWebSocketTransport(asyncio.StreamReader(), writer)
        with self.assertRaises(transport.TransportError):
            await ws.send(b"x" * 126, opcode=transport.OP_PING)
        self.assertFalse(writer.output)
        await ws.close(reason="é" * 100)
        sent = writer.output
        self.assertLessEqual(sent[1] & 127, 125)
        mask = sent[2:6]
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(sent[6:]))
        self.assertEqual(payload[2:].decode("utf-8"), "é" * 61)
        self.assertTrue(writer.closed)

    async def test_upgrade_and_masked_frame_lengths(self):
        reader = asyncio.StreamReader()

        def handshake(data):
            key = data.split(b"Sec-WebSocket-Key: ")[1].split(b"\r\n")[0]
            accept = base64.b64encode(
                hashlib.sha1(key + transport.WEBSOCKET_GUID.encode()).digest()
            )
            reader.feed_data(
                b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                + accept
                + b"\r\n\r\n"
            )

        writer = Writer(handshake)
        with patch.object(
            transport.asyncio, "open_unix_connection", return_value=(reader, writer)
        ):
            ws = await transport.UnixWebSocketTransport.connect("/fixture.sock")
        writer.callback = None
        for size in [10, 200, 70000]:
            writer.output.clear()
            payload = b"x" * size
            await ws.send(payload)
            sent = writer.output
            self.assertEqual(sent[1] & 128, 128)
            width = 0 if size < 126 else 2 if size <= 65535 else 8
            length = (
                sent[1] & 127
                if width == 0
                else int.from_bytes(sent[2 : 2 + width], "big")
            )
            self.assertEqual(length, size)
            mask = sent[2 + width : 6 + width]
            self.assertEqual(
                bytes(b ^ mask[i % 4] for i, b in enumerate(sent[6 + width :])), payload
            )
        await ws.close()
        self.assertTrue(writer.closed)

    async def test_handshake_failure_and_timeout_close_socket(self):
        for reply in [
            b"HTTP/1.1 403 Forbidden\r\n\r\n",
            b"HTTP/1.1 101 OK\r\nSec-WebSocket-Accept: wrong\r\n\r\n",
            None,
        ]:
            reader = asyncio.StreamReader()
            writer = Writer(
                lambda _, r=reply, target=reader: target.feed_data(r) if r else None
            )
            with (
                patch.object(
                    transport.asyncio,
                    "open_unix_connection",
                    return_value=(reader, writer),
                ),
                self.assertRaises(transport.TransportError),
            ):
                await transport.UnixWebSocketTransport.connect(
                    "/fixture.sock", connect_timeout=0.02
                )
            self.assertTrue(writer.closed)

    async def test_fragmentation_and_ping(self):
        reader, writer = asyncio.StreamReader(), Writer()
        ws = transport.UnixWebSocketTransport(reader, writer)
        reader.feed_data(
            frame(b"frag", fin=False)
            + frame(b"ping", opcode=9)
            + frame(b"ment", opcode=0)
        )
        self.assertEqual(await ws.receive(), b"fragment")
        self.assertEqual(writer.output[0], 0x8A)

    async def test_peer_close(self):
        reader, writer = asyncio.StreamReader(), Writer()
        ws = transport.UnixWebSocketTransport(reader, writer)
        reader.feed_data(frame(b"\x03\xe8done", opcode=8))
        with self.assertRaises(transport.WebSocketClose) as caught:
            await ws.receive()
        self.assertEqual(caught.exception.code, 1000)
        await ws.close()
        self.assertTrue(writer.closed)

    async def test_invalid_frames_and_truncation(self):
        cases = [
            b"\xc1\x00",
            b"\x81\x80",
            frame(b"orphan", opcode=0),
            frame(b"\xff"),
            b"\x81\x7e\x00\x01x",
            b"\x81\x7f" + struct.pack(">Q", 1 << 63),
            b"\x89\x7e\x00\x7e",
            frame(b"x", opcode=8),
            b"\x81\x05ab",
            b"\x81\x7e\x00",
        ]
        for data in cases:
            with self.subTest(data=data):
                reader, writer = asyncio.StreamReader(), Writer()
                ws = transport.UnixWebSocketTransport(reader, writer)
                reader.feed_data(data)
                reader.feed_eof()
                with self.assertRaises(transport.TransportError):
                    await asyncio.wait_for(ws.receive(), 1)
                await ws.close()
        reader, writer = asyncio.StreamReader(), Writer()
        ws = transport.UnixWebSocketTransport(reader, writer, max_message_bytes=4)
        reader.feed_data(frame(b"abc", fin=False) + frame(b"de", opcode=0))
        with self.assertRaises(transport.TransportError):
            await ws.receive()


if __name__ == "__main__":
    unittest.main()
