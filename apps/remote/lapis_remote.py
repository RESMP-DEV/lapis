#!/usr/bin/env python3
"""Serve the lapis workspace's agents to the lapis iPhone app.

The gateway reads the desktop's workspace registry and attaches to an
agent's session service only while the phone shows that agent. It joins the
session beside the desktop, so both show the same screen and either can type;
the device in use sets the terminal size, and the desktop's returns when the
phone leaves. A service started before joining
existed rejects it, and then the phone takes the agent from the desktop until
Reconnect agent on the Mac. Agents keep running either way.

It listens on this Mac's Tailscale address. A request is served only when
`tailscale whois` names the same login as this Mac and the peer is an iOS
device or this Mac itself (the simulator). Browsers are refused: requests
that carry Origin, or lack the X-Lapis-Client header, are rejected, so a
web page on the phone cannot drive an agent.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import itertools
import json
import os
import select
import socket
import struct
import subprocess
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PORT = 7349
CLIENT_HEADER = "X-Lapis-Client"
GATEWAY_VERSION = 1

# Wire protocol v6 (services/session/src/transport/local_protocol.hpp).
WIRE_VERSION = 6
HELLO, SNAPSHOT, TEXT, PASTE, KEY, RESIZE, STATUS, ATTACH, READY = range(1, 10)
HISTORY_REQUEST, HISTORY_PAGE = 10, 11
STATUS_NAMES = {1: "rejected", 2: "ended", 3: "replaced", 4: "overloaded"}
# Attach modes: discover takes the agent from its current client; join (added
# within v6) shows it beside the desktop. Services started before join reject it.
DISCOVER, JOIN = 0, 3
MAX_FRAME = 8 * 1024 * 1024
ATTACHMENT_BYTES = 40
SNAPSHOT_HEADER = 72
PALETTE_OFFSET = 61
POOL_OFFSET = 1085
CELL = struct.Struct(">IIBBIBIBIBH")  # 27 bytes, local_protocol.cpp
WIDE_TAIL, WRAP_SPACER = 2, 3

# TerminalKey (services/session/include/lapis/session/terminal.hpp).
KEYS = {
    "up": 0,
    "down": 1,
    "left": 2,
    "right": 3,
    "home": 4,
    "end": 5,
    "pageUp": 6,
    "pageDown": 7,
    "delete": 9,
    "enter": 10,
    "tab": 11,
    "backspace": 12,
    "escape": 13,
}
SHIFT, CONTROL, ALT = 1, 2, 4
MAX_INPUT = 32 * 1024
MAX_CAPTURE = 12 * 1024 * 1024

# Run style bits sent to the phone.
BOLD, ITALIC, FAINT, UNDERLINE, STRIKE, CURSOR = 1, 2, 4, 8, 16, 32


class GatewayError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise GatewayError(message)


# Registry -------------------------------------------------------------------


def qt_string(value):
    encoded = str(value).encode("utf-16-be")
    return struct.pack(">I", len(encoded)) + encoded


def fingerprint(program, arguments, directory, mode):
    """SHA-256 launch fingerprint, as launch_spec.cpp computes it."""
    data = qt_string(program) + struct.pack(">I", len(arguments))
    data += b"".join(qt_string(argument) for argument in arguments)
    data += qt_string(Path(directory).resolve())
    if mode == "claude":
        data = b"lapis-claude-v1\0" + data
    elif mode == "codex":
        data = b"lapis-codex-v1\0" + data
    return hashlib.sha256(data).digest()


def load_workspace(path):
    """Categories and agents from the desktop's registry, as it reads them."""
    path = Path(path).absolute()
    data = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(data, dict), "Workspace registry is not an object")
    categories = []
    for category in data.get("categories", []):
        categories.append(
            {"id": str(category.get("id", "")), "name": str(category.get("name", ""))}
        )
    agents = []
    for entry in data.get("agents", []):
        identifier = str(entry.get("id", ""))
        harness = str(entry.get("harness", "codex"))
        if "arguments" in entry:
            arguments = [str(argument) for argument in entry.get("arguments", [])]
        elif entry.get("resumeThread"):
            arguments = ["resume", str(entry["resumeThread"])]
        else:
            arguments = []
        if harness == "codex":
            mode = "codex"
        elif harness == "claude" and entry.get("mode") == "claude":
            mode = "claude"
        else:
            mode = ""
        endpoint = str(entry.get("endpoint", ""))
        # Only the registry's own private endpoints are reachable.
        if not identifier or endpoint != str(path.parent / (identifier + ".sock")):
            continue
        agents.append(
            {
                "id": identifier,
                "title": str(entry.get("title", "")),
                "category": str(entry.get("category", "")),
                "harness": harness,
                "directory": str(entry.get("directory", "")),
                "program": str(entry.get("program", "")),
                "arguments": arguments,
                "mode": mode,
                "endpoint": endpoint,
            }
        )
    return {
        "categories": categories,
        "agents": agents,
        "activeCategory": str(data.get("activeCategory", "")),
    }


