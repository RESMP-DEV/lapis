#!/usr/bin/env python3
"""Serve the lapis workspace's agents to the lapis iPhone app.

The gateway reads the desktop's workspace registry and attaches to an
agent's session service only while the phone shows that agent. It joins the
session beside the desktop, so both show the same screen and either can type;
the device in use sets the terminal size, and the desktop's returns when the
phone leaves. The phone can also start an agent: the gateway asks the lapis
process that owns the workspace (the window, or the windowless host after a
restart) over its workspace-control socket, and the agent opens as a new tab
in the chosen category. A service started before joining
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
import collections
import concurrent.futures
import glob
import gzip
import hashlib
import inspect
import itertools
import json
import os
import re
import select
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
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
# A turn of the wheel for a full-screen program (added within v6). Only a
# service whose snapshots set ACCEPTS_WHEEL in the alternate-screen byte takes
# it; one from before drops the connection on it.
WHEEL = 16
ALTERNATE_OFFSET, ACCEPTS_WHEEL = 21, 2
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
MAX_CELLS = 32768  # wire::max_cells

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
MAX_REQUEST = 8 * 1024
CONTROL_NAME = "workspace-control.sock"

# Run style bits sent to the phone.
BOLD, ITALIC, FAINT, UNDERLINE, STRIKE, CURSOR = 1, 2, 4, 8, 16, 32


class GatewayError(RuntimeError):
    pass


class DesktopUnavailable(GatewayError):
    """No lapis window or windowless host owns the workspace."""


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


def load_terminals(registry):
    """The desktop's quick-command terminals (terminals.json beside the
    workspace): plain shells, one per machine, reached like agents. Only this
    folder's own endpoints are reachable."""
    folder = Path(registry).absolute().parent
    try:
        data = json.loads((folder / "terminals.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    terminals = []
    for entry in data.get("terminals", []) if isinstance(data, dict) else []:
        if not isinstance(entry, dict):
            continue
        identifier = str(entry.get("id", ""))
        endpoint = Path(str(entry.get("endpoint", "")))
        if (
            not identifier.startswith("terminal-")
            or endpoint.name != identifier + ".sock"
            or endpoint.parent.resolve() != folder.resolve()
        ):
            continue
        machine = str(entry.get("machine", ""))
        terminals.append(
            {
                "id": identifier,
                "title": machine or "This Mac",
                "category": "",
                "harness": "shell",
                "machine": machine,
                "directory": str(entry.get("directory", "")),
                "program": str(entry.get("program", "")),
                "arguments": [str(item) for item in entry.get("arguments", [])],
                "mode": "",
                "endpoint": str(endpoint),
            }
        )
    return terminals


def position(value):
    """A JSON index: a whole number of at least zero, and not true or false."""
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def desktop_request(registry, request, timeout=15.0):
    """One request to the lapis process that owns the workspace (a window, or
    the windowless host): a JSON line out, a JSON line back."""
    path = Path(registry).absolute().parent / CONTROL_NAME
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(timeout)
    try:
        try:
            connection.connect(str(path))
        except (FileNotFoundError, ConnectionRefusedError) as error:
            raise DesktopUnavailable(
                "lapis is not running on the Mac. Open it there, or install the "
                "login helper (scripts/restore_at_login.py) so it keeps serving."
            ) from error
        connection.sendall(json.dumps({"version": 1, **request}).encode() + b"\n")
        data = b""
        while not data.endswith(b"\n") and len(data) < MAX_REQUEST * 8:
            chunk = connection.recv(65536)
            if not chunk:
                break
            data += chunk
    finally:
        connection.close()
    try:
        answer = json.loads(data)
    except ValueError as error:
        raise GatewayError("lapis on the Mac gave no answer") from error
    require(isinstance(answer, dict), "lapis on the Mac gave no answer")
    if not answer.get("ok"):
        raise GatewayError(str(answer.get("error") or "lapis on the Mac refused"))
    return answer


# Folders -------------------------------------------------------------------

# Listed but not descended into: generated, vendored or system trees.
NO_DESCENT = {
    "node_modules",
    ".git",
    "__pycache__",
    ".venv",
    "venv",
    ".tox",
    "target",
    "build",
    "dist",
    ".next",
    ".cache",
    "DerivedData",
    ".Trash",
    "Pods",
    ".gradle",
    ".npm",
    ".cargo",
    ".rustup",
    "site-packages",
    ".pnpm-store",
}
# macOS packages look like folders but are opened as one thing.
PACKAGES = (
    ".app",
    ".bundle",
    ".framework",
    ".photoslibrary",
    ".musiclibrary",
    ".xcodeproj",
    ".xcworkspace",
    ".xcassets",
    ".sparsebundle",
    ".dSYM",
    ".lproj",
)
FOLDER_DEPTH = 4
MAX_FOLDERS = 30000
SESSION_NAME = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.jsonl$"
)


def scan_folders(home, depth=FOLDER_DEPTH, limit=MAX_FOLDERS):
    """Folders under home, relative to it and sorted; breadth first, so a cap
    drops the deepest. Hidden folders are listed, and only home's are opened."""
    found = []
    queue = collections.deque([("", 0)])
    while queue and len(found) < limit:
        relative, level = queue.popleft()
        try:
            entries = list(os.scandir(os.path.join(home, relative)))
        except OSError:
            continue
        for entry in entries:
            try:
                if not entry.is_dir(follow_symlinks=False):
                    continue
            except OSError:
                continue
            path = f"{relative}/{entry.name}" if relative else entry.name
            found.append(path)
            if len(found) >= limit:
                break
            if (
                level + 1 < depth
                and entry.name not in NO_DESCENT
                and not entry.name.endswith(PACKAGES)
                and not (entry.name.startswith(".") and level > 0)
                and path != "Library"
            ):
                queue.append((path, level + 1))
    return sorted(found)


def first_json_line(path, limit=1 << 20):
    with open(path, "rb") as stream:
        return json.loads(stream.readline(limit))


def codex_cwd(path):
    """The folder of a Codex rollout's interactive main thread, else None."""
    try:
        meta = first_json_line(path).get("payload", {})
    except (OSError, ValueError, AttributeError):
        return None
    source = meta.get("source")
    if isinstance(source, dict) or meta.get("parent_thread_id") or source == "exec":
        return None
    cwd = meta.get("cwd")
    return cwd if isinstance(cwd, str) else None


def claude_cwd(session):
    """The folder of an interactive Claude Code session, else None. Sessions
    run with -p or the SDK (entrypoint sdk-*) are automation, not agents
    someone opened."""
    cwd = entry = None
    try:
        with open(session, "rb") as stream:
            for line in itertools.islice(stream, 40):
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(record, dict):
                    continue
                cwd = cwd or record.get("cwd")
                entry = entry or record.get("entrypoint")
                if cwd and entry:
                    break
    except OSError:
        return None
    return cwd if entry == "cli" and isinstance(cwd, str) else None


def typed_title(text):
    """One line of what someone typed; the CLIs' own wrappers are not."""
    text = str(text or "").strip()
    if not text or text[0] in "<#" or text.startswith("Caveat:"):
        return ""
    text = " ".join(text.split())
    return text if len(text) <= 140 else text[:139] + "\u2026"


def claude_title(session):
    """Claude Code's newest title for a session, else its first typed message."""
    first = title = ""
    try:
        with open(session, "rb") as stream:
            for line in itertools.islice(stream, 300):
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(record, dict):
                    continue
                if record.get("type") == "ai-title":
                    title = typed_title(record.get("aiTitle"))
                elif (
                    record.get("type") == "user"
                    and not first
                    and not record.get("isMeta")
                ):
                    content = (record.get("message") or {}).get("content")
                    if isinstance(content, list):
                        content = next(
                            (
                                part.get("text")
                                for part in content
                                if isinstance(part, dict) and part.get("type") == "text"
                            ),
                            "",
                        )
                    first = typed_title(content)
            size = os.fstat(stream.fileno()).st_size
            stream.seek(max(0, size - (128 << 10)))
            for line in stream.read().split(b"\n"):
                if b'"ai-title"' in line:
                    try:
                        record = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(record, dict) and record.get("type") == "ai-title":
                        title = typed_title(record.get("aiTitle")) or title
    except OSError:
        return ""
    return title or first


def codex_title(path):
    """A Codex rollout's first typed message (its thread name is applied later)."""
    try:
        with open(path, "rb") as stream:
            for line in itertools.islice(stream, 200):
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                payload = record.get("payload") if isinstance(record, dict) else None
                if (
                    record.get("type") != "response_item"
                    or not isinstance(payload, dict)
                    or payload.get("type") != "message"
                    or payload.get("role") != "user"
                ):
                    continue
                for part in payload.get("content") or []:
                    if isinstance(part, dict):
                        text = typed_title(part.get("text"))
                        if text:
                            return text
    except OSError:
        pass
    return ""


def codex_thread_names(codex_home):
    names = {}
    try:
        with open(Path(codex_home) / "session_index.jsonl", "rb") as stream:
            for line in stream:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if (
                    isinstance(record, dict)
                    and record.get("id")
                    and record.get("thread_name")
                ):
                    names[str(record["id"])] = " ".join(
                        str(record["thread_name"]).split()
                    )[:140]
    except OSError:
        pass
    return names


class AgentHistory:
    """How many agents were started in each folder and how recently: Codex
    rollouts (interactive main threads), interactive Claude Code sessions and
    the workspace's current agents. Each conversation also adds to its
    folder's heat, halving every two weeks, so recent and frequent work both
    count. What a file says is remembered by its size and time, so later
    passes read only new or changed files. The root folder is never a
    project."""

    HALF_LIFE_DAYS = 14

    def __init__(self, codex_home, claude_home):
        self.codex_home = Path(codex_home)
        self.claude_home = Path(claude_home)
        self.codex = {}
        self.claude = {}
        self.heat = collections.Counter()
        self.conversations = []

    def counts(self, registry):
        counts = collections.Counter()
        heat = collections.Counter()
        conversations = []
        now = time.time()

        def note(cwd, when, conversation):
            counts[cwd] += 1
            heat[cwd] += 0.5 ** (max(0.0, now - when) / 86400 / self.HALF_LIFE_DAYS)
            conversations.append(dict(conversation, directory=cwd, modified=when))

        def remembered(cache, path, read):
            try:
                stat = path.stat()
            except OSError:
                return None, 0
            mark = (stat.st_size, stat.st_mtime)
            cached = cache.get(str(path))
            entry = (
                cached
                if cached is not None and cached[0] == mark
                else (mark, read(path))
            )
            return entry, stat.st_mtime

        def read_rollout(path):
            cwd = codex_cwd(path)
            if not cwd:
                return None
            # A rollout without an id still counts for its folder; it only
            # cannot be resumed, so the list leaves it out.
            try:
                identifier = str(first_json_line(path)["payload"].get("id") or "")
            except (OSError, ValueError, KeyError, TypeError, AttributeError):
                identifier = ""
            return {
                "harness": "codex",
                "id": identifier,
                "cwd": cwd,
                "title": codex_title(path),
            }

        def read_session(path):
            cwd = claude_cwd(path)
            if not cwd:
                return None
            return {
                "harness": "claude",
                "id": path.stem,
                "cwd": cwd,
                "title": claude_title(path),
            }

        seen = {}
        for rollout in (self.codex_home / "sessions").glob("*/*/*/rollout-*.jsonl"):
            entry, when = remembered(self.codex, rollout, read_rollout)
            if entry is None:
                continue
            seen[str(rollout)] = entry
            if entry[1]:
                note(entry[1]["cwd"], when, entry[1])
        self.codex = seen
        projects = self.claude_home / "projects"
        seen = {}
        for project in projects.iterdir() if projects.is_dir() else []:
            try:
                sessions = [p for p in project.iterdir() if SESSION_NAME.match(p.name)]
            except OSError:
                continue
            for session in sessions:
                entry, when = remembered(self.claude, session, read_session)
                if entry is None:
                    continue
                seen[str(session)] = entry
                if entry[1]:
                    note(entry[1]["cwd"], when, entry[1])
        self.claude = seen
        if registry is not None:
            try:
                for agent in load_workspace(registry)["agents"]:
                    if agent["directory"] and not remote_machine(agent):
                        counts[agent["directory"]] += 1
                        heat[agent["directory"]] += 1
            except (OSError, ValueError, GatewayError):
                pass
        counts.pop("/", None)
        heat.pop("/", None)
        names = codex_thread_names(self.codex_home)
        conversations = [item for item in conversations if item["id"]]
        for conversation in conversations:
            if conversation["harness"] == "codex" and names.get(conversation["id"]):
                conversation["title"] = names[conversation["id"]]
            conversation.pop("cwd", None)
        conversations.sort(key=lambda item: item["modified"], reverse=True)
        self.heat = heat
        self.conversations = conversations
        return counts


# Where each agent CLI is found: PATH, then the per-user install folders the
# desktop also searches.
HARNESS_COMMANDS = (
    "claude",
    "codex",
    "opencode",
    "grok",
    "omp",
    "agy",
    "kimi",
)
HARNESS_FOLDERS = (
    "~/.local/bin",
    "~/.bun/bin",
    "~/.grok/bin",
    "~/.kimi-code/bin",
    "~/.opencode/bin",
    "~/bin",
    "/opt/homebrew/bin",
    "/usr/local/bin",
)


def harness_paths():
    search = os.pathsep.join(
        [os.environ.get("PATH", "")] + [os.path.expanduser(p) for p in HARNESS_FOLDERS]
    )
    found = {}
    for command in HARNESS_COMMANDS:
        path = shutil.which(command, path=search)
        if path:
            found[command] = path
    return found


def phone_path(path, home):
    """Relative to home when under it (the phone shows ~/...), else absolute."""
    path = os.path.normpath(path)
    if path == home:
        return ""
    if path.startswith(home + "/"):
        return path[len(home) + 1 :]
    return path


class FolderIndex:
    """This Mac's folders and the most used ones, rebuilt in the background so
    the phone can browse and search without asking again for each step."""

    REFRESH = 600
    STALE = 60

    def __init__(self, registry, home=None, codex_home=None, claude_home=None):
        self.registry = registry
        self.home = os.path.normpath(str(home or Path.home()))
        self.history = AgentHistory(
            codex_home or os.environ.get("CODEX_HOME") or Path.home() / ".codex",
            claude_home
            or os.environ.get("CLAUDE_CONFIG_DIR")
            or Path.home() / ".claude",
        )
        self.lock = threading.Lock()
        self.ready = threading.Event()
        self.wake = threading.Event()
        self.built = 0.0
        self.payload = None
        self.compressed = b""
        self.thread = None

    def start(self):
        with self.lock:
            if self.thread is None:
                self.thread = threading.Thread(target=self.run, daemon=True)
                self.thread.start()

    def run(self):
        while True:
            try:
                self.build()
            except Exception as error:  # a failed pass keeps the previous index
                sys.stderr.write(f"folder index: {error}\n")
            self.ready.set()
            self.wake.wait(self.REFRESH)
            self.wake.clear()

    def build(self):
        body = folder_report(self.home, self.history, self.registry)
        version = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()[
            :16
        ]
        payload = {"version": version, **body}
        compressed = gzip.compress(json.dumps(payload, separators=(",", ":")).encode())
        with self.lock:
            self.payload, self.compressed, self.built = (
                payload,
                compressed,
                time.monotonic(),
            )

    def current(self, timeout=20.0):
        """The newest index; an old one is served while a new one is built."""
        self.start()
        self.ready.wait(timeout)
        with self.lock:
            if self.payload is not None and time.monotonic() - self.built > self.STALE:
                self.wake.set()
            return self.payload, self.compressed

    def refresh(self):
        self.wake.set()


def folder_report(home, history, registry=None, harnesses=False):
    """What the phone needs to pick a folder on one machine."""
    counts = history.counts(registry)
    frequent = []
    for path, count in counts.most_common():
        if len(frequent) >= 12:
            break
        if os.path.isdir(path):
            frequent.append({"path": phone_path(path, home), "count": count})
    # Folder heat, for the phone to put the most active folders first.
    activity = {
        phone_path(path, home): round(score, 4)
        for path, score in history.heat.items()
        if score >= 0.001 and os.path.isdir(path)
    }
    report = {
        "home": home,
        "folders": scan_folders(home),
        "frequent": frequent,
        "activity": activity,
    }
    if harnesses:
        report["harnesses"] = harness_paths()
    return report


# The same scan run on another machine over ssh: these definitions, then a
# line of JSON after a marker (login shells may print first).
REMOTE_MARKER = "LAPIS-FOLDERS "
REMOTE_SCRIPT = "\n".join(
    [
        "import collections, itertools, json, os, re, shutil, time",
        "from pathlib import Path",
        f"NO_DESCENT = {sorted(NO_DESCENT)!r}",
        f"PACKAGES = {PACKAGES!r}",
        f"FOLDER_DEPTH = {FOLDER_DEPTH!r}",
        f"MAX_FOLDERS = {MAX_FOLDERS!r}",
        f"SESSION_NAME = re.compile({SESSION_NAME.pattern!r})",
        f"HARNESS_COMMANDS = {HARNESS_COMMANDS!r}",
        f"HARNESS_FOLDERS = {HARNESS_FOLDERS!r}",
    ]
    + [
        inspect.getsource(item)
        for item in (
            scan_folders,
            first_json_line,
            codex_cwd,
            claude_cwd,
            typed_title,
            claude_title,
            codex_title,
            codex_thread_names,
            AgentHistory,
            harness_paths,
            phone_path,
            folder_report,
        )
    ]
    + [
        "home = os.path.normpath(str(Path.home()))",
        "history = AgentHistory(os.environ.get('CODEX_HOME') or Path.home() / '.codex',"
        " os.environ.get('CLAUDE_CONFIG_DIR') or Path.home() / '.claude')",
        f"print({REMOTE_MARKER!r} + json.dumps(folder_report(home, history, None, True)))",
    ]
)


def remote_folder_report(machine, timeout=60):
    """folder_report on `machine`, through the user's ssh setup. Keys only:
    BatchMode never waits on a password prompt."""
    result = subprocess.run(
        [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=8",
            "--",
            machine,
            # An interactive login shell, so PATH matches the user's terminal.
            'exec "${SHELL:-/bin/sh}" -lic "python3 -"',
        ],
        input=REMOTE_SCRIPT,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    for line in reversed(result.stdout.splitlines()):
        if line.startswith(REMOTE_MARKER):
            return json.loads(line[len(REMOTE_MARKER) :])
    detail = (result.stderr.strip().splitlines() or ["no answer"])[-1]
    raise GatewayError(f"{machine}: {detail[:200]}")


# Machines ------------------------------------------------------------------

MACHINE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")


def ssh_config_hosts(path=None, seen=None):
    """Named hosts in the user's ssh config and its includes (no patterns)."""
    path = Path(path or Path.home() / ".ssh" / "config")
    seen = seen if seen is not None else set()
    if str(path) in seen:
        return []
    seen.add(str(path))
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return []
    hosts = []
    for line in text.splitlines():
        try:
            words = shlex.split(line, comments=True)
        except ValueError:
            continue
        if not words:
            continue
        key = words[0].lower()
        if key == "include":
            for pattern in words[1:]:
                pattern = os.path.expanduser(pattern)
                if not os.path.isabs(pattern):
                    pattern = str(Path.home() / ".ssh" / pattern)
                for match in sorted(glob.glob(pattern)):
                    hosts += ssh_config_hosts(match, seen)
        elif key == "host":
            hosts += [
                name
                for name in words[1:]
                if not set(name) & set("*?!") and MACHINE_NAME.match(name)
            ]
    return hosts


def ssh_target(words):
    """The host of an ssh, mosh or et command line, without user@."""
    arguments = iter(words[1:])
    for argument in arguments:
        if argument == "--":
            argument = next(arguments, "")
        elif argument.startswith("-"):
            if len(argument) == 2 and argument[1] in SSH_VALUE_OPTIONS:
                next(arguments, None)
            continue
        host = argument.rsplit("@", 1)[-1]
        return host if MACHINE_NAME.match(host) else ""
    return ""


def history_hosts(paths=None):
    """How often each host was reached with ssh, mosh or et in shell history."""
    counts = collections.Counter()
    paths = paths or [Path.home() / ".zsh_history", Path.home() / ".bash_history"]
    for path in paths:
        try:
            text = Path(path).read_bytes().decode("utf-8", "replace")
        except OSError:
            continue
        for line in text.splitlines():
            if line.startswith(": ") and ";" in line:  # zsh extended history
                line = line.split(";", 1)[1]
            try:
                words = shlex.split(line)
            except ValueError:
                words = line.split()
            if words and words[0] in ("ssh", "mosh", "et", "autossh"):
                host = ssh_target(words)
                if host:
                    counts[host] += 1
    return counts


def reachable(machine, timeout=1.5):
    """Whether the host (or its first jump host) accepts TCP: no login, no key
    use, so a hardware key never asks for a touch."""
    try:
        resolved = subprocess.run(
            ["ssh", "-G", "--", machine], capture_output=True, text=True, timeout=5
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    settings = dict(line.split(" ", 1) for line in resolved.splitlines() if " " in line)
    jump = settings.get("proxyjump", "none")
    if jump != "none":
        return reachable(jump.split(",")[0].rsplit("@", 1)[-1].split(":")[0], timeout)
    if settings.get("proxycommand", "none") != "none":
        return True  # cannot tell without running it
    try:
        with socket.create_connection(
            (settings.get("hostname", machine), int(settings.get("port", "22"))),
            timeout,
        ):
            return True
    except (OSError, ValueError):
        return False


class MachineList:
    """ssh hosts the phone can start agents on, most used first."""

    STALE = 60

    def __init__(self, registry, config=None, histories=None):
        self.registry = registry
        self.config = config
        self.histories = histories
        self.lock = threading.Lock()
        self.machines = None
        self.built = 0.0
        self.building = False

    def build(self):
        counts = history_hosts(self.histories)
        try:
            for agent in load_workspace(self.registry)["agents"]:
                host = remote_machine(agent)
                if host:
                    counts[host] += 5  # an agent kept in the workspace counts more
        except (OSError, ValueError, GatewayError):
            pass
        configured = ssh_config_hosts(self.config)
        seen = [h for h, n in counts.most_common(24) if n >= 3 and h not in configured]
        names = list(dict.fromkeys(configured + seen))
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            online = dict(zip(names, pool.map(reachable, names)))
        # Hosts known only from history (often gone cloud machines) are shown
        # while they answer; reachable ones come first, most used first.
        names = [n for n in names if n in configured or online[n]]
        names.sort(key=lambda name: (not online[name], -counts[name], name))
        machines = [
            {"name": name, "uses": counts[name], "available": online[name]}
            for name in names
        ]
        with self.lock:
            self.machines, self.built, self.building = machines, time.monotonic(), False

    def current(self, timeout=8.0):
        with self.lock:
            stale = time.monotonic() - self.built > self.STALE
            start = stale and not self.building
            if start:
                self.building = True
        if start:
            thread = threading.Thread(target=self.build, daemon=True)
            thread.start()
            if self.machines is None:
                thread.join(timeout)
        with self.lock:
            return self.machines


class RemoteFolders:
    """Folder reports for other machines, fetched over ssh when first asked
    and again when older than ten minutes; the last one is served meanwhile."""

    STALE = 600

    def __init__(self):
        self.lock = threading.Lock()
        self.reports = {}  # machine -> (payload, compressed, built)
        self.building = set()
        self.errors = {}

    def build(self, machine):
        try:
            report = remote_folder_report(machine)
            version = hashlib.sha256(
                json.dumps(report, sort_keys=True).encode()
            ).hexdigest()[:16]
            payload = {"version": version, "machine": machine, **report}
            compressed = gzip.compress(
                json.dumps(payload, separators=(",", ":")).encode()
            )
            with self.lock:
                self.reports[machine] = (payload, compressed, time.monotonic())
                self.errors.pop(machine, None)
        except (OSError, ValueError, subprocess.SubprocessError, GatewayError) as error:
            with self.lock:
                self.errors[machine] = str(error)
        finally:
            with self.lock:
                self.building.discard(machine)

    def current(self, machine, timeout=30.0):
        with self.lock:
            report = self.reports.get(machine)
            stale = report is None or time.monotonic() - report[2] > self.STALE
            start = stale and machine not in self.building
            if start:
                self.building.add(machine)
        if start:
            thread = threading.Thread(target=self.build, args=(machine,), daemon=True)
            thread.start()
            if report is None:
                thread.join(timeout)
        with self.lock:
            report = self.reports.get(machine)
            return (
                (report[0], report[1]) if report else (None, self.errors.get(machine))
            )

    def program(self, machine, harness):
        with self.lock:
            report = self.reports.get(machine)
        return report[0].get("harnesses", {}).get(harness, "") if report else ""


class KeepAwake:
    """Keeps this Mac from sleeping on power while lapis.json's keepAwake is on
    (the default), so the phone can reach it. caffeinate -s only holds on AC
    power and exits with the gateway; the display still sleeps."""

    def __init__(self, config):
        self.config = Path(config)
        self.process = None
        self.stamp = None

    def wanted(self):
        try:
            return bool(
                json.loads(self.config.read_text(encoding="utf-8")).get(
                    "keepAwake", True
                )
            )
        except (OSError, ValueError, AttributeError):
            return True

    def apply(self):
        try:
            stamp = self.config.stat().st_mtime_ns
        except OSError:
            stamp = None
        running = self.process is not None and self.process.poll() is None
        if stamp == self.stamp and running == self.wanted():
            return
        self.stamp = stamp
        if self.wanted() and not running:
            caffeinate = shutil.which("caffeinate")
            if caffeinate:
                self.process = subprocess.Popen(
                    [caffeinate, "-s", "-w", str(os.getpid())],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                sys.stderr.write("keeping this Mac awake on power\n")
        elif not self.wanted() and running:
            self.process.terminate()
            self.process = None
            sys.stderr.write("letting this Mac sleep\n")

    def run(self, interval=5.0):
        while True:
            self.apply()
            time.sleep(interval)


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
    alternate, application_cursor = payload[ALTERNATE_OFFSET], payload[23]
    default_fg, default_bg = struct.unpack_from(">II", payload, 24)
    palette = struct.unpack_from(">256I", payload, PALETTE_OFFSET)
    count = struct.unpack_from(">I", payload, POOL_OFFSET)[0]
    offset = POOL_OFFSET + 4
    require(len(payload) >= offset + 4 * count + 4, "Truncated grapheme pool")
    points = struct.unpack_from(f">{count}I", payload, offset)
    require(
        all(point <= 0x10FFFF and not 0xD800 <= point <= 0xDFFF for point in points),
        "Invalid grapheme codepoint",
    )
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
            if cursor == (column, row):
                foreground, background, flags = current[1]
                line.append(run(*current))
                line.append(["", foreground, background, flags | CURSOR, column, 1])
                current = None
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
        # The phone sends the wheel to the program instead of paging history.
        "wheel": bool(alternate & ACCEPTS_WHEEL),
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
        # Whether the latest screen is a full-screen program whose service
        # takes wheel input.
        self.wheel_accepted = False
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
        body = data[SNAPSHOT_HEADER:]
        self.wheel_accepted = (
            len(body) > ALTERNATE_OFFSET and body[ALTERNATE_OFFSET] & ACCEPTS_WHEEL != 0
        )
        return body

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

    def wheel(self, steps, column, row):
        """A turn of the wheel over a cell: positive steps scroll back."""
        require(self.wheel_accepted, "This screen does not take the wheel")
        require(steps != 0 and -64 <= steps <= 64, "Invalid wheel steps")
        require(0 <= column <= 0xFFFF and 0 <= row <= 0xFFFF, "Invalid wheel cell")
        self.send(WHEEL, struct.pack(">hHH", steps, column, row))

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
        require(
            10 <= columns <= 500 and 3 <= rows <= 300 and columns * rows <= MAX_CELLS,
            "Terminal size out of range",
        )
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
    def __init__(self, registry, auth, port, folders=None, machines=None):
        self.registry = Path(registry)
        self.auth = auth
        self.port = port
        self.lock = threading.Lock()
        self.sessions = {}
        self.folders = folders or FolderIndex(self.registry)
        self.machines = machines or MachineList(self.registry)
        self.remote = RemoteFolders()

    def workspace(self):
        return load_workspace(self.registry)

    def agent(self, identifier):
        """An agent, or a quick-command terminal, by id."""
        if identifier.startswith("terminal-"):
            return next(
                (
                    item
                    for item in load_terminals(self.registry)
                    if item["id"] == identifier
                ),
                None,
            )
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


def category_route(parts, action):
    return len(parts) == 4 and parts[:2] == ["api", "categories"] and parts[3] == action


# The Mac settings the phone can change, which the Mac's Settings window also
# sets: staying awake for the phone, the Mac's alerts, and plan usage. How the
# Mac's window looks stays the Mac's to choose.
PHONE_SETTINGS = {
    "keepAwake": bool,
    "alertSound": bool,
    "alertRepeat": int,
    "finishSound": bool,
    "notify": bool,
    "showUsage": bool,
}


class Handler(BaseHTTPRequestHandler):
    server_version = "lapis-remote"
    # Keep-alive saves the phone a connection set-up per request over
    # Tailscale; idle connections close after a minute.
    protocol_version = "HTTP/1.1"
    timeout = 60
    gateway: Gateway

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass  # the phone went away between requests

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

    def reply(self, status, body, compressed=None):
        """JSON, or the same JSON already gzipped when the phone accepts it."""
        gzipped = compressed is not None and "gzip" in self.headers.get(
            "Accept-Encoding", ""
        )
        data = (
            compressed if gzipped else json.dumps(body, separators=(",", ":")).encode()
        )
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        if gzipped:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def fail(self, status, message):
        # A refused request's body may be unread; never reuse the connection.
        self.close_connection = True
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
        elif parts == ["api", "harnesses"]:
            self.harnesses()
        elif parts == ["api", "folders"]:
            self.list_folders()
        elif parts == ["api", "machines"]:
            self.list_machines()
        elif parts == ["api", "terminals"]:
            self.list_terminals()
        elif parts == ["api", "conversations"]:
            self.list_conversations()
        elif parts == ["api", "settings"]:
            self.forward({"request": "settings"}, "settings")
        elif agent_route(parts, "screen"):
            self.screen(parts[2])
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
        elif parts == ["api", "agents"]:
            self.start_agent()
        elif agent_route(parts, "close"):
            self.close_agent(parts[2])
        elif agent_route(parts, "rename"):
            self.rename_agent(parts[2])
        elif agent_route(parts, "place"):
            self.place_agent(parts[2])
        elif agent_route(parts, "restart"):
            if self.drained():
                self.forward({"request": "restartAgent", "id": parts[2]})
        elif parts == ["api", "categories"]:
            self.create_category()
        elif category_route(parts, "rename"):
            self.rename_category(parts[2])
        elif category_route(parts, "remove"):
            if self.drained():
                self.forward({"request": "removeCategory", "id": parts[2]})
        elif category_route(parts, "place"):
            self.place_category(parts[2])
        elif parts == ["api", "settings"]:
            self.change_settings()
        elif parts == ["api", "terminals"]:
            self.open_terminal()
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

    def ask_desktop(self, request):
        """The desktop's answer, or None after replying with why it failed."""
        try:
            return desktop_request(self.gateway.registry, request)
        except DesktopUnavailable as error:
            self.fail(HTTPStatus.SERVICE_UNAVAILABLE, str(error))
        except (OSError, GatewayError) as error:
            self.fail(HTTPStatus.UNPROCESSABLE_ENTITY, str(error))
        return None

    def harnesses(self):
        answer = self.ask_desktop({"request": "harnesses"})
        if answer is not None:
            self.reply(
                HTTPStatus.OK,
                {
                    "harnesses": answer.get("harnesses", []),
                    "defaults": answer.get("defaults", {}),
                },
            )

    def list_machines(self):
        machines = self.gateway.machines.current()
        if machines is None:
            self.fail(HTTPStatus.SERVICE_UNAVAILABLE, "Still looking for ssh machines")
            return
        self.reply(HTTPStatus.OK, {"machines": machines})

    def list_folders(self):
        """A machine's folder index (this Mac's without `machine`); `have`
        names the version the phone already holds."""
        query = parse_qs(urlsplit(self.path).query)
        machine = query.get("machine", [""])[0]
        if machine:
            if not MACHINE_NAME.match(machine):
                self.fail(HTTPStatus.BAD_REQUEST, "Invalid machine")
                return
            payload, compressed = self.gateway.remote.current(machine)
            if payload is None:
                self.fail(
                    HTTPStatus.SERVICE_UNAVAILABLE,
                    compressed or f"Still reading {machine}",
                )
                return
        else:
            payload, compressed = self.gateway.folders.current()
            if payload is None:
                self.fail(
                    HTTPStatus.SERVICE_UNAVAILABLE, "Still reading this Mac's folders"
                )
                return
        have = query.get("have", [""])[0]
        if have == payload["version"]:
            self.reply(HTTPStatus.OK, {"version": have, "unchanged": True})
            return
        self.reply(HTTPStatus.OK, payload, compressed)

    def screen(self, identifier):
        """The agent's current screen for the phone to show at once, read by
        joining briefly without resizing; never takes an agent from the Mac."""
        agent = self.gateway.agent(identifier)
        if agent is None:
            self.fail(HTTPStatus.NOT_FOUND, "No such agent")
            return
        try:
            session = WireSession(agent, None, None, timeout=5.0, mode=JOIN)
        except GatewayError as error:
            self.fail(HTTPStatus.CONFLICT, f"Cannot view without taking it: {error}")
            return
        except (OSError, EOFError) as error:
            self.fail(HTTPStatus.BAD_GATEWAY, f"Cannot attach: {error}")
            return
        try:
            frame = render_snapshot(session.first)
        finally:
            session.close()
        self.reply(HTTPStatus.OK, frame)

    def start_agent(self):
        """A new agent in a category, started by the Mac's lapis as a new tab."""
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_REQUEST:
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid request size")
            return
        try:
            body = json.loads(self.rfile.read(length))
            require(isinstance(body, dict), "The request must be an object")
            request = {"request": "createAgent"}
            for field, limit in (
                ("category", 64),
                ("harness", 32),
                ("directory", 4096),
            ):
                value = body.get(field)
                require(
                    isinstance(value, str) and 0 < len(value) <= limit,
                    f"Missing or invalid {field}",
                )
                request[field] = value
            title = body.get("title", "")
            require(isinstance(title, str) and len(title) <= 80, "Invalid title")
            if title:
                request["title"] = title
            for field in ("model", "mode", "resume"):
                value = body.get(field, "")
                require(
                    isinstance(value, str) and len(value) <= 128, f"Invalid {field}"
                )
                if value:
                    request[field] = value
            machine = body.get("machine", "")
            require(
                isinstance(machine, str)
                and (not machine or MACHINE_NAME.match(machine)),
                "Invalid machine",
            )
            if machine:
                request["machine"] = machine
                # The CLI's path there, when that machine's folder report found it.
                program = self.gateway.remote.program(machine, request["harness"])
                if program:
                    request["program"] = program
        except (ValueError, GatewayError) as error:
            self.fail(HTTPStatus.BAD_REQUEST, str(error))
            return
        answer = self.ask_desktop(request)
        if answer is not None:
            self.gateway.folders.refresh()  # the folder now has one more agent
            self.reply(
                HTTPStatus.OK,
                {
                    "id": str(answer.get("id", "")),
                    "updating": bool(answer.get("updating")),
                },
            )

    def read_object(self):
        """The request's JSON object, or None after replying why not."""
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_REQUEST:
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid request size")
            return None
        try:
            body = json.loads(self.rfile.read(length))
        except ValueError:
            body = None
        if not isinstance(body, dict):
            self.fail(HTTPStatus.BAD_REQUEST, "The request must be an object")
            return None
        return body

    def drained(self):
        """Reads any body, so the kept-alive connection stays in step; False
        after replying when it is too large."""
        try:
            length = int(self.headers.get("Content-Length", "0") or 0)
        except ValueError:
            length = -1
        if not 0 <= length <= MAX_REQUEST:
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid request size")
            return False
        self.rfile.read(length)
        return True

    def forward(self, request, field=None):
        """Asks the Mac's lapis; replies {"ok": true}, or its `field` when named."""
        answer = self.ask_desktop(request)
        if answer is not None:
            self.reply(
                HTTPStatus.OK,
                {field: answer.get(field, {})} if field else {"ok": True},
            )

    def create_category(self):
        """A new category on the Mac; the Mac's window stays where it is."""
        body = self.read_object()
        if body is None:
            return
        name = body.get("name")
        if not isinstance(name, str) or not 0 < len(name.strip()) <= 80:
            self.fail(HTTPStatus.BAD_REQUEST, "Use a category name of 1-80 characters")
            return
        answer = self.ask_desktop({"request": "createCategory", "name": name.strip()})
        if answer is not None:
            self.reply(HTTPStatus.OK, {"id": str(answer.get("id", ""))})

    def rename_agent(self, identifier):
        """Names the agent on the Mac; the name stays over its conversation's title."""
        body = self.read_object()
        if body is None:
            return
        title = body.get("title")
        if not isinstance(title, str) or not 0 < len(title.strip()) <= 80:
            self.fail(HTTPStatus.BAD_REQUEST, "Use an agent name of 1-80 characters")
            return
        answer = self.ask_desktop(
            {"request": "renameAgent", "id": identifier, "title": title.strip()}
        )
        if answer is not None:
            self.reply(HTTPStatus.OK, {"ok": True})

    def rename_category(self, identifier):
        """Names a category on the Mac, as Rename category does there."""
        body = self.read_object()
        if body is None:
            return
        name = body.get("name")
        if not isinstance(name, str) or not 0 < len(name.strip()) <= 80:
            self.fail(HTTPStatus.BAD_REQUEST, "Use a category name of 1-80 characters")
            return
        self.forward(
            {"request": "renameCategory", "id": identifier, "name": name.strip()}
        )

    def place_category(self, identifier):
        """Moves a category to a position in the Mac's list of categories."""
        body = self.read_object()
        if body is None:
            return
        index = body.get("index")
        if not position(index):
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid index")
            return
        self.forward({"request": "placeCategory", "id": identifier, "index": index})

    def place_agent(self, identifier):
        """Moves an agent to a position in a category (its end without one)."""
        body = self.read_object()
        if body is None:
            return
        category = body.get("category")
        index = body.get("index", 1 << 20)
        if not isinstance(category, str) or not 0 < len(category) <= 64:
            self.fail(HTTPStatus.BAD_REQUEST, "Missing or invalid category")
            return
        if not position(index):
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid index")
            return
        self.forward(
            {
                "request": "placeAgent",
                "id": identifier,
                "category": category,
                "index": index,
            }
        )

    def change_settings(self):
        """Changes some of PHONE_SETTINGS in the Mac's lapis.json; the answer
        is all of them."""
        body = self.read_object()
        if body is None:
            return
        for name, value in body.items():
            kind = PHONE_SETTINGS.get(name)
            if kind is bool:
                valid = isinstance(value, bool)
            else:
                valid = kind is int and position(value) and 1 <= value <= 10
            if not valid:
                self.fail(HTTPStatus.BAD_REQUEST, f"Invalid setting {name}")
                return
        self.forward({"request": "changeSettings", "settings": body}, "settings")

    def list_terminals(self):
        """The quick-command terminals: plain shells, one per machine."""
        self.reply(
            HTTPStatus.OK,
            {
                "terminals": [
                    {
                        "id": item["id"],
                        "machine": item["machine"],
                        "name": item["title"],
                        "running": service_answers(item["endpoint"]),
                        "onPhone": self.gateway.session(item["id"]) is not None,
                    }
                    for item in load_terminals(self.gateway.registry)
                ]
            },
        )

    def open_terminal(self):
        """A machine's terminal, started by the Mac's lapis when it has none."""
        body = self.read_object()
        if body is None:
            return
        machine = body.get("machine", "")
        if not isinstance(machine, str) or (
            machine and not MACHINE_NAME.match(machine)
        ):
            self.fail(HTTPStatus.BAD_REQUEST, "Invalid machine")
            return
        answer = self.ask_desktop({"request": "openTerminal", "machine": machine})
        if answer is not None:
            self.reply(HTTPStatus.OK, {"id": str(answer.get("id", ""))})

    def list_conversations(self):
        """This Mac's recent Claude and Codex conversations, newest first, to
        resume one as a new agent."""
        payload, _ = self.gateway.folders.current()
        if payload is None:
            self.fail(
                HTTPStatus.SERVICE_UNAVAILABLE, "Still reading this Mac's conversations"
            )
            return
        home = self.gateway.folders.home
        now = time.time()
        self.reply(
            HTTPStatus.OK,
            {
                "conversations": [
                    {
                        "harness": item["harness"],
                        "id": item["id"],
                        "directory": phone_path(item["directory"], home),
                        "title": item["title"],
                        "age": int(max(0, now - item["modified"])),
                    }
                    for item in self.gateway.folders.history.conversations[:60]
                ]
            },
        )

    def close_agent(self, identifier):
        """Ends the agent, as Command-Shift-W does on the Mac, or a terminal's shell."""
        if not self.drained():
            return
        kind = "closeTerminal" if identifier.startswith("terminal-") else "closeAgent"
        answer = self.ask_desktop({"request": kind, "id": identifier})
        if answer is not None:
            self.gateway.supersede(identifier)
            self.reply(HTTPStatus.OK, {"ok": True})

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
        # The stream has no length; it ends when either side closes it.
        self.send_header("Connection", "close")
        self.close_connection = True
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
            page = (
                render_snapshot(reply["snapshot"], show_cursor=False)
                if reply["snapshot"]
                else None
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
        if page is not None:
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
            require(isinstance(body, dict), "Capture must be an object")
            image = base64.b64decode(body.pop("png"), validate=True)
            require(image.startswith(b"\x89PNG\r\n\x1a\n"), "Not a PNG")
        except (ValueError, KeyError, TypeError, GatewayError) as error:
            self.fail(HTTPStatus.BAD_REQUEST, f"Invalid capture: {error}")
            return
        directory = self.gateway.registry.parent / "phone-captures"
        directory.mkdir(mode=0o700, exist_ok=True)
        # One-second names silently replaced rapid debug captures.
        name = time.strftime("%Y%m%d-%H%M%S") + f"-{uuid.uuid4().hex}"
        for suffix, data in (
            (".png", image),
            (".json", json.dumps(body, indent=1).encode()),
        ):
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
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
            if "wheel" in body:
                steps, column, row = body["wheel"]
                require(
                    all(position(value) for value in (column, row))
                    and isinstance(steps, int)
                    and not isinstance(steps, bool),
                    "Invalid wheel",
                )
                session.wheel(steps, column, row)
        except (ValueError, TypeError, KeyError, GatewayError, OSError) as error:
            self.fail(HTTPStatus.BAD_REQUEST, str(error))
            return
        self.reply(HTTPStatus.OK, {"ok": True})


def lapis_home(environ=None, home=None):
    """Where lapis keeps its workspace, as the desktop decides: LAPIS_HOME,
    else the downloaded app's ~/.lapis once it has a workspace, else this
    checkout (a developer build)."""
    environ = os.environ if environ is None else environ
    if environ.get("LAPIS_HOME"):
        return Path(environ["LAPIS_HOME"])
    app = Path(home or Path.home()) / ".lapis"
    if (app / "runtime" / "workspace.json").is_file():
        return app
    return ROOT


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
    Handler.gateway = Gateway(
        args.registry,
        auth,
        args.port,
        folders=FolderIndex(
            args.registry,
            home=args.folders_home,
            codex_home=args.codex_home,
            claude_home=args.claude_home,
        ),
        machines=MachineList(args.registry, args.ssh_config, args.shell_history),
    )
    if sys.platform == "darwin":
        threading.Thread(target=KeepAwake(args.config).run, daemon=True).start()
    sys.stderr.write(f"lapis remote on {bind}:{args.port} for {auth.dns_name}\n")
    server.serve_forever()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--registry", default=str(lapis_home() / "runtime" / "workspace.json")
    )
    parser.add_argument(
        "--config", default=str(lapis_home() / "lapis.json"), help="lapis settings"
    )
    parser.add_argument("--bind", help="address; defaults to this Mac's Tailscale IPv4")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--tailscale", default="tailscale")
    # Fixtures for the simulator check; the defaults are this user's own.
    parser.add_argument("--folders-home", help="folders the phone browses (tests)")
    parser.add_argument("--codex-home", help="Codex history to rank folders by (tests)")
    parser.add_argument("--claude-home", help="Claude Code history (tests)")
    parser.add_argument("--ssh-config", help="ssh config to list machines from (tests)")
    parser.add_argument(
        "--shell-history",
        action="append",
        help="shell history to rank machines by (tests)",
    )
    parser.add_argument(
        "--allow-local",
        action="store_true",
        help="also admit 127.0.0.1 without whois (tests only)",
    )
    serve(parser.parse_args(argv))


if __name__ == "__main__":
    os.umask(0o077)
    main()
