"""Exercise explicit CLI launch, attachment isolation and optional desktop capture."""

import argparse
import hashlib
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time
import traceback
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

if __package__:
    from . import lapis
else:
    import lapis

ROOT = Path(__file__).resolve().parents[1]
VERSION = 6
HELLO, SNAPSHOT, TEXT, PASTE, KEY, RESIZE, STATUS, ATTACH, READY = range(1, 10)
HISTORY_REQUEST, HISTORY_PAGE = range(10, 12)
ATTENTION_SNAPSHOT, ATTENTION_DECISION, ATTENTION_RETRY = range(12, 15)
PASTE_REQUEST, PASTE_RESULT = 17, 18
WAIT = 5


class CheckError(RuntimeError):
    pass


class FrameDeadline(CheckError):
    pass


def require(condition, message):
    if not condition:
        raise CheckError(message)


def qt_string(value):
    encoded = str(value).encode("utf-16-be")
    return struct.pack(">I", len(encoded)) + encoded


def fingerprint(program, arguments, directory, *, codex=False, claude=False):
    data = qt_string(os.path.abspath(program)) + struct.pack(">I", len(arguments))
    data += b"".join(qt_string(argument) for argument in arguments)
    data += qt_string(Path(directory).resolve())
    require(not (codex and claude), "Expected at most one agent mode")
    if claude:
        data = b"lapis-claude-v1\0" + data
    if codex:
        data = b"lapis-codex-v1\0" + data
    return hashlib.sha256(data).digest()


def frame(kind, payload=b""):
    return struct.pack(">IB", len(payload) + 1, kind) + payload


def history_request_payload(request_id, direction=0, reference=0):
    return struct.pack(">QQB", request_id, reference, direction)


def decode_history_reply(payload):
    require(len(payload) >= 60, "Truncated history reply")
    attachment = payload[:40]
    request_id, page_id = struct.unpack_from(">QQ", payload, 40)
    length = struct.unpack_from(">I", payload, 56)[0]
    require(len(payload) == 60 + length, "Invalid history reply message")
    return {
        "attachment": attachment,
        "request_id": request_id,
        "page_id": page_id,
        "message": payload[60:].decode("utf-8"),
    }


def attach_payload(
    program,
    arguments,
    directory,
    expected=None,
    *,
    codex=False,
    claude=False,
    paste_transactions=False,
    join=False,
):
    identity = expected[:32] if expected is not None else bytes(32)
    return (
        struct.pack(">I", VERSION)
        + fingerprint(program, arguments, directory, codex=codex, claude=claude)
        + bytes(
            [
                (3 if join else 1 if expected is not None else 0)
                | (0x20 if paste_transactions else 0)
            ]
        )
        + identity
    )


def decode_snapshot(payload):
    # Fixed prefix mirrors local_protocol.cpp: cursor RGB is always serialized,
    # including when its preceding presence flag is false. Each cell is 27 bytes.
    require(len(payload) >= 1089, "Truncated snapshot prefix")
    revision, columns, rows = struct.unpack_from(">QHH", payload)
    count = struct.unpack_from(">I", payload, 1085)[0]
    require(count <= 65536, "Oversized grapheme pool")
    offset = 1089 + 4 * count
    require(len(payload) >= offset + 4, "Truncated grapheme pool")
    points = struct.unpack_from(f">{count}I", payload, 1089)
    cells = struct.unpack_from(">I", payload, offset)[0]
    offset += 4
    require(cells == columns * rows and cells <= 32768, "Invalid snapshot geometry")
    require(len(payload) == offset + cells * 27, "Invalid cell payload size")
    text = []
    for index in range(cells):
        start, length = struct.unpack_from(">II", payload, offset + index * 27)
        require(start + length <= count, "Invalid grapheme reference")
        text.append(
            "".join(chr(point) for point in points[start : start + length]) or " "
        )
        if (index + 1) % columns == 0:
            text.append("\n")
    return {
        "revision": revision,
        "columns": columns,
        "rows": rows,
        "text": "".join(text),
    }