SHELL_HOSTS = ("ssh", "mosh", "et")
SSH_VALUE_OPTIONS = set("bcDEeFIiJLlmOoPpQRSWw")


def remote_machine(agent):
    """The host an ssh, mosh or et launch runs on, or "" for this Mac."""
    if Path(agent["program"]).name not in SHELL_HOSTS:
        return ""
    arguments = iter(agent["arguments"])
    for argument in arguments:
        if argument == "--":
            argument = next(arguments, "")
        elif argument.startswith("-"):
            if len(argument) == 2 and argument[1] in SSH_VALUE_OPTIONS:
                next(arguments, None)
            continue
        return argument.rsplit("@", 1)[-1]
    return ""


def display_place(agent):
    """Where an agent runs, as the phone shows it: ~/dev/x here, devbox:~/x elsewhere."""
    machine = remote_machine(agent)
    if machine:
        remote = next(
            (
                part.split("cd ", 1)[1].split("&&")[0].split(";")[0].strip()
                for part in agent["arguments"]
                if "cd " in part
            ),
            "",
        )
        return machine, f"{machine}:{remote}"
    directory = agent["directory"]
    home = str(Path.home())
    if directory == home or directory.startswith(home + "/"):
        directory = "~" + directory[len(home) :]
    return "", directory


def service_answers(endpoint, timeout=0.3):
    """True when a session service accepts connections at the endpoint."""
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    probe.settimeout(timeout)
    try:
        probe.connect(endpoint)
        return True
    except OSError:
        return False
    finally:
        probe.close()


# Snapshots ------------------------------------------------------------------


def hex_color(rgb):
    return f"#{rgb & 0xFFFFFF:06x}"


def resolve(kind, value, palette):
    if kind == 1:
        return palette[value] if value < len(palette) else None
    if kind == 2:
        return value & 0xFFFFFF
    return None


