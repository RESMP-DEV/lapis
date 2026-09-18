"""Small, validated WebSocket client transport for local Codex probes."""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import os
import struct
from pathlib import Path
from typing import Any

WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_HANDSHAKE_BYTES = 16 * 1024
MAX_MESSAGE_BYTES = 2 * 1024 * 1024
WRITE_TIMEOUT = 5.0
OP_CONTINUATION = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA


class TransportError(RuntimeError):
    """The local WebSocket transport violated a bounded protocol rule."""


class WebSocketClose(TransportError):
    """A peer-initiated WebSocket close."""

    def __init__(self, code: int, reason: str):
        super().__init__(f"WebSocket peer closed with code {code}")
        self.code = code
        self.reason = reason


class UnixWebSocketTransport:
    """A client-only RFC 6455 transport over an asyncio Unix stream."""

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        *,
        max_message_bytes: int = MAX_MESSAGE_BYTES,
    ) -> None:
        self.reader = reader
        self.writer = writer
        self.max_message_bytes = max_message_bytes
        self.closed = False
        self._closing = False

    @classmethod
    async def connect(
        cls,
        path: str | Path,
        *,
        connect_timeout: float = 10.0,
        max_message_bytes: int = MAX_MESSAGE_BYTES,
    ) -> UnixWebSocketTransport:
        """Validate the HTTP upgrade; close the socket on every failed handshake."""
        writer = None
        accepted = False
        try:
            async with asyncio.timeout(connect_timeout):
                reader, writer = await asyncio.open_unix_connection(
                    str(path), limit=MAX_HANDSHAKE_BYTES
                )
                key = base64.b64encode(os.urandom(16)).decode("ascii")
                writer.write(
                    (
                        "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: "
                        + key
                        + "\r\nSec-WebSocket-Version: 13\r\n\r\n"
                    ).encode("ascii")
                )
                await writer.drain()
                header = await reader.readuntil(b"\r\n\r\n")
                if len(header) > MAX_HANDSHAKE_BYTES:
                    raise TransportError("WebSocket handshake header exceeded bound")
                lines = header.split(b"\r\n")
                if lines[0].split()[:2] != [b"HTTP/1.1", b"101"]:
                    raise TransportError("WebSocket handshake did not return HTTP 101")
                headers = {}
                for line in lines[1:]:
                    if not line:
                        continue
                    name, separator, value = line.partition(b":")
                    name = name.strip().lower()
                    if not separator or name in headers:
                        raise TransportError(
                            "Invalid or duplicate WebSocket handshake header"
                        )
                    headers[name] = value.strip()
                expected = base64.b64encode(
                    hashlib.sha1((key + WEBSOCKET_GUID).encode("ascii")).digest()
                )
                if headers.get(b"sec-websocket-accept") != expected:
                    raise TransportError("WebSocket accept header mismatch")
                if headers.get(b"upgrade", b"").lower() != b"websocket":
                    raise TransportError("WebSocket upgrade header mismatch")
                if b"upgrade" not in {
                    p.strip().lower()
                    for p in headers.get(b"connection", b"").split(b",")
                }:
                    raise TransportError(
                        "WebSocket connection header did not include Upgrade"
                    )
                if (
                    b"sec-websocket-extensions" in headers
                    or b"sec-websocket-protocol" in headers
                ):
                    raise TransportError(
                        "Unrequested WebSocket extension or subprotocol"
                    )
                accepted = True
                return cls(reader, writer, max_message_bytes=max_message_bytes)
        except (OSError, EOFError, TimeoutError, asyncio.LimitOverrunError) as error:
            raise TransportError("WebSocket handshake failed") from error
        finally:
            if writer is not None and not accepted:
                writer.close()
                try:
                    async with asyncio.timeout(1):
                        await writer.wait_closed()
                except (OSError, TimeoutError):
                    pass

    async def send(self, payload: bytes, *, opcode: int = OP_TEXT) -> None:
        """Send one masked client frame; text callers supply UTF-8 bytes."""
        if self.closed:
            raise TransportError("WebSocket is closed")
        if len(payload) > self.max_message_bytes:
            raise TransportError("WebSocket outbound message exceeded bound")
        if opcode not in (OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG):
            raise TransportError("WebSocket outbound opcode is invalid")
        if opcode >= OP_CLOSE and len(payload) > 125:
            raise TransportError("WebSocket outbound control frame exceeded bound")
        mask = os.urandom(4)
        first = 0x80 | opcode
        length = len(payload)
        if length < 126:
            header = bytes((first, 0x80 | length))
        elif length <= 0xFFFF:
            header = bytes((first, 0x80 | 126)) + struct.pack(">H", length)
        else:
            header = bytes((first, 0x80 | 127)) + struct.pack(">Q", length)
        masked = bytes(byte ^ mask[index & 3] for index, byte in enumerate(payload))
        try:
            self.writer.write(header + mask + masked)
            async with asyncio.timeout(WRITE_TIMEOUT):
                await self.writer.drain()
        except TimeoutError as error:
            self.abort()
            raise TransportError("WebSocket send timed out") from error
        except (BrokenPipeError, ConnectionResetError) as error:
            raise TransportError(
                f"WebSocket connection closed during send: {error}"
            ) from error

    async def send_json(self, message: dict[str, Any]) -> None:
        try:
            payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        except (TypeError, ValueError, UnicodeError) as error:
            raise TransportError(
                f"message was not encodable as JSON: {error}"
            ) from error
        await self.send(payload, opcode=OP_TEXT)

    async def receive(self) -> bytes:
        """Receive a bounded text/binary message, including fragmented messages."""
        fragments = bytearray()
        message_opcode = None
        try:
            while True:
                first, second = await self.reader.readexactly(2)
                if first & 0x70 or second & 0x80:
                    raise TransportError("Unexpected WebSocket RSV bits or server mask")
                opcode, fin, length = first & 15, bool(first & 128), second & 127
                if length in (126, 127):
                    if opcode >= OP_CLOSE:
                        raise TransportError("Extended control frame length")
                    size = 2 if length == 126 else 8
                    length = int.from_bytes(await self.reader.readexactly(size), "big")
                    if (size == 2 and length < 126) or (
                        size == 8 and (length <= 65535 or length >> 63)
                    ):
                        raise TransportError("Noncanonical WebSocket length")
                if opcode >= OP_CLOSE:
                    if not fin or length > 125:
                        raise TransportError("Fragmented or oversized control frame")
                elif (
                    length > self.max_message_bytes
                    or len(fragments) + length > self.max_message_bytes
                ):
                    raise TransportError("WebSocket inbound message exceeded bound")
                payload = await self.reader.readexactly(length)
                if opcode >= OP_CLOSE:
                    if opcode == OP_CLOSE:
                        if length == 1:
                            raise TransportError("Invalid close frame length")
                        code = int.from_bytes(payload[:2], "big") if length else 1005
                        if length and (
                            code < 1000
                            or code >= 5000
                            or code in (1004, 1005, 1006, 1015)
                        ):
                            raise TransportError("Invalid close code")
                        reason = payload[2:].decode("utf-8")
                        raise WebSocketClose(code, reason)
                    if opcode == OP_PING:
                        await self.send(payload, opcode=OP_PONG)
                    elif opcode != OP_PONG:
                        raise TransportError("Unknown WebSocket control opcode")
                    continue
                if opcode in (OP_TEXT, OP_BINARY) and message_opcode is None:
                    message_opcode = opcode
                elif opcode != OP_CONTINUATION or message_opcode is None:
                    raise TransportError(
                        "Invalid WebSocket fragmentation or data opcode"
                    )
                fragments.extend(payload)
                if fin:
                    if message_opcode == OP_TEXT:
                        fragments.decode("utf-8")
                    return bytes(fragments)
        except (asyncio.IncompleteReadError, UnicodeDecodeError) as error:
            raise TransportError("Truncated or non-UTF8 WebSocket message") from error

    async def receive_json(self) -> dict[str, Any]:
        payload = await self.receive()
        try:
            message = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise TransportError("Invalid JSON message") from error
        if not isinstance(message, dict):
            raise TransportError("JSON message is not an object")
        return message

    async def close(self, code: int = 1000, reason: str = "") -> None:
        """Bound close writes/waits; socket teardown must not hang the fixture."""
        if self.closed:
            return
        try:
            # Argument errors must still run socket cleanup below.
            if code < 1000 or code >= 5000 or code in (1004, 1005, 1006, 1015):
                raise ValueError("Invalid WebSocket close code")
            # Truncate only at a complete UTF-8 character boundary.
            reason_bytes = (
                reason.encode("utf-8")[:123]
                .decode("utf-8", errors="ignore")
                .encode("utf-8")
            )
            payload = struct.pack(">H", code) + reason_bytes
            async with asyncio.timeout(1):
                await self.send(payload, opcode=OP_CLOSE)
        except (OSError, TimeoutError, TransportError):
            pass
        finally:
            self.closed = True
            self.writer.close()
            try:
                async with asyncio.timeout(1):
                    await self.writer.wait_closed()
            except (OSError, TimeoutError):
                pass

    def abort(self) -> None:
        """Close immediately after an inbound failure/cancellation."""
        self.closed = True
        self.writer.close()
