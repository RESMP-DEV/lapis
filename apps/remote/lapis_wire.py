"""The session service's wire protocol and launch identity, for Python clients.

One copy for the phone gateway (lapis_remote.py, which imports it from beside
itself) and the check scripts. The C++ definitions are the contract:
services/session/src/transport/local_protocol.hpp/.cpp for the protocol and
services/session/src/launch_spec.cpp for launch_fingerprint. The Python tests
pin both (scripts/tests/test_lapis_wire.py).
"""

from __future__ import annotations

import hashlib
import struct

# Wire protocol v6 (wire::version).
WIRE_VERSION = 6
# Message kinds (wire::Kind).
HELLO, SNAPSHOT, TEXT, PASTE, KEY, RESIZE, STATUS, ATTACH, READY = range(1, 10)
HISTORY_REQUEST, HISTORY_PAGE = 10, 11
ATTENTION_SNAPSHOT, ATTENTION_DECISION, ATTENTION_RETRY = 12, 13, 14
# Client request to end the agent; the service answers with `ended`.
TERMINATE = 15
# A turn of the wheel for a full-screen program (added within v6). Only a
# service whose snapshots set ACCEPTS_WHEEL in the alternate-screen byte takes
# it; one from before drops the connection on it.
WHEEL = 16
# One bounded paste admitted to the agent's input as a unit, optionally
# followed by Return, with the service's answer (added within v6). A view asks
# for it with the attach flag; a service that grants it says so in its hello.
PASTE_REQUEST, PASTE_RESULT = 17, 18

# Attach modes (wire::AttachMode): discover takes the agent from its current
# client; reconnect resumes one identity; create starts a new epoch; join
# (added within v6) shows it beside the desktop. Python exposes every C++ mode
# so the exhaustive pin catches drift even when this client never sends it.
DISCOVER, RECONNECT, CREATE, JOIN = 0, 1, 2, 3
ATTACH_PASTE_TRANSACTIONS = 0x20

MAX_FRAME = 8 * 1024 * 1024  # wire::max_frame_bytes
MAX_CELLS = 32768  # wire::max_cells
MAX_CODEPOINTS = 65536  # wire::max_codepoints
MAX_PASTE = 960 * 1024  # wire::max_paste_bytes

# Message layout. An attachment is two 16-byte identities and a BE u64
# generation; a snapshot message puts its sequence and three timestamps before
# the screen.
ATTACHMENT_BYTES = 40
SNAPSHOT_HEADER = 72  # wire::snapshot_header_bytes
# Offsets into an encoded screen (wire::encode_snapshot).
ALTERNATE_OFFSET, ACCEPTS_WHEEL = 21, 2
HISTORY_FIELDS_OFFSET = 37
PALETTE_OFFSET = 61
POOL_OFFSET = 1085
CELL = struct.Struct(">IIBBIBIBIBH")  # 27 bytes, local_protocol.cpp
WIDE_TAIL, WRAP_SPACER = 2, 3  # CellKind

# History direction `at` (added within v6): the page holding a row. A page's
# history fields (total, first row, rows) say where it sits; services before
# it send only the page itself, and reject `at`.
HISTORY_AT = 2
STATUS_NAMES = {1: "rejected", 2: "ended", 3: "replaced", 4: "overloaded"}

CODEX_PREFIX = b"lapis-codex-v1\0"
CLAUDE_PREFIX = b"lapis-claude-v1\0"


class WireError(RuntimeError):
    """A message that does not follow the protocol."""


def _require(condition, message):
    if not condition:
        raise WireError(message)


def qt_string(value):
    """A QString as QDataStream (Qt 6, big-endian) writes a non-null one.

    An empty Python string and a hypothetical null QString encode the same
    zero byte length. Callers must not use this helper where null and empty
    strings have different meanings.
    """
    encoded = str(value).encode("utf-16-be")
    return struct.pack(">I", len(encoded)) + encoded


def fingerprint(program, arguments, directory, mode=""):
    """SHA-256 launch fingerprint, as launch_fingerprint computes it.

    Like the C++, it hashes the values as given: pass the launch as the
    service validated it (absolute program path, canonical directory). `mode`
    is "codex" or "claude"; anything else is a plain terminal.
    """
    data = qt_string(program) + struct.pack(">I", len(arguments))
    data += b"".join(qt_string(argument) for argument in arguments)
    data += qt_string(directory)
    if mode == "claude":
        data = CLAUDE_PREFIX + data
    elif mode == "codex":
        data = CODEX_PREFIX + data
    return hashlib.sha256(data).digest()


def frame(kind, payload=b""):
    """One message: BE u32 length (kind plus payload), kind byte, payload."""
    return struct.pack(">IB", len(payload) + 1, kind) + payload


def status_message(data):
    """A status message as "<name>: <text>"."""
    code = data[0] if data else 0
    text = data[1:].decode("utf-8", "replace") if len(data) > 1 else ""
    return f"{STATUS_NAMES.get(code, 'status')}: {text}".strip()


def page_place(payload):
    """Where an archived page sits: every kept row, its first row among them
    (0 the oldest), its own rows, and whether its service can jump."""
    _require(len(payload) >= HISTORY_FIELDS_OFFSET + 24, "Truncated history place")
    total, offset, rows = struct.unpack_from(">QQQ", payload, HISTORY_FIELDS_OFFSET)
    _require(offset <= total and rows <= total - offset, "Invalid history place")
    return {
        "total": total,
        "offset": offset,
        "rows": rows,
        "scrubbable": offset > 0 or total > rows,
    }


def decode_history_reply(data, attachment):
    """Validate a history reply before it can reach a waiter or the stream."""
    _require(
        len(data) >= 60 and data[:ATTACHMENT_BYTES] == attachment, "Bad history reply"
    )
    request_id, page_id = struct.unpack_from(">QQ", data, ATTACHMENT_BYTES)
    length = struct.unpack_from(">I", data, 56)[0]
    _require(length <= len(data) - 60, "Truncated history message")
    message = data[60 : 60 + length].decode("utf-8", "replace")
    snapshot = data[60 + length :]
    if snapshot:
        _require(len(snapshot) >= POOL_OFFSET + 4, "Truncated history snapshot")
        page_place(snapshot)
    return request_id, {
        "page": page_id,
        "message": message,
        "snapshot": snapshot or None,
    }