def render_snapshot(payload, show_cursor=True):
    """Decode a terminal snapshot into styled runs per row for the phone.

    Each run is [text, foreground, background, flags, column, width]: the
    phone draws it at its column across its width in cells, so backgrounds fill
    whole rows and wide characters keep their two cells.
    """
    require(len(payload) >= POOL_OFFSET + 4, "Truncated snapshot")
    revision, columns, rows, cursor_x, cursor_y = struct.unpack_from(">QHHHH", payload)
    in_viewport, visible = payload[16], payload[17]
    alternate, application_cursor = payload[21], payload[23]
    default_fg, default_bg = struct.unpack_from(">II", payload, 24)
    palette = struct.unpack_from(">256I", payload, PALETTE_OFFSET)
    count = struct.unpack_from(">I", payload, POOL_OFFSET)[0]
    offset = POOL_OFFSET + 4
    require(len(payload) >= offset + 4 * count + 4, "Truncated grapheme pool")
    points = struct.unpack_from(f">{count}I", payload, offset)
    offset += 4 * count
    cells = struct.unpack_from(">I", payload, offset)[0]
    offset += 4
    require(cells == columns * rows, "Invalid snapshot geometry")
    require(len(payload) == offset + cells * CELL.size, "Invalid cell payload")
    cursor = (cursor_x, cursor_y) if show_cursor and in_viewport and visible else None
    lines = []
    line = []
    current = None  # [texts, style, column, width]
    for index, cell in enumerate(CELL.iter_unpack(payload[offset:])):
        start, length, kind, fg_kind, fg, bg_kind, bg, _, _, underline, flags = cell
        column, row = index % columns, index // columns
        if kind == WIDE_TAIL and current is not None:
            current[3] += 1
        elif kind != WIDE_TAIL:
            text = (
                "".join(chr(point) for point in points[start : start + length])
                if length and kind != WRAP_SPACER
                else " "
            )
            foreground = resolve(fg_kind, fg, palette)
            background = resolve(bg_kind, bg, palette)
            style = (BOLD if flags & 1 else 0) | (ITALIC if flags & 2 else 0)
            style |= (FAINT if flags & 4 else 0) | (STRIKE if flags & 64 else 0)
            style |= UNDERLINE if underline else 0
            if flags & 16:  # inverse
                foreground, background = (
                    default_bg if background is None else background,
                    default_fg if foreground is None else foreground,
                )
            if flags & 32:  # invisible
                text = " " * len(text)
            if cursor == (column, row):
                style |= CURSOR
            key = (foreground, background, style)
            if current is None or current[1] != key:
                if current is not None:
                    line.append(run(*current))
                current = [[], key, column, 0]
            current[0].append(text)
            current[3] += 1
        if column == columns - 1:
            if current is not None:
                line.append(run(*current))
            lines.append(line)
            line, current = [], None
    return {
        "revision": revision,
        "columns": columns,
        "rows": rows,
        "cursor": {"x": cursor_x, "y": cursor_y, "visible": cursor is not None},
        "alternateScreen": bool(alternate),
        "applicationCursor": bool(application_cursor),
        "foreground": hex_color(default_fg),
        "background": hex_color(default_bg),
        "lines": lines,
    }


def run(texts, style, column, width):
    foreground, background, flags = style
    return [
        "".join(texts),
        None if foreground is None else hex_color(foreground),
        None if background is None else hex_color(background),
        flags,
        column,
        width,
    ]


# Session service connection -------------------------------------------------


def frame(kind, payload=b""):
    return struct.pack(">IB", len(payload) + 1, kind) + payload


