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
import time
import traceback
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = 4
HELLO, SNAPSHOT, TEXT, PASTE, KEY, RESIZE, STATUS, ATTACH, READY = range(1, 10)
HISTORY_REQUEST, HISTORY_PAGE = range(10, 12)
WAIT = 5


class CheckError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CheckError(message)


def qt_string(value):
    encoded = str(value).encode("utf-16-be")
    return struct.pack(">I", len(encoded)) + encoded


def fingerprint(program, arguments, directory):
    data = qt_string(os.path.abspath(program)) + struct.pack(">I", len(arguments))
    data += b"".join(qt_string(argument) for argument in arguments)
    return hashlib.sha256(data + qt_string(Path(directory).resolve())).digest()


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


def attach_payload(program, arguments, directory, expected=None):
    identity = expected[:32] if expected is not None else bytes(32)
    return (
        struct.pack(">I", VERSION)
        + fingerprint(program, arguments, directory)
        + bytes([1 if expected is not None else 0])
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
        if kind in (TEXT, PASTE, KEY, RESIZE, HISTORY_REQUEST):
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
            require(remaining > 0, "Frame deadline expired")
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(65536)
            if not chunk:
                raise EOFError("Service disconnected")
            self.buffer.extend(chunk)

    def attach(self, program, arguments, directory, expected=None):
        self.send(ATTACH, attach_payload(program, arguments, directory, expected))
        pid = self.hello()
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
        require(kind == HELLO and len(data) == 52, "Expected hello after attachment")
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
    ):
        self.endpoint = runtime / (name + ".sock")
        self.program, self.arguments, self.directory = program, arguments, directory
        self.child_pid = None
        self.attachment = None
        self.log = (artifacts / (name + ".service.log")).open("wb")
        self.process = subprocess.Popen(
            [str(binary), str(self.endpoint), str(directory), program, *arguments],
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
                self.program, self.arguments, self.directory, self.attachment
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
        return self.log.read_text(errors="replace")


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


def exercise(build, runtime, artifacts, desktop_enabled, codex=None):
    binary = build / "services/session/lapis_session_service"
    desktop = build / "apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop"
    if sys.platform != "darwin":
        desktop = build / "apps/desktop/lapis_desktop"
    program = str(Path(sys.executable).resolve())
    values = ["has space", "meta*$=!;|<>", "quote'\\\"", "line\nbreak", "", "界😀"]
    values += ["-platform", "literal-not-a-platform", "--help"]
    arguments = ["-u", "-c", FIXTURE, *values]
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
        results.append(result)
        print(json.dumps(result), flush=True)

    @contextmanager
    def session(name):
        service = Service(binary, runtime, artifacts, name, program, arguments, runtime)
        try:
            yield service
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
            with WireClient(service.endpoint) as legacy:
                legacy.send(
                    ATTACH,
                    struct.pack(">II", 2, 32)
                    + fingerprint(program, arguments, runtime),
                )
                require("incompatible" in legacy.status(), "v2 attachment accepted")
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

    def backpressure():
        with session("backpressure") as service, service.connect() as client:
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
                require(service.process.poll() is None, "Queue overflow killed service")
            finally:
                os.kill(service.child_pid, signal.SIGCONT)
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
                    chunk = client.socket.recv(65536)
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

    record(
        "invalid resize preserves geometry and signal exit status",
        invalid_resize_and_signal,
    )
    record(
        "identity generations reject stale input and preserve active clients",
        identity_boundaries,
    )
    record(
        "initial screen acknowledgement, fragmentation and protocol version boundaries",
        synchronization_boundaries,
    )
    record("PTY queue overflow and non-reading attachment", backpressure)
    record(
        "history backpressure detaches only the owning receive client",
        history_backpressure_receive_batch,
    )
    record("replacement service rejects remembered identity", replacement_service)
    record("snapshot limit preserves child and permits recovery", snapshot_limit)
    record("literal argv, cwd, resize, paste and exit", literal_resize_exit)
    record("detached output and same-PID reattachment", detach)
    record("mismatch and malformed attachment preserve active client", mismatch)
    record("failed executable and cwd", failures)
    if desktop_enabled:
        record("desktop capture, reattachment, shell default and option rejection", gui)
    if codex:
        record(
            "installed Codex TUI input, navigation, paste, resize, reattach and interrupt",
            codex_terminal,
        )
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
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.parent))
    runtime_root = ROOT / "runtime"
    runtime_root.mkdir(exist_ok=True)
    receipt = {
        "schema": "lapis.cli-launch-check/1",
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "artifacts": str(artifacts),
        "build": str(args.build_dir.resolve()),
        "passed": False,
        "scope": "Controlled fixture, optional Qt captures and optional no-prompt Codex TUI; no agent attention qualification.",
    }
    try:
        if args.codex:
            executable = Path(shutil.which(args.codex) or args.codex).resolve(
                strict=True
            )
            with executable.open("rb") as stream:
                receipt["codex"] = {
                    "sha256": hashlib.file_digest(stream, "sha256").hexdigest(),
                    "arguments": [],
                    "prompt_submitted": False,
                }
        with tempfile.TemporaryDirectory(prefix="cli-", dir=runtime_root) as directory:
            receipt["checks"] = exercise(
                args.build_dir.resolve(),
                Path(directory),
                artifacts,
                args.desktop,
                args.codex,
            )
        receipt["passed"] = all(check["passed"] for check in receipt["checks"])
    except (
        CheckError,
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