class AttentionReader:
    def __init__(self, data):
        require(len(data) <= 1024 * 1024, "Oversized attention snapshot")
        self.data = data
        self.offset = 0

    def take(self, size):
        require(0 <= size <= len(self.data) - self.offset, "Truncated attention field")
        result = self.data[self.offset : self.offset + size]
        self.offset += size
        return result

    def number(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def string(self, limit=32768):
        size = self.number(">I")
        require(size <= limit, "Oversized attention string")
        return self.take(size).decode("utf-8")

    def request_id(self):
        kind = self.number(">B")
        require(kind in (0, 1), "Invalid request ID tag")
        return self.number(">q") if kind == 0 else self.string(1024)


def decode_attention_snapshot(data):
    """Decode the versioned service attention frame shared by QA probes."""
    reader = AttentionReader(data)
    require(reader.number(">I") == VERSION, "Attention version mismatch")
    result = {"attachment": reader.take(40)}
    for key in ("available", "connected", "ready"):
        value = reader.number(">B")
        require(value in (0, 1), "Invalid readiness flag")
        result[key] = bool(value)
    result["activity"] = reader.number(">B")
    result["epoch"] = reader.number(">Q")
    result["diagnostic"] = reader.string(4096)
    count = reader.number(">I")
    require(count <= 128, "Oversized attention list")
    result["requests"] = []
    for _ in range(count):
        request = {"id": reader.request_id()}
        for key in ("thread", "turn", "item", "reason", "summary"):
            request[key] = reader.string(4096)
        choices = reader.number(">I")
        require(choices <= 32, "Oversized choices")
        request["choices"] = [reader.string(256) for _ in range(choices)]
        request["priority"] = reader.number(">B")
        request["details"] = json.loads(reader.string())
        request["status"] = reader.number(">B")
        for key in ("revision", "arrived", "not_before", "epoch"):
            request[key] = reader.number(">Q")
        request["submitted"] = bool(reader.number(">B"))
        result["requests"].append(request)
    require(reader.offset == len(data), "Trailing attention bytes")
    return result


def default_shell() -> str:
    """Mirror the session service: $SHELL, then the account's login shell.

    The service resolves the shell when environment lacks SHELL, so this must
    agree with it or the launch fingerprints differ and attachment is rejected.
    """
    configured = os.environ.get("SHELL")
    if configured:
        return configured
    try:
        import pwd

        return pwd.getpwuid(os.getuid()).pw_shell or "/bin/sh"
    except (ImportError, KeyError, OSError):
        return "/bin/sh"


class WireClient:
    def __init__(self, endpoint):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(WAIT)
        self.buffer = bytearray()
        self.attachment = None
        self.sequence = 0
        self.cached_snapshot = None
        try:
            self.socket.connect(str(endpoint))
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self.socket.close()

    def send(self, kind, payload=b""):
        if kind in (TEXT, PASTE, KEY, RESIZE, HISTORY_REQUEST, ATTENTION_DECISION):
            require(self.attachment is not None, "No accepted attachment")
            payload = self.attachment + payload
        self.socket.sendall(frame(kind, payload))

    def receive(self, timeout=WAIT):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack_from(">I", self.buffer)[0]
                require(1 <= size <= 8 * 1024 * 1024, "Invalid frame size")
                if len(self.buffer) >= size + 4:
                    kind, payload = self.buffer[4], bytes(self.buffer[5 : size + 4])
                    del self.buffer[: size + 4]
                    return kind, payload
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise FrameDeadline("Frame deadline expired")
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(65536)
            if not chunk:
                raise EOFError("Service disconnected")
            self.buffer.extend(chunk)

    def attach(
        self,
        program,
        arguments,
        directory,
        expected=None,
        *,
        codex=False,
        claude=False,
        paste_transactions=False,
        join=False,
    ):
        self.send(
            ATTACH,
            attach_payload(
                program,
                arguments,
                directory,
                expected,
                codex=codex,
                claude=claude,
                paste_transactions=paste_transactions,
                join=join,
            ),
        )
        pid = self.hello()
        require(
            not paste_transactions or self.paste_transactions,
            "Paste capability not acknowledged",
        )
        if expected is not None:
            require(self.attachment[:32] == expected[:32], "Reconnect identity changed")
            require(
                struct.unpack(">Q", self.attachment[32:])[0]
                > struct.unpack(">Q", expected[32:])[0],
                "Generation did not advance",
            )
        self.cached_snapshot = self.next_snapshot()
        self.send(READY, self.attachment + struct.pack(">Q", self.sequence))
        return pid

    def hello(self):
        kind, data = self.receive()
        require(
            kind == HELLO and len(data) in (52, 56), "Expected hello after attachment"
        )
        self.paste_transactions = (
            len(data) == 56 and struct.unpack_from(">I", data, 52)[0] == 1
        )
        version = struct.unpack_from(">I", data)[0]
        self.attachment = data[4:44]
        pid = struct.unpack_from(">Q", data, 44)[0]
        require(
            version == VERSION
            and pid > 0
            and data[4:20] != bytes(16)
            and data[20:36] != bytes(16)
            and data[36:44] != bytes(8),
            "Invalid hello identity",
        )
        return pid

    def next_snapshot(self, timeout=WAIT):
        kind, data = self.receive(timeout)
        require(kind == SNAPSHOT and len(data) > 48, "Expected snapshot envelope")
        require(data[:40] == self.attachment, "Snapshot attachment mismatch")
        sequence = struct.unpack_from(">Q", data, 40)[0]
        require(sequence > self.sequence, "Snapshot sequence did not advance")
        self.sequence = sequence
        return decode_snapshot(data[72:])

    def snapshot(self, predicate=lambda _: True, timeout=WAIT):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.cached_snapshot is not None:
                snapshot, self.cached_snapshot = self.cached_snapshot, None
            else:
                snapshot = self.next_snapshot(max(0.01, deadline - time.monotonic()))
            if predicate(snapshot):
                return snapshot
        raise CheckError("Snapshot condition not met")

    def status(self):
        deadline = time.monotonic() + WAIT
        while time.monotonic() < deadline:
            kind, data = self.receive(max(0.01, deadline - time.monotonic()))
            if kind == STATUS:
                require(len(data) > 1 and 1 <= data[0] <= 4, "Invalid status")
                return data[1:].decode("utf-8")
            require(kind == SNAPSHOT, "Unexpected frame before status")
        raise CheckError("Status deadline expired")

    def history_reply(self):
        deadline = time.monotonic() + WAIT
        while True:
            kind, data = self.receive(max(0.01, deadline - time.monotonic()))
            if kind == HISTORY_PAGE:
                reply = decode_history_reply(data)
                require(
                    reply["attachment"] == self.attachment,
                    "History reply attachment mismatch",
                )
                return reply
            require(kind == SNAPSHOT, "Unexpected frame before history reply")


def wait_raw_terminal(pid):
    """The Codex banner may be drawn before its PTY enters interactive raw mode."""
    terminal = subprocess.run(
        ["ps", "-p", str(pid), "-o", "tty="],
        check=True,
        capture_output=True,
        text=True,
        timeout=5,
    ).stdout.strip()
    require(
        terminal and terminal not in ("?", "??") and ".." not in terminal,
        "Controlled CLI has no terminal",
    )
    path = Path("/dev") / terminal.removeprefix("/dev/")
    descriptor = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        deadline = time.monotonic() + WAIT
        while time.monotonic() < deadline:
            local_flags = termios.tcgetattr(descriptor)[3]
            if not local_flags & (termios.ICANON | termios.ECHO):
                return
            time.sleep(0.02)
        raise CheckError("Controlled CLI did not enter raw terminal input mode")
    finally:
        os.close(descriptor)


def wait_socket(endpoint, process):
    deadline = time.monotonic() + WAIT
    while time.monotonic() < deadline:
        require(
            process.poll() is None, "Service exited before listening; see service log"
        )
        if endpoint.exists():
            return
        time.sleep(0.02)
    raise CheckError("Service did not listen in time")


class Service:
    def __init__(
        self,
        binary,
        runtime,
        artifacts,
        name,
        program,
        arguments,
        directory,
        env_overrides=None,
        *,
        codex=False,
        claude=False,
    ):
        require(not (codex and claude), "Expected at most one agent mode")
        self.endpoint = runtime / (name + ".sock")
        self.program, self.arguments, self.directory = program, arguments, directory
        self.child_pid = None
        self.codex = codex
        self.claude = claude
        self.attachment = None
        self.log_path = artifacts / (name + ".service.log")
        self.log = self.log_path.open("wb")
        self.process = subprocess.Popen(
            [str(binary)]
            + (["--codex"] if codex else ["--claude"] if claude else [])
            + [str(self.endpoint), str(directory), program, *arguments],
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=self.log,
            start_new_session=True,
            env={
                **os.environ,
                "LAPIS_HISTORY_ROOT": str(runtime / "history"),
                **(env_overrides or {}),
            },
        )

    def connect(self):
        wait_socket(self.endpoint, self.process)
        client = WireClient(self.endpoint)
        try:
            pid = client.attach(
                self.program,
                self.arguments,
                self.directory,
                self.attachment,
                codex=self.codex,
                claude=self.claude,
            )
            self.attachment = client.attachment
            if self.child_pid is not None:
                require(pid == self.child_pid, "Reattachment changed the child PID")
            self.child_pid = pid
            return client
        except BaseException:
            client.close()
            raise

    def stop(self):
        try:
            if self.process.poll() is None:
                if self.child_pid is not None:
                    try:
                        os.killpg(self.child_pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                else:
                    self.process.terminate()
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    if self.child_pid is not None:
                        try:
                            os.killpg(self.child_pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    self.process.kill()
                    self.process.wait(timeout=3)
        finally:
            self.log.close()

    def log_text(self):
        self.log.flush()
        return self.log_path.read_text(errors="replace")


FIXTURE = r"""
import hashlib,json,os,signal,sys,termios,time,tty
attr=termios.tcgetattr(0);attr[3]&=~termios.ECHO;termios.tcsetattr(0,termios.TCSANOW,attr)
print('ARGV='+hashlib.sha256(json.dumps(sys.argv[1:],ensure_ascii=False).encode()).hexdigest(),flush=True)
print('CWD='+hashlib.sha256(os.getcwd().encode()).hexdigest(),flush=True)
print('READY',flush=True)
for line in sys.stdin:
    command=line.rstrip('\n')
    if command=='quit': sys.exit(7)
    if command=='pause':
        tty.setraw(0); print('PAUSED',flush=True); os.kill(os.getpid(),signal.SIGSTOP)
    elif command=='overflow':
        print('ORDER_START',flush=True)
        burst=b'\x1b[H'*44000
        while burst:
            count=os.write(1,burst)
            burst=burst[count:]
        print('\x1b[5;1HORDER_END',flush=True)
        print('OVERFLOW_WRITTEN',flush=True)
        os.kill(os.getpid(),signal.SIGSTOP)
    elif command=='burst':
        time.sleep(.3)
        for i in range(12000): print('0123456789abcdef')
        print('DETACHED_DONE',flush=True)
    elif command=='size':
        size=os.get_terminal_size(0);print('SIZE=%dx%d'%(size.columns,size.lines),flush=True)
    else: print('ECHO:'+command,flush=True)
"""


def run_capture(desktop, options, artifacts, name, expect_success=True):
    options = list(options)
    if "--ui-preview" not in options and "--codex" not in options:
        options.insert(0, "--development-shell")
    if (
        expect_success
        and "--socket" in options
        and "--new-session" not in options
        and "--discover" not in options
    ):
        endpoint = Path(options[options.index("--socket") + 1])
        if not Path(str(endpoint) + ".session").exists():
            options.insert(0, "--discover")
    with (artifacts / (name + ".log")).open("wb") as log:
        process = subprocess.Popen(
            [str(desktop), *options],
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=log,
            start_new_session=True,
        )
        try:
            code = process.wait(timeout=20)
        except BaseException:
            process.kill()
            process.wait(timeout=3)
            raise
    require(
        (code == 0) == expect_success, f"Unexpected desktop exit {code}; see {name}.log"
    )
    return code


def _case_actions(build, runtime, artifacts, desktop_enabled, codex=None):
    binary = build / "services/session/lapis_session_service"
    desktop = build / "apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop"
    if sys.platform != "darwin":
        desktop = build / "apps/desktop/lapis_desktop"
    program = str(Path(sys.executable).resolve())
    values = ["has space", "meta*$=!;|<>", "quote'\\\"", "line\nbreak", "", "界😀"]
    values += ["-platform", "literal-not-a-platform", "--help"]
    arguments = ["-u", "-c", FIXTURE, *values]

    @contextmanager
    def session(name):
        service = Service(binary, runtime, artifacts, name, program, arguments, runtime)
        try:
            yield service
        finally:
            service.stop()

    def hyperlink_capability():
        # The legacy decoder stays strict. A capable joined view receives the
        # same text plus OSC 8 destinations without replacing the old client.
        code = "import sys; print('\\x1b]8;;https://example.com/target\\x1b\\\\label\\x1b]8;;\\x1b\\\\', flush=True); sys.stdin.readline()"
        service = Service(
            binary, runtime, artifacts, "links", program, ["-u", "-c", code], runtime
        )
        try:
            with service.connect() as legacy:
                legacy.snapshot(lambda snap: "label" in snap["text"])
                for index, capabilities in enumerate((0x80, 0xC0)):
                    with WireClient(service.endpoint) as capable:
                        request = bytearray(
                            attach_payload(program, service.arguments, runtime)
                        )
                        request[36] = capabilities | 3  # join, independent capabilities
                        capable.send(ATTACH, request)
                        require(
                            capable.hello() == service.child_pid,
                            "Joined view replaced the child",
                        )
                        kind, data = capable.receive()
                        require(
                            kind == SNAPSHOT and data[:40] == capable.attachment,
                            "Wrong joined screen",
                        )
                        snapshot = data[72:]
                        points = struct.unpack_from(">I", snapshot, 1085)[0]
                        offset = 1089 + points * 4
                        cells = struct.unpack_from(">I", snapshot, offset)[0]
                        end = offset + 4 + cells * 27
                        require(
                            "label" in decode_snapshot(snapshot[:end])["text"],
                            "Joined text was lost",
                        )
                        marker, spans, first, count, length = struct.unpack_from(
                            ">IIIII", snapshot, end
                        )
                        require(
                            (marker, spans, first, count) == (0x4C4E4B31, 1, 0, 5)
                            and snapshot[end + 20 :] == b"https://example.com/target"
                            and length == len(snapshot[end + 20 :]),
                            "OSC 8 destination was lost between PTY and joined client",
                        )
                        capable.send(READY, capable.attachment + data[40:48])
                        # A resize forces another snapshot to the legacy attachment;
                        # its strict decoder still requires the exact old format.
                        columns = 79 - index
                        legacy.send(RESIZE, struct.pack(">HH", columns, 24))
                        legacy.snapshot(
                            lambda snap: (
                                snap["columns"] == columns and "label" in snap["text"]
                            )
                        )
            return {
                "legacy_and_capable_views": True,
                "same_child": True,
                "osc8_from_real_pty": True,
                "phase_capability_preserves_links": True,
            }
        finally:
            service.stop()

    def literal_resize_exit():
        with session("literal") as service, service.connect() as client:
            argv_hash = hashlib.sha256(
                json.dumps(values, ensure_ascii=False).encode()
            ).hexdigest()
            cwd_hash = hashlib.sha256(str(runtime.resolve()).encode()).hexdigest()
            client.snapshot(
                lambda s: (
                    "ARGV=" + argv_hash in s["text"] and "CWD=" + cwd_hash in s["text"]
                )
            )
            client.send(RESIZE, struct.pack(">HH", 73, 19))
            client.snapshot(lambda s: (s["columns"], s["rows"]) == (73, 19))
            client.send(TEXT, b"size\n")
            client.snapshot(lambda s: "SIZE=73x19" in s["text"])
            client.send(PASTE, b"literal-paste\n")
            client.snapshot(lambda s: "ECHO:literal-paste" in s["text"])
            client.send(TEXT, b"quit\n")
            require(
                "Process exited (7)" in client.status(), "Child exit status was lost"
            )
            require(
                service.process.wait(timeout=WAIT) == 7, "Service exit code was lost"
            )

    def detach():
        with session("detach") as service:
            with service.connect() as first:
                first.snapshot(lambda s: "READY" in s["text"])
                first.send(TEXT, b"burst\n")
            time.sleep(0.5)
            with service.connect() as second:
                second.snapshot(lambda s: "DETACHED_DONE" in s["text"])
                second.send(TEXT, b"still-usable\n")
                second.snapshot(lambda s: "ECHO:still-usable" in s["text"])

    def mismatch():
        with session("mismatch") as service, service.connect() as good:
            good.snapshot(lambda s: "READY" in s["text"])
            with WireClient(service.endpoint) as bad:
                bad.send(
                    ATTACH, attach_payload(program, arguments + ["different"], runtime)
                )
                require("mismatch" in bad.status().lower(), "Mismatch not reported")
            good.send(TEXT, b"original-client\n")
            good.snapshot(lambda s: "ECHO:original-client" in s["text"])
            with WireClient(service.endpoint) as slow:
                slow.socket.sendall(
                    frame(ATTACH, attach_payload(program, arguments, runtime))[:-1]
                )
                try:
                    slow.receive(timeout=4)
                except EOFError:
                    pass
                else:
                    raise CheckError("Partial handshake was not disconnected")
            with WireClient(service.endpoint) as wrong:
                wrong.send(ATTACH, struct.pack(">II", VERSION + 1, 32) + bytes(32))
                require(
                    "incompatible" in wrong.status(), "Protocol version not rejected"
                )
            good.send(TEXT, b"after-invalid\n")
            good.snapshot(lambda s: "ECHO:after-invalid" in s["text"])

    def codex_config_arguments():
        marker = runtime / "backend-arguments.json"
        executable = runtime / "argument-codex"
        executable.write_text(
            f"#!{program}\n"
            "import json,sys\n"
            "from pathlib import Path\n"
            f"Path({str(marker)!r}).write_text(json.dumps(sys.argv[1:]))\n"
        )
        executable.chmod(0o700)
        cases = []
        for option in ("-c", "--config", "--enable", "--disable"):
            cases.extend(
                [
                    ([option], None),
                    ([option, "--", "--config=literal"], None),
                    (
                        [option, "fixture", "--", "--config=literal"],
                        [option, "fixture"],
                    ),
                ]
            )
        cases.append((["--", "-c", "--", "--enable=literal"], []))
        for index, (arguments, expected) in enumerate(cases):
            marker.unlink(missing_ok=True)
            name = f"codex-arguments-{index}"
            service = Service(
                binary,
                runtime,
                artifacts,
                name,
                str(executable),
                arguments,
                runtime,
                codex=True,
            )
            try:
                code = service.process.wait(timeout=5)
                require(code != 0, "Fixture backend exit must stop its service")
            finally:
                service.stop()
            if expected is None:
                require(
                    not marker.exists(),
                    f"Malformed option launched backend: {arguments}",
                )
                require(
                    "Codex config option requires a value"
                    in (artifacts / (name + ".service.log")).read_text(),
                    f"Missing config-value diagnostic: {arguments}",
                )
            else:
                require(
                    marker.exists(), f"Valid options did not reach backend: {arguments}"
                )
                forwarded = json.loads(marker.read_text())
                require(
                    forwarded[:2] == ["app-server", "--listen"]
                    and forwarded[2] == "unix://" + str(service.endpoint) + ".codex"
                    and forwarded[3:] == expected,
                    f"Literal arguments leaked into backend options: {forwarded}",
                )
        return {"argument_cases": len(cases), "live_codex_used": False}

    def codex_resume_permissions():
        # Codex refuses approval and sandbox options when a remote TUI resumes
        # a thread; the dedicated server takes them as config instead.
        executable = runtime / "permission-codex"
        executable.write_text(
            f"#!{program}\n"
            "import json,socket,sys\n"
            "from pathlib import Path\n"
            f"fixture_root=Path({str(runtime)!r})\n"
            "if len(sys.argv)<2:\n"
            " sys.exit(2)\n"
            "if sys.argv[1]=='app-server':\n"
            " if len(sys.argv)<4:\n"
            "  sys.exit(2)\n"
            " endpoint=sys.argv[3].removeprefix('unix://')\n"
            " marker=fixture_root/(Path(endpoint).name+'.backend.json')\n"
            " marker.write_text(json.dumps(sys.argv[1:]))\n"
            " server=socket.socket(socket.AF_UNIX)\n"
            " server.bind(endpoint)\n"
            " server.listen()\n"
            " while True:\n"
            "  connection,_=server.accept();connection.close()\n"
            "else:\n"
            " if len(sys.argv)<3:\n"
            "  sys.exit(2)\n"
            " endpoint=sys.argv[2].removeprefix('unix://')\n"
            " marker=fixture_root/(Path(endpoint).name+'.tui.json')\n"
            " marker.write_text(json.dumps(sys.argv[1:]))\n"
            " for line in sys.stdin: pass\n"
        )
        executable.chmod(0o700)
        thread = "00000000-0000-7000-8000-000000000001"
        full = ['approval_policy="never"', 'sandbox_mode="danger-full-access"']
        cases = [
            (
                ["--dangerously-bypass-approvals-and-sandbox", "resume", thread],
                full,
                ["resume", thread],
            ),
            (
                ["--full-auto", "resume", thread],
                ['approval_policy="on-request"', 'sandbox_mode="workspace-write"'],
                ["resume", thread],
            ),
            (
                ["--yolo", "fork", thread],
                full,
                ["fork", thread],
            ),
            (
                ["--approve-for-me", "resume", thread],
                [
                    'approval_policy="on-request"',
                    'sandbox_mode="workspace-write"',
                    'approvals_reviewer="auto_review"',
                ],
                ["resume", thread],
            ),
            # Permission config on a resuming TUI is an explicit override Codex
            # refuses; the server keeps it, unrelated config survives.
            (
                ["-c", "approval_policy=never", "-c", "model=o3", "resume", thread],
                ['approval_policy="never"', "model=o3"],
                ["-c", "model=o3", "resume", thread],
            ),
            (
                ["-c", "approvals_reviewer=auto_review", "resume", thread],
                ['approvals_reviewer="auto_review"'],
                ["resume", thread],
            ),
            (
                ["-c", "approvals_reviewer=guardian_subagent", "resume", thread],
                ['approvals_reviewer="guardian_subagent"'],
                ["resume", thread],
            ),
            (
                ["-c", 'approvals_reviewer="auto_review"', "resume", thread],
                ['approvals_reviewer="auto_review"'],
                ["resume", thread],
            ),
            (
                ['--config=sandbox_mode="read-only"', "resume", thread],
                ['sandbox_mode="read-only"'],
                ["resume", thread],
            ),
            (
                ['-c=sandbox_mode = "read-only"', "resume", thread],
                ['sandbox_mode="read-only"'],
                ["resume", thread],
            ),
            (
                ["-a", "never", "-s", "workspace-write", "fork", thread],
                ['approval_policy="never"', 'sandbox_mode="workspace-write"'],
                ["fork", thread],
            ),
            (
                ["-s=read-only", "resume", thread],
                ['sandbox_mode="read-only"'],
                ["resume", thread],
            ),
            (
                ["-sread-only", "resume", thread],
                ['sandbox_mode="read-only"'],
                ["resume", thread],
            ),
            (
                ["resume", thread, "--sandbox=read-only"],
                ['sandbox_mode="read-only"'],
                ["resume", thread],
            ),
            # A new thread keeps its options; Codex accepts them there.
            (
                ["--dangerously-bypass-approvals-and-sandbox"],
                full,
                ["--dangerously-bypass-approvals-and-sandbox"],
            ),
            (
                ["--full-auto"],
                ['approval_policy="on-request"', 'sandbox_mode="workspace-write"'],
                ["--full-auto"],
            ),
            (
                ["--approve-for-me"],
                [
                    'approval_policy="on-request"',
                    'sandbox_mode="workspace-write"',
                    'approvals_reviewer="auto_review"',
                ],
                ["--approve-for-me"],
            ),
            # "resume" as an option's value is not the subcommand.
            (["-m", "resume", "-a", "never"], ['approval_policy="never"'], None),
            # After the literal separator, every token belongs to the TUI.
            (
                ["--", "-a", "never", "resume", thread],
                [],
                ["--", "-a", "never", "resume", thread],
            ),
        ]
        for key, value in {
            "approval_policy": "untrusted",
            "sandbox_mode": "read-only",
            "approvals_reviewer": "user",
        }.items():
            cases.extend(
                [
                    (
                        ["-c", f"{key}={value}", "resume", thread],
                        [f'{key}="{value}"'],
                        ["resume", thread],
                    ),
                    (
                        ["-c", f'{key}="{value}"', "resume", thread],
                        [f'{key}="{value}"'],
                        ["resume", thread],
                    ),
                ]
            )
        for index, (arguments, backend, tui) in enumerate(cases):
            if tui is None:
                tui = arguments
            name = f"codex-permissions-{index}"
            service = Service(
                binary,
                runtime,
                artifacts,
                name,
                str(executable),
                arguments,
                runtime,
                codex=True,
            )
            try:
                socket_path = str(service.endpoint) + ".codex"
                marker = Path(socket_path + ".tui.json")
                deadline = time.monotonic() + WAIT
                while not marker.exists():
                    require(
                        time.monotonic() < deadline, f"TUI never started: {arguments}"
                    )
                    time.sleep(0.01)
                time.sleep(0.05)
                forwarded = json.loads(Path(socket_path + ".backend.json").read_text())
                # `backend` holds config values unless a case marks itself raw
                # (a `(raw_backend)` tuple), in which case it is literal argv.
                if isinstance(backend, tuple):
                    expected = list(backend[0])
                else:
                    expected = [part for pair in backend for part in ("-c", pair)]
                require(
                    forwarded[3:] == expected,
                    f"Backend permissions {forwarded[3:]} != {expected} for {arguments}",
                )
                launched = json.loads(marker.read_text())
                require(
                    launched == ["--remote", "unix://" + socket_path] + tui,
                    f"TUI arguments {launched} for {arguments}",
                )
            finally:
                service.stop()
        invalid_arguments = [
            ["-a", "untrusted", "resume", thread],
            ["-s", 'x" y', "resume", thread],
            ["-s", "read-only\n", "resume", thread],
            ["-sandbox", "resume", thread],
            ["-a", "on-failure", "resume", thread],
            ["-c", 'approval_policy=x" y', "resume", thread],
            ["-c", 'approval_policy="bogus"', "resume", thread],
            ["-c", " approval_policy=bogus", "resume", thread],
            ["-c", "approvals_reviewer=bogus", "resume", thread],
            ["-a"],
            ["--sandbox="],
        ]
        syntax_invalid_indices = set()
        for key, value in {
            "approval_policy": "never",
            "sandbox_mode": "read-only",
            "approvals_reviewer": "user",
        }.items():
            start = len(invalid_arguments)
            invalid_arguments.extend(
                [
                    ["-c", f'{key}={value}"', "resume", thread],
                    ["-c", f'{key}="{value}', "resume", thread],
                ]
            )
            syntax_invalid_indices.add(start + 1)
        syntax_invalid_indices.add(len(invalid_arguments))
        invalid_arguments.append(["-c", 'approval_policy="never" x', "resume", thread])
        for index, arguments in enumerate(invalid_arguments):
            name = f"codex-permissions-invalid-{index}"
            service = Service(
                binary,
                runtime,
                artifacts,
                name,
                str(executable),
                arguments,
                runtime,
                codex=True,
            )
            try:
                try:
                    code = service.process.wait(timeout=WAIT)
                except subprocess.TimeoutExpired:
                    raise CheckError(
                        f"Invalid permission value hung the service: {arguments}"
                    ) from None
                else:
                    require(
                        code != 0,
                        f"Invalid permission value launched service: {arguments}",
                    )
            finally:
                service.stop()
            diagnostic = (artifacts / (name + ".service.log")).read_text()
            expected_diagnostic = (
                "config syntax is invalid"
                if index in syntax_invalid_indices
                else "requires a value"
                if index == 9
                else "value is invalid for"
            )
            require(
                expected_diagnostic in diagnostic,
                f"Missing {expected_diagnostic!r} diagnostic: {arguments}",
            )
        return {"permission_cases": len(cases), "live_codex_used": False}

    def codex_backend_exit():
        # Exercise a dedicated backend failure before TUI startup. Raw backend
        # output may contain private data and must not enter service diagnostics.
        executable = runtime / "exiting-codex"
        private_marker = "PRIVATE_BACKEND_STDERR_FIXTURE"
        outcomes = []
        for name, statement, expected in (
            ("normal", "sys.exit(23)", ("23", "normal")),
            ("signal", "os.kill(os.getpid(), signal.SIGKILL)", ("crash",)),
        ):
            executable.write_text(
                f"#!{program}\n"
                "import os,signal,sys\n"
                f"print({private_marker!r},file=sys.stderr,flush=True)\n"
                + statement
                + "\n"
            )
            executable.chmod(0o700)
            fixture = "codex-exit-" + name
            service = Service(
                binary,
                runtime,
                artifacts,
                fixture,
                str(executable),
                [],
                runtime,
                codex=True,
            )
            try:
                require(
                    service.process.wait(timeout=WAIT) != 0,
                    "Backend failure did not stop startup",
                )
            finally:
                service.stop()
            diagnostic = (artifacts / (fixture + ".service.log")).read_text()
            require(
                "Codex server exited" in diagnostic
                and all(part in diagnostic.lower() for part in expected),
                f"Backend {name} exit lost code/status diagnostic",
            )
            require(
                private_marker not in diagnostic,
                "Backend stderr leaked into service diagnostics",
            )
            outcomes.append(name)
        return {"backend_exit_cases": outcomes, "raw_stderr_excluded": True}

    def attention_publication_pacing():
        """Real service IPC, with an unqualified disposable Codex source."""
        from check_service_attention import decision as make_attention_decision

        executable = runtime / "attention-pacing-codex"
        executable.write_text(
            f"#!{sys.executable}\n"
            "import os,socket,sys,time\n"
            "from pathlib import Path\n"
            "if sys.argv[1]=='app-server':\n"
            " time.sleep(.02)\n"
            " endpoint=sys.argv[3].removeprefix('unix://')\n"
            " server=socket.socket(socket.AF_UNIX)\n"
            " server.bind(endpoint)\n"
            " Path(endpoint+'.pid').write_text(str(os.getpid()))\n"
            " Path(endpoint+'.bound').touch()\n"
            " server.listen()\n"
            " while True:\n"
            "  connection,_=server.accept();connection.close()\n"
            "else:\n"
            " endpoint=sys.argv[2].removeprefix('unix://')\n"
            " deadline=time.monotonic()+5\n"
            " while not Path(endpoint).exists() and time.monotonic()<deadline:\n"
            "  time.sleep(.01)\n"
            " client=socket.socket(socket.AF_UNIX)\n"
            " client.connect(endpoint);client.close()\n"
            " print('READY',flush=True)\n"
            " for line in sys.stdin: print('ECHO:'+line.strip(),flush=True)\n"
        )
        executable.chmod(0o700)
        service = Service(
            binary,
            runtime,
            artifacts,
            "attention-pacing",
            str(executable),
            [],
            runtime,
            codex=True,
        )

        def read_attention(client, timeout=WAIT, predicate=lambda _: True):
            deadline = time.monotonic() + timeout
            publications = 0
            while time.monotonic() < deadline:
                try:
                    kind, data = client.receive(max(0.01, deadline - time.monotonic()))
                except (FrameDeadline, socket.timeout):
                    continue
                require(
                    kind in (SNAPSHOT, ATTENTION_SNAPSHOT, ATTENTION_RETRY, STATUS),
                    "Unexpected frame while reading attention pacing",
                )
                if kind == ATTENTION_SNAPSHOT:
                    require(
                        data[4:44] == client.attachment,
                        f"Attention attachment mismatch: {client.attachment.hex()} != {data[4:44].hex()}",
                    )
                    require(
                        struct.unpack_from(">I", data)[0] == VERSION,
                        "Attention version mismatch",
                    )
                    offset = 44 + 3
                    offset += 1
                    epoch = struct.unpack_from(">Q", data, offset)[0]
                    offset += 8
                    length = struct.unpack_from(">I", data, offset)[0]
                    offset += 4
                    diagnostic = data[offset : offset + length].decode()
                    offset += length
                    count = struct.unpack_from(">I", data, offset)[0]
                    snapshot = {
                        "connected": bool(data[45]),
                        "ready": bool(data[46]),
                        "epoch": epoch,
                        "diagnostic": diagnostic,
                        "requests": count,
                    }
                    publications += 1
                    if predicate(snapshot):
                        return snapshot, publications
            raise CheckError("Attention publication deadline expired")

        def wait_marker(suffix):
            deadline = time.monotonic() + WAIT
            marker = Path(str(service.endpoint) + suffix)
            while not marker.exists():
                require(time.monotonic() < deadline, "Attention fixture marker missing")
                time.sleep(0.01)
            return marker

        try:
            wait_marker(".codex.bound")
            pid = int(wait_marker(".codex.pid").read_text())
            with service.connect() as client:
                initial, _ = read_attention(client)
                require(
                    initial["requests"] == 0,
                    "Unqualified source did not publish initial attention",
                )
                # Send one input batch without a Python scheduling gap between
                # decisions: one write keeps the two revisions a single burst.
                # The attachment prefix repeats WireClient.send's invariant, so
                # a connection without a completed attach fails here as a
                # check error instead of a bare TypeError.
                require(
                    client.attachment is not None,
                    "No accepted attachment",
                )
                decisions = []
                for revision in (1, 2):
                    decisions.append(
                        frame(
                            ATTENTION_DECISION,
                            client.attachment
                            + make_attention_decision(
                                {
                                    "epoch": max(1, initial["epoch"]),
                                    "id": 1,
                                    "revision": revision,
                                },
                                "allow",
                            ),
                        )
                    )
                client.socket.sendall(b"".join(decisions))
                # Writing the decisions does not prove the service consumed
                # them. If exit were raced ahead of the decision publication,
                # stop_codex() would clear decision_error_ and publish the
                # exit diagnostic, and the late decisions would set
                # decision_error_ again; publish_attention() prioritizes it
                # over codex_error_, hiding the exit diagnostic from the wait
                # below and leaking a decision publication into the quiet
                # window. Observe the decision batch first, then treat source
                # exit as its own publication phase.
                decision_snapshot, decision_publications = read_attention(client)
                # The source has not been stopped yet, so this publication is
                # the batch's; exit text here would mean the phases crossed.
                require(
                    "Codex server exited" not in decision_snapshot["diagnostic"],
                    "Exit diagnostic arrived before the source was stopped",
                )
                require(
                    decision_snapshot["requests"] == initial["requests"],
                    "Decision batch changed the pending request count",
                )
                # A first-hit read always returns 1, so it cannot show the
                # batch produced exactly one publication; a quiet window with
                # no further attention frames can.
                quiet = time.monotonic() + 0.04
                while (remaining := quiet - time.monotonic()) > 0:
                    try:
                        kind, _ = client.receive(remaining)
                    except (FrameDeadline, socket.timeout):
                        break
                    require(
                        kind != ATTENTION_SNAPSHOT,
                        "Decision batch produced more than one publication",
                    )
                os.kill(pid, signal.SIGTERM)
                latest, publications = read_attention(
                    client,
                    predicate=lambda snapshot: (
                        "Codex server exited" in snapshot["diagnostic"]
                    ),
                )
                require(
                    publications == 1,
                    f"Source exit produced {publications} publications",
                )
                deadline = time.monotonic() + 0.04
                while (remaining := deadline - time.monotonic()) > 0:
                    try:
                        kind, _ = client.receive(remaining)
                    except (FrameDeadline, socket.timeout):
                        break
                    require(
                        kind != ATTENTION_SNAPSHOT,
                        "Unchanged attention produced another publication",
                    )
            return {
                "attention_decision_publications": decision_publications,
                "attention_publications_through_exit": publications,
                "newest_state": "source-exit",
            }
        finally:
            service.stop()

    def delayed_codex_listener():
        # This disposable executable is deliberately unqualified: terminal startup
        # still works, while structured attention remains disabled.
        executable = runtime / "delayed-codex"
        executable.write_text(
            f"#!{sys.executable}\n"
            "import socket,sys,time\n"
            "from pathlib import Path\n"
            "if sys.argv[1]=='app-server':\n"
            " endpoint=sys.argv[3].removeprefix('unix://')\n"
            " server=socket.socket(socket.AF_UNIX)\n"
            " server.bind(endpoint)\n"
            " Path(endpoint+'.bound').touch()\n"
            " time.sleep(.5)\n"
            " server.listen()\n"
            " while True:\n"
            "  connection,_=server.accept();connection.close()\n"
            "else:\n"
            " client=socket.socket(socket.AF_UNIX)\n"
            " client.connect(sys.argv[2].removeprefix('unix://'))\n"
            " client.close()\n"
            " print('LISTENER_READY',flush=True)\n"
            " for line in sys.stdin: print('ECHO:'+line.strip(),flush=True)\n"
        )
        executable.chmod(0o700)
        service = Service(
            binary,
            runtime,
            artifacts,
            "delayed-codex",
            str(executable),
            [],
            runtime,
            codex=True,
        )

        def terminal_text(client, text):
            if client.cached_snapshot and text in client.cached_snapshot["text"]:
                return
            deadline = time.monotonic() + WAIT
            while time.monotonic() < deadline:
                kind, data = client.receive(max(0.01, deadline - time.monotonic()))
                if kind == ATTENTION_SNAPSHOT:
                    continue
                require(
                    kind == SNAPSHOT and data[:40] == client.attachment,
                    "Unexpected frame during delayed startup",
                )
                if text in decode_snapshot(data[72:])["text"]:
                    return
            raise CheckError("Delayed backend terminal text missing")

        try:
            deadline = time.monotonic() + WAIT
            while not Path(str(service.endpoint) + ".codex.bound").exists():
                require(time.monotonic() < deadline, "Fake backend never bound")
                time.sleep(0.01)
            with service.connect() as client:
                terminal_text(client, "LISTENER_READY")
            with service.connect() as client:
                client.send(TEXT, b"reattached\n")
                terminal_text(client, "ECHO:reattached")
        finally:
            service.stop()

    def failures():
        for name, executable, cwd in [
            ("missing-program", "/nonexistent/lapis", runtime),
            ("missing-cwd", program, runtime / "missing"),
        ]:
            with (artifacts / (name + ".log")).open("wb") as log:
                result = subprocess.run(
                    [
                        str(binary),
                        str(runtime / (name + ".sock")),
                        str(cwd),
                        executable,
                    ],
                    stdout=log,
                    stderr=log,
                    timeout=WAIT,
                    check=False,
                )
            require(result.returncode != 0, "Invalid launch succeeded")
            require(
                not (runtime / (name + ".sock")).exists(),
                "Invalid launch left a listener",
            )

    def invalid_resize_and_signal():
        with session("resize-failure") as service:
            with service.connect() as client:
                client.snapshot(lambda s: "READY" in s["text"])
                client.send(ATTENTION_DECISION, b"")
                require(
                    "unsupported for terminal" in client.status(),
                    "Terminal-only attention decision was not rejected",
                )
            with service.connect() as client:
                client.snapshot(lambda s: "READY" in s["text"])
                client.send(RESIZE, struct.pack(">HH", 65535, 65535))
                require("geometry" in client.status(), "Invalid resize not rejected")
            with service.connect() as client:
                client.send(TEXT, b"size\n")
                client.snapshot(lambda s: "SIZE=100x30" in s["text"])
                os.kill(service.child_pid, signal.SIGTERM)
                require(
                    "signal (15)" in client.status(),
                    "Signal termination was mislabeled",
                )
                require(
                    service.process.wait(timeout=WAIT) == 143,
                    "Signal exit policy was lost",
                )

    def snapshot_limit():
        source = (
            "import signal,sys\n"
            "def clear(*args): print('\\x1b[2J\\x1b[HRECOVERED',flush=True)\n"
            "signal.signal(signal.SIGUSR1,clear)\n"
            "print('READY',flush=True)\n"
            "for line in sys.stdin:\n"
            " if line.strip()=='grow': print(('a'+'\\u0301'*8)*20000,flush=True)\n"
        )
        service = Service(
            binary,
            runtime,
            artifacts,
            "snapshot-limit",
            program,
            ["-u", "-c", source],
            runtime,
        )
        try:
            with service.connect() as client:
                client.snapshot(lambda s: "READY" in s["text"])
                client.send(RESIZE, struct.pack(">HH", 300, 100))
                client.snapshot(lambda s: s["columns"] == 300)
                client.send(TEXT, b"grow\n")
                require(
                    "Snapshot limit exceeded" in client.status(),
                    "Snapshot cap not reported",
                )
                require(
                    service.process.poll() is None, "Snapshot cap killed the session"
                )
            os.kill(service.child_pid, signal.SIGUSR1)
            time.sleep(0.1)
            with service.connect() as client:
                client.snapshot(lambda s: "RECOVERED" in s["text"])
        finally:
            service.stop()

    def identity_boundaries():
        with session("identity") as service:
            with service.connect() as initial:
                initial.snapshot(lambda s: "READY" in s["text"])
                old = initial.attachment
            with service.connect() as current:
                require(current.attachment[:32] == old[:32], "Identity changed")
                current.socket.sendall(frame(TEXT, old + b"STALE_INPUT\n"))
                require("Stale" in current.status(), "Stale generation accepted")
            with service.connect() as restored:
                restored.send(TEXT, b"fresh-input\n")
                screen = restored.snapshot(lambda s: "ECHO:fresh-input" in s["text"])
                require(
                    "STALE_INPUT" not in screen["text"], "Stale bytes reached child"
                )
                active = restored.attachment
                with WireClient(service.endpoint) as wrong:
                    wrong.send(
                        ATTACH,
                        attach_payload(
                            program, arguments, runtime, bytes([42]) * 32 + bytes(8)
                        ),
                    )
                    require(
                        "identity" in wrong.status(), "Replacement identity accepted"
                    )
                with WireClient(service.endpoint) as racing_creator:
                    creation = attach_payload(program, arguments, runtime)
                    creation = creation[:36] + bytes([2]) + bytes([43]) * 16 + bytes(16)
                    racing_creator.send(ATTACH, creation)
                    require(
                        "identity" in racing_creator.status(),
                        "Racing creation displaced existing session",
                    )
                restored.send(TEXT, b"still-active\n")
                restored.snapshot(lambda s: "ECHO:still-active" in s["text"])
                require(restored.attachment == active, "Bad attachment replaced client")
            with service.connect() as stale_resize:
                stale_resize.socket.sendall(
                    frame(RESIZE, old + struct.pack(">HH", 40, 10))
                )
                require("Stale" in stale_resize.status(), "Stale resize accepted")
            with service.connect() as restored:
                restored.send(TEXT, b"size\n")
                restored.snapshot(lambda s: "SIZE=100x30" in s["text"])
                restored.send(PASTE, b"x" * (65536 + 1))
                require(restored.status(), "Oversized paste not rejected")
            with service.connect() as restored:
                restored.send(TEXT, b"after-paste\n")
                screen = restored.snapshot(lambda s: "ECHO:after-paste" in s["text"])
                require("xxx" not in screen["text"], "Oversized paste reached child")
        return {
            "session_id": old[:16].hex(),
            "epoch": old[16:32].hex(),
            "initial_generation": struct.unpack(">Q", old[32:])[0],
            "final_generation": struct.unpack(">Q", service.attachment[32:])[0],
            "same_child_pid": service.child_pid,
        }

    def synchronization_boundaries():
        with session("synchronization") as service:
            with service.connect() as initial:
                initial.snapshot(lambda s: "READY" in s["text"])
            with WireClient(service.endpoint) as early:
                early.send(ATTACH, attach_payload(program, arguments, runtime))
                early.hello()
                early.send(TEXT, b"BEFORE_READY\n")
                require("ready" in early.status(), "Input before ready accepted")
            with WireClient(service.endpoint) as fragmented:
                packet = frame(ATTACH, attach_payload(program, arguments, runtime))
                for chunk in [packet[:4], packet[4:17], packet[17:-1]]:
                    fragmented.socket.sendall(chunk)
                    time.sleep(0.01)
                fragmented.socket.sendall(packet[-1:])
                fragmented.hello()
                screen = fragmented.next_snapshot()
                require(
                    "BEFORE_READY" not in screen["text"],
                    "Pre-ready input reached child",
                )
                fragmented.send(
                    READY,
                    fragmented.attachment + struct.pack(">Q", fragmented.sequence),
                )
                fragmented.send(TEXT, b"fragmented-ok\n")
                fragmented.snapshot(lambda s: "ECHO:fragmented-ok" in s["text"])
            for version, payload in (
                (
                    2,
                    struct.pack(">II", 2, 32)
                    + fingerprint(program, arguments, runtime),
                ),
                (
                    VERSION - 1,
                    struct.pack(">I", VERSION - 1)
                    + attach_payload(program, arguments, runtime)[4:],
                ),
            ):
                with WireClient(service.endpoint) as legacy:
                    legacy.send(ATTACH, payload)
                    require(
                        "incompatible" in legacy.status(),
                        f"v{version} attachment accepted",
                    )
            with WireClient(service.endpoint) as idle:
                idle.send(ATTACH, attach_payload(program, arguments, runtime))
                idle.hello()
                # Do not acknowledge the screen. Bounded timeout retires this client.
                require(
                    "acknowledgement timed out" in idle.status(),
                    "Missing acknowledgement not bounded",
                )
            with service.connect() as restored:
                restored.send(TEXT, b"after-timeout\n")
                restored.snapshot(lambda s: "ECHO:after-timeout" in s["text"])

    def paste_admission():
        output = runtime / "paste-input.bin"
        code = """import os, signal, sys, tty
from pathlib import Path
tty.setraw(0)
signal.signal(signal.SIGUSR1, lambda *_: os.write(1, b'\\x1b[?2004lMODEOFF\\r\\n'))
os.write(1, b'\\x1b[?2004hREADY\\r\\n')
with open(sys.argv[1], 'wb', buffering=0) as output:
    while True:
        data = os.read(0, 65536)
        if not data:
            break
        output.write(data)
"""
        argv = ["-u", "-c", code, str(output)]
        service = Service(
            binary, runtime, artifacts, "paste-admission", program, argv, runtime
        )

        def wait_bytes(expected):
            deadline = time.monotonic() + WAIT
            while time.monotonic() < deadline:
                if output.exists() and output.stat().st_size >= len(expected):
                    require(
                        output.read_bytes() == expected,
                        "Paste bytes were partial, duplicated or mis-encoded",
                    )
                    return
                time.sleep(0.01)
            raise CheckError("Paste bytes did not reach the reader")

        def request(client, request_id, text, submit=False):
            client.socket.sendall(
                frame(
                    PASTE_REQUEST,
                    client.attachment
                    + struct.pack(">QB", request_id, int(submit))
                    + text,
                )
            )
            deadline = time.monotonic() + WAIT
            while time.monotonic() < deadline:
                kind, payload = client.receive(max(0.01, deadline - time.monotonic()))
                if kind == SNAPSHOT:
                    continue
                require(
                    kind == PASTE_RESULT and payload[:40] == client.attachment,
                    "Wrong paste receipt attachment",
                )
                require(
                    struct.unpack_from(">Q", payload, 40)[0] == request_id,
                    "Wrong paste receipt ID",
                )
                return bool(payload[48]), payload[49:].decode()
            raise CheckError("Paste receipt timed out")

        try:
            wait_socket(service.endpoint, service.process)
            with WireClient(service.endpoint) as client:
                service.child_pid = client.attach(
                    program, argv, runtime, paste_transactions=True
                )
                client.snapshot(lambda screen: "READY" in screen["text"])
                os.kill(service.child_pid, signal.SIGSTOP)
                filler = b"f" * (256 * 1024)
                for offset in range(0, len(filler), 65536):
                    client.send(TEXT, filler[offset : offset + 65536])
                queued, message = request(client, 1, b"p" * (960 * 1024))
                require(
                    not queued and "queue" in message,
                    "Prefilled PTY queue admitted an incomplete paste",
                )
                os.kill(service.child_pid, signal.SIGCONT)
                wait_bytes(filler)
                large = b"p" * (960 * 1024)
                require(
                    request(client, 2, large)[0], "Empty queue rejected a maximum paste"
                )
                expected = filler + b"\x1b[200~" + large + b"\x1b[201~"
                wait_bytes(expected)
                require(
                    request(client, 3, b"go", True)[0], "Paste-and-submit was refused"
                )
                expected += b"\x1b[200~go\x1b[201~\r"
                wait_bytes(expected)
                os.kill(service.child_pid, signal.SIGUSR1)
                client.snapshot(lambda screen: "MODEOFF" in screen["text"])
                require(request(client, 4, b"one\ntwo")[0], "Plain paste refused")
                expected += b"one\rtwo"
                wait_bytes(expected)
                with WireClient(service.endpoint) as joined:
                    pid = joined.attach(
                        program, argv, runtime, paste_transactions=True, join=True
                    )
                    require(
                        pid == service.child_pid, "Joined paste started another child"
                    )
                    require(request(joined, 1, b"joined")[0], "Joined paste rejected")
                    expected += b"joined"
                    wait_bytes(expected)
                # An incomplete frame must never send its prefix to the PTY.
                with WireClient(service.endpoint) as partial:
                    partial.attach(
                        program, argv, runtime, paste_transactions=True, join=True
                    )
                    packet = frame(
                        PASTE_REQUEST,
                        partial.attachment + struct.pack(">QB", 1, 0) + b"not-written",
                    )
                    partial.socket.sendall(packet[:-1])
                require(
                    request(client, 5, b"after-partial")[0],
                    "Partial peer broke primary input",
                )
                expected += b"after-partial"
                wait_bytes(expected)
                return {
                    "max_paste_bytes": len(large),
                    "pty_sha256": hashlib.sha256(expected).hexdigest(),
                    "prefilled_refused_whole": True,
                    "joined_and_partial_frame": True,
                }
        finally:
            if service.child_pid:
                try:
                    os.kill(service.child_pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            service.stop()

    def claude_idle_paste_admission():
        output = runtime / "claude-idle-paste.bin"
        executable = runtime / "claude-idle-hooks"
        executable.write_text(
            f"#!{program}\n"
            "import json, os, subprocess, sys\n"
            "from pathlib import Path\n"
            "settings = Path(sys.argv[sys.argv.index('--settings') + 1])\n"
            "hooks = json.loads(settings.read_text())['hooks']\n"
            "command = hooks['SessionStart'][0]['hooks'][0]['command']\n"
            "def send(event):\n"
            "    source = json.dumps(event) + '\\n'\n"
            "    subprocess.run(command, shell=True, input=source.encode(), check=True)\n"
            "send({'hook_event_name': 'SessionStart', 'session_id': 'idle-paste'})\n"
            "send({'hook_event_name': 'UserPromptSubmit', 'session_id': 'idle-paste', 'prompt_id': 'turn-1'})\n"
            "send({'hook_event_name': 'Notification', 'session_id': 'idle-paste', 'prompt_id': 'turn-1', 'notification_type': 'idle_prompt'})\n"
            "print('READY', flush=True)\n"
            "with open(sys.argv[-1], 'wb', buffering=0) as captured:\n"
            "    buffered = b''\n"
            "    while True:\n"
            "        data = os.read(0, 65536)\n"
            "        if not data:\n"
            "            break\n"
            "        captured.write(data)\n"
            "        buffered += data\n"
            "        if b'permission' in buffered:\n"
            "            send({'hook_event_name': 'Notification', 'session_id': 'idle-paste', 'prompt_id': 'turn-1', 'notification_type': 'permission_prompt'})\n"
            "            print('PERMISSION_READY', flush=True)\n"
            "            break\n"
        )
        executable.chmod(0o700)
        argv = ["--claude-fixture", str(output)]
        service = Service(
            binary,
            runtime,
            artifacts,
            "claude-idle-paste",
            str(executable),
            argv,
            runtime,
            claude=True,
        )

        def wait_attention_requests(client, expected):
            deadline = time.monotonic() + WAIT
            while time.monotonic() < deadline:
                kind, data = client.receive(max(0.01, deadline - time.monotonic()))
                if kind != ATTENTION_SNAPSHOT:
                    continue
                require(
                    data[4:44] == client.attachment,
                    "Claude attention attachment mismatch",
                )
                diagnostic_length = struct.unpack_from(">I", data, 56)[0]
                count = struct.unpack_from(">I", data, 60 + diagnostic_length)[0]
                if count == expected:
                    return count
            raise CheckError("Managed Claude attention publication timed out")

        def request(client, request_id, text, submit=False):
            client.socket.sendall(
                frame(
                    PASTE_REQUEST,
                    client.attachment
                    + struct.pack(">QB", request_id, int(submit))
                    + text,
                )
            )
            deadline = time.monotonic() + WAIT
            while time.monotonic() < deadline:
                kind, payload = client.receive(max(0.01, deadline - time.monotonic()))
                if kind in (SNAPSHOT, ATTENTION_SNAPSHOT):
                    continue
                require(
                    kind == PASTE_RESULT and payload[:40] == client.attachment,
                    "Wrong Claude paste receipt attachment",
                )
                require(
                    struct.unpack_from(">Q", payload, 40)[0] == request_id,
                    "Wrong Claude paste receipt ID",
                )
                return bool(payload[48]), payload[49:].decode()
            raise CheckError("Claude paste receipt timed out")

        try:
            wait_socket(service.endpoint, service.process)
            with WireClient(service.endpoint) as client:
                service.child_pid = client.attach(
                    str(executable), argv, runtime, paste_transactions=True, claude=True
                )
                wait_attention_requests(client, 1)
                queued, _ = request(client, 1, b"idle-prompt", True)
                require(
                    queued, "Current observation-only idle notice blocked submission"
                )
                client.send(TEXT, b"permission\n")
                wait_attention_requests(client, 2)
                queued, message = request(client, 2, b"blocked", True)
                require(
                    not queued
                    and message
                    == "Agent has a pending request; automatic paste-plus-Return was refused",
                    "Permission prompt did not block submission",
                )
                return {
                    "idle_notice_admitted": True,
                    "permission_notice_refused": True,
                }
        finally:
            service.stop()

    def backpressure():
        with session("input-backpressure") as service:
            with service.connect() as client:
                client.snapshot(lambda screen: "READY" in screen["text"])
                client.send(TEXT, b"pause\n")
                client.snapshot(lambda screen: "PAUSED" in screen["text"])
                try:
                    # Stop the reader, then exceed the bounded PTY queue with whole messages.
                    packet = frame(TEXT, client.attachment + b"q" * 65536)
                    try:
                        client.socket.sendall(packet * 24)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    require(
                        "PTY input queue full" in client.status(),
                        "Queue overflow was silent",
                    )
                    require(
                        service.process.poll() is None, "Queue overflow killed service"
                    )
                finally:
                    os.kill(service.child_pid, signal.SIGCONT)
            # A fresh child isolates output retention from the preceding input
            # queue overflow. A fresh viewer consumes snapshots while the child
            # produces more than the 64 KiB PTY admission bound.
        with session("output-overflow") as service:
            slow = service.connect()
            slow.snapshot(lambda screen: "READY" in screen["text"])
            slow.send(TEXT, b"overflow\n")
            # The service pauses inside the first admission batch once retained
            # output reaches its boundary. The child then blocks in the PTY on
            # this fixed burst; recovery does not depend on an enormous write.
            deadline = time.monotonic() + WAIT
            while "PTY output retention paused" not in service.log_text():
                if time.monotonic() >= deadline:
                    raise CheckError("Output retention deadline expired")
                time.sleep(0.01)
            slow.close()
            with service.connect() as recovered:
                os.kill(service.child_pid, signal.SIGCONT)
                recovered.snapshot(
                    lambda screen: (
                        "ORDER_START" in screen["text"]
                        and "ORDER_END" in screen["text"]
                        and "OVERFLOW_WRITTEN" in screen["text"]
                    )
                )
                diagnostic = service.log_text()
                require(
                    "PTY output retention drained" in diagnostic,
                    "Output retention did not report recovery",
                )
                os.kill(service.child_pid, signal.SIGCONT)
                recovered.send(TEXT, b"after-output-overflow\n")
                recovered.snapshot(
                    lambda screen: "ECHO:after-output-overflow" in screen["text"]
                )
        with session("nonreading") as service:
            wait_socket(service.endpoint, service.process)
            with WireClient(service.endpoint) as blocked:
                blocked.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                blocked.send(ATTACH, attach_payload(program, arguments, runtime))
                time.sleep(3.3)  # Deliberately do not read hello or the initial screen.
                # A non-reader may lose a partial screen when the bounded drain
                # expires; require closure, not delivery of the trailing status.
                received = 0
                deadline = time.monotonic() + WAIT
                while True:
                    blocked.socket.settimeout(max(0.01, deadline - time.monotonic()))
                    chunk = blocked.socket.recv(65536)
                    if not chunk:
                        break
                    received += len(chunk)
                    require(
                        received <= 8 * 1024 * 1024 + 8192,
                        "Unbounded non-reader output",
                    )
                    require(time.monotonic() < deadline, "Non-reader not disconnected")
            with service.connect() as restored:
                restored.send(TEXT, b"after-nonreader\n")
                restored.snapshot(
                    lambda screen: "ECHO:after-nonreader" in screen["text"]
                )
        return {
            "input_queue_whole_refusal": True,
            "output_overflow_recovery": True,
            "output_retention_recovery": True,
            "bounded_non_reader_disconnect": True,
        }

    def history_backpressure_receive_batch():
        with session("history-backpressure") as service:
            with service.connect() as client:
                client.snapshot(lambda screen: "READY" in screen["text"])
                client.send(
                    HISTORY_REQUEST,
                    history_request_payload(1),
                )
                require(
                    "No more archived history" in client.history_reply()["message"],
                    "Initial history response was not observed",
                )
                # Exceed 8 MiB plus socket buffering without draining replies.
                packet = b"".join(
                    frame(
                        HISTORY_REQUEST,
                        client.attachment + history_request_payload(request_id),
                    )
                    for request_id in range(2, 200002)
                )
                try:
                    client.socket.sendall(packet)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                time.sleep(1)  # Deliberately withhold reads to fill the reply queue.
                deadline = time.monotonic() + WAIT
                received_bytes = 0
                while True:
                    remaining = deadline - time.monotonic()
                    require(remaining > 0, "Backpressured client did not disconnect")
                    client.socket.settimeout(remaining)
                    try:
                        chunk = client.socket.recv(65536)
                    except ConnectionResetError:
                        # Linux may reset a peer closed with unread request bytes.
                        break
                    if not chunk:
                        break
                    received_bytes += len(chunk)
                require(
                    service.process.poll() is None,
                    f"History backpressure killed service (exit {service.process.returncode})",
                )
            with service.connect() as restored:
                restored.send(TEXT, b"after-history-backpressure\n")
                restored.snapshot(
                    lambda screen: "ECHO:after-history-backpressure" in screen["text"]
                )
                return {
                    "requests_sent_maximum": 200000,
                    "reply_bytes_drained": received_bytes,
                    "same_child_pid": service.child_pid,
                    "reattached_and_echoed": True,
                }

    def replacement_service():
        with session("replacement") as service, service.connect() as old:
            identity = old.attachment
            old.send(TEXT, b"quit\n")
            require("Process exited" in old.status(), "Old service did not exit")
            service.process.wait(timeout=WAIT)
        with session("replacement") as replacement, replacement.connect() as current:
            require(
                current.attachment[:32] != identity[:32],
                "Replacement reused identity",
            )
            with WireClient(replacement.endpoint) as stale:
                stale.send(
                    ATTACH, attach_payload(program, arguments, runtime, identity)
                )
                require(
                    "identity" in stale.status(),
                    "Saved identity attached to replacement",
                )
            current.send(TEXT, b"replacement-alive\n")
            current.snapshot(lambda s: "ECHO:replacement-alive" in s["text"])

    def gui():
        with session("gui") as service:
            with service.connect() as initial:
                initial.snapshot(lambda s: "READY" in s["text"])
            options = [
                "--discover",
                "--socket",
                str(service.endpoint),
                "--cwd",
                str(runtime),
                "--capture",
                str(artifacts / "gui.png"),
                "--trace",
                str(artifacts / "gui.json"),
                "--capture-delay",
                "500",
                "--",
                program,
                *arguments,
            ]
            run_capture(desktop, options, artifacts, "gui")
            require(
                (artifacts / "gui.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n"),
                "Missing PNG",
            )
            json.loads((artifacts / "gui.json").read_text())
            with service.connect() as restored:
                restored.snapshot(lambda s: "READY" in s["text"])
        # A shell smoke uses the default shell launch on a dedicated endpoint.
        endpoint = runtime / "shell.sock"
        try:
            run_capture(
                desktop,
                [
                    "--socket",
                    str(endpoint),
                    "--new-session",
                    "--smoke-input",
                    "--capture",
                    str(artifacts / "shell.png"),
                ],
                artifacts,
                "shell",
            )
        finally:
            original_error = sys.exc_info()[1]
            if endpoint.exists():
                configured_shell = default_shell()
                shell = os.path.abspath(
                    shutil.which(configured_shell) or configured_shell
                )
                try:
                    with WireClient(endpoint) as client:
                        client.attach(shell, ["-i"], ROOT)
                        client.send(TEXT, b"exit\n")
                        require(
                            "Process exited (0)" in client.status(),
                            "Default shell did not exit",
                        )
                except (
                    CheckError,
                    OSError,
                    ValueError,
                    EOFError,
                    struct.error,
                ) as error:
                    if original_error is None:
                        raise
                    original_error.add_note(f"shell cleanup also failed: {error}")
        for index, options in enumerate(
            [
                ["--ui-preview", "--socket", str(runtime / "rejected.sock")],
                ["--cwd", str(runtime)],
                [
                    "--socket",
                    str(runtime / "rejected.sock"),
                    "--smoke-input",
                    "--",
                    program,
                ],
                ["--socket", ""],
                ["--new-session"],
                ["--discover"],
            ]
        ):
            run_capture(
                desktop, options, artifacts, f"invalid-{index}", expect_success=False
            )

    def codex_terminal():
        executable = str(Path(shutil.which(codex) or codex).resolve(strict=True))
        # Codex 0.154.0 removed `--no-daemon`; the plain TUI is the owned backend.
        cli_arguments = []
        service = Service(
            binary, runtime, artifacts, "codex", executable, cli_arguments, ROOT
        )
        marker = "lapis_probe_input"
        edited = marker[:-1] + "X" + marker[-1] + "_paste"
        try:
            with service.connect() as client:
                client.snapshot(lambda s: "OpenAI Codex" in s["text"], timeout=15)
                wait_raw_terminal(service.child_pid)
                client.send(TEXT, marker.encode())
                client.snapshot(lambda s: marker in s["text"])
                client.send(KEY, bytes([2, 0]))  # TerminalKey::left
                client.send(TEXT, b"X")
                client.send(KEY, bytes([3, 0]))  # TerminalKey::right
                client.send(PASTE, b"_paste")
                client.snapshot(lambda s: edited in s["text"])
                client.send(RESIZE, struct.pack(">HH", 93, 27))
                client.snapshot(
                    lambda s: (
                        s["columns"] == 93 and s["rows"] == 27 and edited in s["text"]
                    )
                )
            if desktop_enabled:
                for name, extra in [("codex", []), ("codex-compact", ["--compact"])]:
                    run_capture(
                        desktop,
                        [
                            "--socket",
                            str(service.endpoint),
                            "--cwd",
                            str(ROOT),
                            "--capture",
                            str(artifacts / (name + ".png")),
                            "--trace",
                            str(artifacts / (name + ".json")),
                            "--capture-delay",
                            "500",
                            *extra,
                            "--",
                            executable,
                            *cli_arguments,
                        ],
                        artifacts,
                        name,
                    )
                    require(
                        (artifacts / (name + ".png"))
                        .read_bytes()
                        .startswith(b"\x89PNG\r\n\x1a\n"),
                        "Missing Codex PNG",
                    )
            with service.connect() as restored:
                restored.snapshot(lambda s: edited in s["text"])
                # No Enter is sent: this clears the unsubmitted text and exits.
                restored.send(TEXT, b"\x03")
                restored.snapshot(lambda s: edited not in s["text"])
                # Empty-composer Ctrl-D uses the CLI quit shortcut; unlike an
                # extra Ctrl-C it cannot become SIGINT after terminal restoration.
                restored.send(TEXT, b"\x04\x04")
                require(
                    service.process.wait(timeout=15) == 0,
                    "Codex did not exit cleanly",
                )
        finally:
            service.stop()

    entries = [
        (
            "OSC 8 metadata negotiates independently for legacy and joined clients",
            hyperlink_capability,
            None,
        ),
        (
            "invalid resize preserves geometry and signal exit status",
            invalid_resize_and_signal,
            None,
        ),
        (
            "identity generations reject stale input and preserve active clients",
            identity_boundaries,
            None,
        ),
        (
            "initial screen acknowledgement, fragmentation and protocol version boundaries",
            synchronization_boundaries,
            None,
        ),
        (
            "Atomic paste admission, service encoding and ordered submit",
            paste_admission,
            None,
        ),
        (
            "Managed Claude idle notices submit while permission prompts refuse",
            claude_idle_paste_admission,
            None,
        ),
        ("PTY queue overflow and non-reading attachment", backpressure, None),
        (
            "history backpressure detaches only the owning receive client",
            history_backpressure_receive_batch,
            None,
        ),
        ("replacement service rejects remembered identity", replacement_service, None),
        ("snapshot limit preserves child and permits recovery", snapshot_limit, None),
        ("literal argv, cwd, resize, paste and exit", literal_resize_exit, None),
        ("detached output and same-PID reattachment", detach, None),
        ("mismatch and malformed attachment preserve active client", mismatch, None),
        ("failed executable and cwd", failures, None),
        ("Codex config values and literal separator", codex_config_arguments, None),
        (
            "Codex permissions reach the server and leave a resuming TUI",
            codex_resume_permissions,
            None,
        ),
        (
            "Codex backend exit diagnostics exclude private stderr",
            codex_backend_exit,
            None,
        ),
        (
            "attention bursts publish the newest state at the pacing deadline",
            attention_publication_pacing,
            None,
        ),
        (
            "Codex bind-before-listen startup and reattachment",
            delayed_codex_listener,
            None,
        ),
        (
            "desktop capture, reattachment, shell default and option rejection",
            gui,
            "desktop",
        ),
        (
            "installed Codex TUI input, navigation, paste, resize, reattach and interrupt",
            codex_terminal,
            "codex",
        ),
    ]
    return entries


def case_catalog(build):
    # Defining actions is inert: no runtime, artifact, or tool is opened.
    return [
        {
            "case": action.__name__.replace("_", "-"),
            "description": name,
            "requires": needed,
        }
        for name, action, needed in _case_actions(build, None, None, False)
    ]


def exercise(build, runtime, artifacts, desktop_enabled, codex=None, *, cases=None):
    results = []

    def record(name, action):
        started = time.monotonic()
        try:
            observed = action()
            result = {"name": name, "passed": True}
            if observed is not None:
                result["observed"] = observed
        except (
            CheckError,
            OSError,
            ValueError,
            EOFError,
            struct.error,
            subprocess.SubprocessError,
        ) as error:
            result = {
                "name": name,
                "passed": False,
                "error": f"{type(error).__name__}: {error}",
                "traceback": traceback.format_exc(),
            }
        result["seconds"] = round(time.monotonic() - started, 3)
        result["case"] = action.__name__.replace("_", "-")
        results.append(result)
        print(json.dumps(result), flush=True)

    entries = _case_actions(build, runtime, artifacts, desktop_enabled, codex)
    for name, action, needed in entries:
        key = action.__name__.replace("_", "-")
        if cases and key not in cases:
            continue
        if needed == "desktop" and not desktop_enabled:
            continue
        if needed == "codex" and not codex:
            continue
        record(name, action)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/cli-launch-check/receipt.json"
    )
    parser.add_argument("--desktop", action="store_true")
    parser.add_argument(
        "--codex",
        nargs="?",
        const="codex",
        help="Optional installed Codex TUI probe; submits no prompt",
    )
    parser.add_argument(
        "--list-cases", action="store_true", help="List cases without starting tools"
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="Run only this case; repeat to select several",
    )
    args = parser.parse_args()
    catalog = case_catalog(args.build_dir)
    if args.list_cases:
        print(json.dumps(catalog, indent=2))
        return 0
    args.output.unlink(missing_ok=True)
    args.case = list(dict.fromkeys(args.case))
    known = {entry["case"]: entry for entry in catalog}
    for selected in args.case:
        if selected not in known:
            parser.error(f"Unknown case: {selected}; use --list-cases")
        needed = known[selected]["requires"]
        if needed and not getattr(args, needed):
            parser.error(f"Case {selected} requires --{needed}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.parent))
    receipt = {
        "schema": "lapis.cli-launch-check/1",
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "artifacts": str(artifacts),
        "build": str(args.build_dir.resolve()),
        "passed": False,
        "scope": "Controlled fixture, optional Qt captures and optional no-prompt Codex TUI; no agent attention qualification.",
    }
    try:
        if args.codex and (not args.case or "codex-terminal" in args.case):
            executable = Path(shutil.which(args.codex) or args.codex).resolve(
                strict=True
            )
            with executable.open("rb") as stream:
                receipt["codex"] = {
                    "sha256": hashlib.file_digest(stream, "sha256").hexdigest(),
                    "arguments": [],
                    "prompt_submitted": False,
                }
        # The fixture owns private disposable state. Keep its path short enough
        # for Unix sockets even when the checkout is a deeply nested worktree.
        with tempfile.TemporaryDirectory(prefix="lapis-cli-", dir="/tmp") as directory:
            receipt["checks"] = exercise(
                args.build_dir.resolve(),
                Path(directory).resolve(),
                artifacts,
                args.desktop,
                args.codex,
                cases=set(args.case),
            )
        receipt["selection_mode"] = "subset" if args.case else "all"
        receipt["selected_cases"] = [check["case"] for check in receipt["checks"]]
        receipt["passed"] = bool(receipt["checks"]) and all(
            check["passed"] for check in receipt["checks"]
        )
    except (
        CheckError,
        lapis.SetupError,
        OSError,
        ValueError,
        EOFError,
        struct.error,
        subprocess.SubprocessError,
    ) as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
    finally:
        args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Receipt: {args.output}")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