class WireSession:
    """One attachment to an agent's session service."""

    def __init__(self, agent, columns=None, rows=None, timeout=5.0, mode=JOIN):
        self.lock = threading.Lock()
        self.buffer = bytearray()
        self.attachment = None
        self.sequence = 0
        self.first = None
        self.closed = False
        # Set when another phone view is taking this agent, so its "replaced"
        # status is not reported as the Mac taking it back.
        self.superseded = False
        self.history_ids = itertools.count(1)
        self.history_waiters = {}
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        try:
            self.socket.connect(agent["endpoint"])
            launch = fingerprint(
                agent["program"], agent["arguments"], agent["directory"], agent["mode"]
            )
            # No expected session or epoch; the fingerprint must match.
            self.socket.sendall(
                frame(
                    ATTACH,
                    struct.pack(">I", WIRE_VERSION)
                    + launch
                    + bytes([mode])
                    + bytes(32),
                )
            )
            kind, data = self.receive(timeout)
            if kind == STATUS:
                raise GatewayError(status_message(data))
            require(kind == HELLO and len(data) == 52, "Expected hello")
            require(struct.unpack_from(">I", data)[0] == WIRE_VERSION, "Wire mismatch")
            self.attachment = data[4:44]
            kind, data = self.receive(timeout)
            if kind == STATUS:
                raise GatewayError(status_message(data))
            self.first = self.accept_snapshot(kind, data)
            require(self.first is not None, "Expected the first snapshot")
            self.socket.sendall(
                frame(READY, self.attachment + struct.pack(">Q", self.sequence))
            )
            if columns and rows:
                self.resize(columns, rows)
        except BaseException:
            self.close()
            raise

    def accept_snapshot(self, kind, data):
        if kind != SNAPSHOT or len(data) < SNAPSHOT_HEADER:
            return None
        if data[:ATTACHMENT_BYTES] != self.attachment:
            return None
        sequence = struct.unpack_from(">Q", data, ATTACHMENT_BYTES)[0]
        if sequence <= self.sequence:
            return None
        self.sequence = sequence
        return data[SNAPSHOT_HEADER:]

    def receive(self, timeout):
        """One frame, or None when nothing arrives within the timeout."""
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack_from(">I", self.buffer)[0]
                require(1 <= size <= MAX_FRAME, "Invalid frame size")
                if len(self.buffer) >= size + 4:
                    kind, payload = self.buffer[4], bytes(self.buffer[5 : size + 4])
                    del self.buffer[: size + 4]
                    return kind, payload
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(262144)
            except TimeoutError:
                return None
            if not chunk:
                raise EOFError("The session service closed the connection")
            self.buffer.extend(chunk)

    def send(self, kind, payload=b""):
        with self.lock:
            require(not self.closed, "The agent is no longer attached")
            self.socket.sendall(frame(kind, self.attachment + payload))

    def text(self, data):
        self.send(TEXT, data)

    def paste(self, data):
        self.send(PASTE, data)

    def key(self, name, modifiers=0):
        require(name in KEYS, "Unknown key")
        self.send(KEY, bytes([KEYS[name], modifiers & 0x0F]))

    def request_history(self, reference=0, timeout=10.0, newer=False):
        """The archived page before `reference` (0: the newest page), or after it.

        The stream's reader thread delivers the reply (deliver_history).
        """
        request_id = next(self.history_ids)
        waiter = [threading.Event(), None]
        with self.lock:
            self.history_waiters[request_id] = waiter
        try:
            direction = 1 if newer else 0
            self.send(
                HISTORY_REQUEST, struct.pack(">QQB", request_id, reference, direction)
            )
            require(waiter[0].wait(timeout), "History did not answer")
            return waiter[1]
        finally:
            with self.lock:
                self.history_waiters.pop(request_id, None)

    def deliver_history(self, data):
        require(
            len(data) >= 60 and data[:ATTACHMENT_BYTES] == self.attachment,
            "Bad history reply",
        )
        request_id, page_id = struct.unpack_from(">QQ", data, ATTACHMENT_BYTES)
        length = struct.unpack_from(">I", data, 56)[0]
        message = data[60 : 60 + length].decode("utf-8", "replace")
        snapshot = data[60 + length :]
        with self.lock:
            waiter = self.history_waiters.get(request_id)
        if waiter is not None:
            waiter[1] = {
                "page": page_id,
                "message": message,
                "snapshot": snapshot or None,
            }
            waiter[0].set()

    def resize(self, columns, rows):
        require(10 <= columns <= 500 and 3 <= rows <= 300, "Terminal size out of range")
        self.send(RESIZE, struct.pack(">HH", columns, rows))

    def close(self):
        with self.lock:
            self.closed = True
        try:
            self.socket.close()
        except OSError:
            pass


def open_session(agent, columns=None, rows=None):
    """Join the agent's session, or take it over when its service predates joining."""
    try:
        return WireSession(agent, columns, rows, mode=JOIN), True
    except GatewayError as error:
        if not str(error).startswith("rejected"):
            raise
    return WireSession(agent, columns, rows, mode=DISCOVER), False


def status_message(data):
    code = data[0] if data else 0
    text = data[1:].decode("utf-8", "replace") if len(data) > 1 else ""
    return f"{STATUS_NAMES.get(code, 'status')}: {text}".strip()


# Tailnet identity -----------------------------------------------------------


class TailnetAuth:
    """Admit only the Mac owner's iOS devices and this Mac itself."""

    def __init__(self, tailscale="tailscale", allow_local=False, runner=None):
        self.tailscale = tailscale
        self.allow_local = allow_local
        self.runner = runner or self._run
        self.cache = {}
        self.lock = threading.Lock()
        status = self.runner([self.tailscale, "status", "--json"])
        own = status["Self"]
        self.owner = status["User"][str(own["UserID"])]["LoginName"]
        self.addresses = set(own.get("TailscaleIPs") or [])
        self.dns_name = str(own.get("DNSName", "")).rstrip(".")

    @staticmethod
    def _run(command):
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=5, check=True
        )
        return json.loads(result.stdout)

    def allowed(self, address):
        if address in self.addresses:
            return True
        if self.allow_local and address in ("127.0.0.1", "::1"):
            return True
        now = time.monotonic()
        with self.lock:
            cached = self.cache.get(address)
            if cached and cached[1] > now:
                return cached[0]
        try:
            who = self.runner([self.tailscale, "whois", "--json", address])
            login = who["UserProfile"]["LoginName"]
            system = who["Node"]["Hostinfo"].get("OS", "")
            verdict = login == self.owner and system == "iOS"
        except (subprocess.SubprocessError, OSError, KeyError, ValueError, TypeError):
            return False
        with self.lock:
            self.cache[address] = (verdict, now + 300)
        return verdict

    def hosts(self, port):
        names = set(self.addresses) | {self.dns_name, self.dns_name.split(".")[0]}
        if self.allow_local:
            names |= {"127.0.0.1", "localhost"}
        allowed = set()
        for name in names:
            if not name:
                continue
            allowed.add(f"[{name}]:{port}" if ":" in name else f"{name}:{port}")
        return allowed


