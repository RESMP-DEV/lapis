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
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = 2
HELLO, SNAPSHOT, TEXT, PASTE, KEY, RESIZE, STATUS, ATTACH = range(1, 9)
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


def attach_payload(program, arguments, directory):
    return struct.pack(">II", VERSION, 32) + fingerprint(program, arguments, directory)


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


class WireClient:
    def __init__(self, endpoint):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(WAIT)
        self.buffer = bytearray()
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

    def attach(self, program, arguments, directory):
        self.send(ATTACH, attach_payload(program, arguments, directory))
        kind, data = self.receive()
        require(kind == HELLO and len(data) == 12, "Expected hello after attachment")
        version, pid = struct.unpack(">IQ", data)
        require(version == VERSION and pid > 0, "Invalid hello identity")
        return pid

    def snapshot(self, predicate=lambda _: True, timeout=WAIT):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            kind, data = self.receive(max(0.01, deadline - time.monotonic()))
            require(kind == SNAPSHOT, "Expected terminal snapshot")
            snapshot = decode_snapshot(data)
            if predicate(snapshot):
                return snapshot
        raise CheckError("Snapshot condition not met")

    def status(self):
        deadline = time.monotonic() + WAIT
        while time.monotonic() < deadline:
            kind, data = self.receive(max(0.01, deadline - time.monotonic()))
            if kind == STATUS:
                return data.decode("utf-8")
            require(kind == SNAPSHOT, "Unexpected frame before status")
        raise CheckError("Status deadline expired")


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
    def __init__(self, binary, runtime, artifacts, name, program, arguments, directory):
        self.endpoint = runtime / (name + ".sock")
        self.program, self.arguments, self.directory = program, arguments, directory
        self.child_pid = None
        self.log = (artifacts / (name + ".service.log")).open("wb")
        self.process = subprocess.Popen(
            [str(binary), str(self.endpoint), str(directory), program, *arguments],
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=self.log,
            start_new_session=True,
        )

    def connect(self):
        wait_socket(self.endpoint, self.process)
        client = WireClient(self.endpoint)
        try:
            pid = client.attach(self.program, self.arguments, self.directory)
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


FIXTURE = r"""
import hashlib,json,os,signal,sys,termios,time
attr=termios.tcgetattr(0);attr[3]&=~termios.ECHO;termios.tcsetattr(0,termios.TCSANOW,attr)
print('ARGV='+hashlib.sha256(json.dumps(sys.argv[1:],ensure_ascii=False).encode()).hexdigest(),flush=True)
print('CWD='+hashlib.sha256(os.getcwd().encode()).hexdigest(),flush=True)
print('READY',flush=True)
for line in sys.stdin:
    command=line.rstrip('\n')
    if command=='quit': sys.exit(7)
    if command=='burst':
        time.sleep(.3)
        for i in range(12000): print('0123456789abcdef')
        print('DETACHED_DONE',flush=True)
    elif command=='size':
        size=os.get_terminal_size(0);print('SIZE=%dx%d'%(size.columns,size.lines),flush=True)
    else: print('ECHO:'+command,flush=True)
"""


def run_capture(desktop, options, artifacts, name, expect_success=True):
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
            action()
            result = {"name": name, "passed": True}
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

    def gui():
        with session("gui") as service:
            with service.connect() as initial:
                initial.snapshot(lambda s: "READY" in s["text"])
            options = [
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
                    "--smoke-input",
                    "--capture",
                    str(artifacts / "shell.png"),
                ],
                artifacts,
                "shell",
            )
        finally:
            if endpoint.exists():
                shell = os.path.abspath(
                    shutil.which(os.environ.get("SHELL", "/bin/sh"))
                )
                with WireClient(endpoint) as client:
                    client.attach(shell, ["-i"], ROOT)
                    client.send(TEXT, b"exit\n")
                    require(
                        "Process exited (0)" in client.status(),
                        "Default shell did not exit",
                    )
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
        cli_arguments = ["--no-daemon"]
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
                for _ in range(2):
                    if service.process.poll() is not None:
                        break
                    try:
                        restored.send(TEXT, b"\x03")
                    except (BrokenPipeError, ConnectionResetError):
                        break
                    time.sleep(0.2)
                require(
                    service.process.wait(timeout=WAIT) == 0,
                    "Codex did not exit cleanly",
                )
        finally:
            service.stop()

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
                    "arguments": ["--no-daemon"],
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