# HTTP -----------------------------------------------------------------------


class Gateway:
    def __init__(self, registry, auth, port):
        self.registry = Path(registry)
        self.auth = auth
        self.port = port
        self.lock = threading.Lock()
        self.sessions = {}

    def workspace(self):
        return load_workspace(self.registry)

    def agent(self, identifier):
        for agent in self.workspace()["agents"]:
            if agent["id"] == identifier:
                return agent
        return None

    def supersede(self, identifier, value=True):
        with self.lock:
            previous = self.sessions.get(identifier)
        if previous is not None:
            previous.superseded = value

    def adopt(self, identifier, session):
        """Make this the agent's only phone attachment."""
        with self.lock:
            previous = self.sessions.get(identifier)
            self.sessions[identifier] = session
        if previous is not None:
            previous.close()

    def release(self, identifier, session):
        with self.lock:
            if self.sessions.get(identifier) is session:
                del self.sessions[identifier]
        session.close()

    def session(self, identifier):
        with self.lock:
            return self.sessions.get(identifier)


def agent_route(parts, action):
    return len(parts) == 4 and parts[:2] == ["api", "agents"] and parts[3] == action


class Handler(BaseHTTPRequestHandler):
    server_version = "lapis-remote"
    gateway: Gateway

    def log_message(self, format, *args):
        # Paths and status only; never request bodies.
        sys.stderr.write(
            f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {self.client_address[0]} "
            f"{format % args}\n"
        )

    def admitted(self):
        gateway = self.gateway
        if self.headers.get("Origin") is not None:
            self.fail(HTTPStatus.FORBIDDEN, "Browsers are not served")
            return False
        if self.headers.get(CLIENT_HEADER) is None:
            self.fail(HTTPStatus.FORBIDDEN, "Missing client header")
            return False
        if self.headers.get("Host", "") not in gateway.auth.hosts(gateway.port):
            self.fail(HTTPStatus.MISDIRECTED_REQUEST, "Unknown host")
            return False
        if not gateway.auth.allowed(self.client_address[0]):
            self.fail(HTTPStatus.FORBIDDEN, "Not one of this Mac owner's devices")
            return False
        return True

    def reply(self, status, body):
        data = json.dumps(body, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def fail(self, status, message):
        self.reply(status, {"error": message})

    def route(self):
        parts = [part for part in urlsplit(self.path).path.split("/") if part]
        return parts

    def do_GET(self):
        if not self.admitted():
            return
        parts = self.route()
        if parts == ["api", "health"]:
            self.reply(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "version": GATEWAY_VERSION,
                    "host": self.gateway.auth.dns_name,
                },
            )
        elif parts == ["api", "agents"]:
            self.list_agents()
        elif agent_route(parts, "stream"):
            self.stream(parts[2])
        elif agent_route(parts, "history"):
            self.history(parts[2])
        else:
            self.fail(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self):
        if not self.admitted():
            return
        parts = self.route()
        if agent_route(parts, "input"):
            self.input(parts[2])
        elif parts == ["api", "captures"]:
            self.capture()
        else:
            self.fail(HTTPStatus.NOT_FOUND, "Not found")

    def list_agents(self):
        try:
            workspace = self.gateway.workspace()
        except (OSError, ValueError, GatewayError) as error:
            self.fail(
                HTTPStatus.SERVICE_UNAVAILABLE, f"Cannot read the workspace: {error}"
            )
            return
        categories = []
        for category in workspace["categories"]:
            agents = [
                {
                    "id": agent["id"],
                    "title": agent["title"],
                    "harness": agent["harness"],
                    "directory": agent["directory"],
                    "machine": display_place(agent)[0],
                    "place": display_place(agent)[1],
                    "running": service_answers(agent["endpoint"]),
                    "onPhone": self.gateway.session(agent["id"]) is not None,
                }
                for agent in workspace["agents"]
                if agent["category"] == category["id"]
            ]
            categories.append({**category, "agents": agents})
        self.reply(
            HTTPStatus.OK,
            {"categories": categories, "activeCategory": workspace["activeCategory"]},
        )

    def event(self, name, body):
        data = json.dumps(body, separators=(",", ":"))
        self.wfile.write(f"event: {name}\ndata: {data}\n\n".encode())
        self.wfile.flush()

    def stream(self, identifier):
        query = parse_qs(urlsplit(self.path).query)
        agent = self.gateway.agent(identifier)
        if agent is None:
            self.fail(HTTPStatus.NOT_FOUND, "No such agent")
            return
        try:
            columns = int(query.get("columns", ["0"])[0])
            rows = int(query.get("rows", ["0"])[0])
        except ValueError:
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid terminal size")
            return
        self.gateway.supersede(identifier)
        try:
            session, shared = open_session(agent, columns or None, rows or None)
        except (OSError, EOFError, GatewayError) as error:
            self.gateway.supersede(identifier, False)
            self.fail(HTTPStatus.BAD_GATEWAY, f"Cannot attach: {error}")
            return
        self.gateway.adopt(identifier, session)
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.event("attached", {"shared": shared})
            self.pump(session)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.gateway.release(identifier, session)

    def phone_left(self):
        """True once the phone has closed its end of the stream."""
        readable, _, _ = select.select([self.connection], [], [], 0)
        if not readable:
            return False
        try:
            return self.connection.recv(1, socket.MSG_PEEK) == b""
        except OSError:
            return True

    def pump(self, session):
        """Forward the newest screen at most every 50 ms until either side leaves."""
        self.event("frame", render_snapshot(session.first))
        latest, last_sent, last_ping = None, time.monotonic(), time.monotonic()
        while not session.closed:
            if self.phone_left():
                return
            wait = 0.05 if latest is not None else 1.0
            try:
                received = session.receive(wait)
            except (EOFError, OSError, GatewayError) as error:
                if session.closed:
                    self.event(
                        "status", {"state": "released", "message": "Opened elsewhere"}
                    )
                else:
                    self.event(
                        "status", {"state": "disconnected", "message": str(error)}
                    )
                return
            now = time.monotonic()
            if received is not None:
                kind, data = received
                if kind == STATUS:
                    code = data[0] if data else 0
                    state = STATUS_NAMES.get(code, "status")
                    if state == "replaced" and session.superseded:
                        state = "released"
                    self.event(
                        "status", {"state": state, "message": status_message(data)}
                    )
                    return
                if kind == HISTORY_PAGE:
                    session.deliver_history(data)
                    continue
                snapshot = session.accept_snapshot(kind, data)
                if snapshot is not None:
                    latest = snapshot
            if latest is not None and now - last_sent >= 0.05:
                self.event("frame", render_snapshot(latest))
                latest, last_sent = None, now
            if now - last_ping >= 10:
                self.wfile.write(b": ping\n\n")
                self.wfile.flush()
                last_ping = now
        self.event("status", {"state": "released", "message": "Opened elsewhere"})

    def history(self, identifier):
        """An archived page of the agent's output: ?before=N (0: newest) or ?after=N."""
        session = self.gateway.session(identifier)
        if session is None:
            self.fail(HTTPStatus.CONFLICT, "This agent is not open on the phone")
            return
        try:
            query = parse_qs(urlsplit(self.path).query)
            if "after" in query:
                after = int(query["after"][0])
                require(after > 0, "after needs a page")
                reply = session.request_history(after, newer=True)
            else:
                reply = session.request_history(
                    max(0, int(query.get("before", ["0"])[0]))
                )
        except (ValueError, GatewayError, OSError) as error:
            self.fail(HTTPStatus.BAD_GATEWAY, f"History unavailable: {error}")
            return
        body = {
            "page": reply["page"],
            "message": reply["message"],
            "end": reply["page"] == 0 and "No more" in reply["message"],
            "busy": reply["page"] == 0 and "busy" in reply["message"],
        }
        if reply["snapshot"]:
            page = render_snapshot(reply["snapshot"], show_cursor=False)
            body.update({"columns": page["columns"], "lines": page["lines"]})
        self.log_message(
            "history page %s, %s rows", body["page"], len(body.get("lines", []))
        )
        self.reply(HTTPStatus.OK, body)

    def capture(self):
        """Save what the phone shows, to debug rendering from the Mac."""
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_CAPTURE:
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid capture size")
            return
        try:
            body = json.loads(self.rfile.read(length))
            image = base64.b64decode(body.pop("png"), validate=True)
            require(image.startswith(b"\x89PNG\r\n\x1a\n"), "Not a PNG")
        except (ValueError, KeyError, TypeError, GatewayError) as error:
            self.fail(HTTPStatus.BAD_REQUEST, f"Invalid capture: {error}")
            return
        directory = self.gateway.registry.parent / "phone-captures"
        directory.mkdir(mode=0o700, exist_ok=True)
        name = time.strftime("%Y%m%d-%H%M%S")
        for suffix, data in (
            (".png", image),
            (".json", json.dumps(body, indent=1).encode()),
        ):
            flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
            with os.fdopen(
                os.open(directory / (name + suffix), flags, 0o600), "wb"
            ) as out:
                out.write(data)
        self.reply(HTTPStatus.OK, {"saved": name})

    def input(self, identifier):
        session = self.gateway.session(identifier)
        if session is None:
            self.fail(HTTPStatus.CONFLICT, "This agent is not open on the phone")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_INPUT * 2:
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid input size")
            return
        try:
            body = json.loads(self.rfile.read(length))
            require(isinstance(body, dict), "Input must be an object")
            if "resize" in body:
                columns, rows = body["resize"]
                session.resize(int(columns), int(rows))
            if "paste" in body:
                data = str(body["paste"]).encode("utf-8")
                require(len(data) <= MAX_INPUT, "Paste too large")
                session.paste(data)
            if "text" in body:
                data = str(body["text"]).encode("utf-8")
                require(len(data) <= MAX_INPUT, "Text too large")
                session.text(data)
            if "key" in body:
                if body.get("submit") or "paste" in body:
                    time.sleep(0.12)  # let a paste settle before Enter
                session.key(str(body["key"]), int(body.get("modifiers", 0)))
        except (ValueError, TypeError, KeyError, GatewayError, OSError) as error:
            self.fail(HTTPStatus.BAD_REQUEST, str(error))
            return
        self.reply(HTTPStatus.OK, {"ok": True})


def tailscale_address(tailscale):
    result = subprocess.run(
        [tailscale, "ip", "-4"], capture_output=True, text=True, timeout=5, check=True
    )
    return result.stdout.split()[0]


def serve(args):
    while True:
        try:
            auth = TailnetAuth(args.tailscale, args.allow_local)
            bind = args.bind or tailscale_address(args.tailscale)
            server = ThreadingHTTPServer((bind, args.port), Handler)
            break
        except (OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
            # Tailscale may still be starting after login.
            sys.stderr.write(f"waiting for Tailscale: {error}\n")
            time.sleep(5)
    server.daemon_threads = True
    Handler.gateway = Gateway(args.registry, auth, args.port)
    sys.stderr.write(f"lapis remote on {bind}:{args.port} for {auth.dns_name}\n")
    server.serve_forever()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--registry", default=str(ROOT / "runtime" / "workspace.json"))
    parser.add_argument("--bind", help="address; defaults to this Mac's Tailscale IPv4")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--tailscale", default="tailscale")
    parser.add_argument(
        "--allow-local",
        action="store_true",
        help="also admit 127.0.0.1 without whois (tests only)",
    )
    serve(parser.parse_args(argv))


if __name__ == "__main__":
    os.umask(0o077)
    main()
