"""Opt-in real Codex attention round trips against an isolated CCR/GLM fixture."""

import argparse
import asyncio
import fcntl
import hashlib
import json
import os
import pty
import shlex
import shutil
import struct
import tempfile
import termios
from datetime import datetime, timezone
from pathlib import Path

try:
    from .codex_probe_transport import UnixWebSocketTransport
except ImportError:
    from codex_probe_transport import UnixWebSocketTransport

ROOT = Path(__file__).resolve().parents[1]
MODEL = "zai,glm-5.3"
METHODS = {
    "input": "item/tool/requestUserInput",
    "approval": "item/commandExecution/requestApproval",
}


def approved_fixture(command):
    """Accept only the harmless fixture command, optionally in a known shell."""
    try:
        tokens = shlex.split(command)
        if (
            len(tokens) == 3
            and tokens[0] in ("/opt/homebrew/bin/bash", "/bin/bash", "/bin/zsh")
            and tokens[1] in ("-c", "-lc")
        ):
            tokens = shlex.split(tokens[2])
        return tokens == ["python3", "-c", "print(123456789)"]
    except (TypeError, ValueError):
        return False


def matches(message, method, thread, *, turn=None, request_id=None):
    if message.get("method") != method:
        return False
    params = message.get("params", {})
    if params.get("threadId") != thread:
        return False
    if turn is not None and params.get("turn", {}).get("id") != turn:
        return False
    if request_id is not None:
        actual = params.get("requestId")
        return type(actual) is type(request_id) and actual == request_id
    return True


class RpcError(RuntimeError):
    """Content-free source RPC error for capability probes."""

    def __init__(self, method, code, message=None):
        self.method = method
        self.code = code if type(code) is int else None
        self.source_message = message if isinstance(message, str) else ""
        super().__init__(f"{method}: RPC error {self.code}")


class Client:
    """Separate solicited replies from asynchronous requests and notifications."""

    def __init__(self, transport):
        self.transport = transport
        self.sequence = 0
        self.pending = {}
        self.event_boundaries = set()
        self.events = asyncio.Queue(maxsize=1024)
        self.failure = None
        self.task = asyncio.create_task(self.read())

    async def read(self):
        try:
            while True:
                message = await self.transport.receive_json()
                if "method" in message:
                    self.events.put_nowait(message)
                elif message.get("id") in self.pending:
                    future = self.pending[message["id"]]
                    if not future.done():
                        events = None
                        if message["id"] in self.event_boundaries:
                            events = []
                            while not self.events.empty():
                                events.append(self.events.get_nowait())
                        future.set_result((message, events))
        except (
            OSError,
            EOFError,
            ValueError,
            RuntimeError,
            TypeError,
            LookupError,
            asyncio.QueueFull,
        ) as error:
            self.failure = error
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(RuntimeError("Source transport failed"))

    async def send(self, message):
        if self.failure is not None:
            raise RuntimeError("Source transport failed") from self.failure
        await self.transport.send_json(message)

    async def rpc(self, method, params, *, capture_events=False):
        self.sequence += 1
        request_id = f"lapis-probe-{self.sequence}"
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        if capture_events:
            self.event_boundaries.add(request_id)
        try:
            async with asyncio.timeout(15):
                await self.send({"id": request_id, "method": method, "params": params})
                message, events = await future
            if "error" in message:
                # Source error text may contain paths, credentials or transcripts.
                raise RpcError(
                    method,
                    message["error"].get("code"),
                    message["error"].get("message"),
                )
            return (message["result"], events) if capture_events else message["result"]
        finally:
            self.pending.pop(request_id, None)
            self.event_boundaries.discard(request_id)

    async def event(self, predicate, timeout=90):
        async with asyncio.timeout(timeout):
            while True:
                if self.failure is not None:
                    raise RuntimeError("Source transport failed") from self.failure
                # A bounded poll also observes reader failure without waiting 90 seconds.
                try:
                    message = await asyncio.wait_for(self.events.get(), 0.1)
                except TimeoutError:
                    continue
                if predicate(message):
                    return message

    async def close(self):
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)
        await self.transport.close()


async def stop(process):
    if process is None or process.returncode is not None:
        return
    process.terminate()
    try:
        await asyncio.wait_for(process.wait(), 5)
    except TimeoutError:
        process.kill()
        await process.wait()


async def resume_snapshot(client, thread):
    """Exercise this binary's same-thread serialized resume/read boundary.

    Unlike a quiet interval, receipt of thread/read is an explicit wire boundary.
    Qualification also depends on the inspected server's serialization contract;
    this helper alone does not prove that another Codex version has that contract.
    """
    await client.rpc("thread/resume", {"threadId": thread, "excludeTurns": True})
    result, events = await client.rpc(
        "thread/read", {"threadId": thread, "includeTurns": True}, capture_events=True
    )
    view = result["thread"]
    if view["id"] != thread:
        raise RuntimeError("Reconciliation returned another thread")
    pending = {}
    retired = set()
    for event in events:
        if event.get("params", {}).get("threadId") != thread:
            continue
        if event.get("method") in METHODS.values():
            key = (type(event["id"]), event["id"])
            # Resolution can race with the server's copied replay batch.
            if key in retired:
                continue
            if key in pending and pending[key] != event:
                raise RuntimeError("Conflicting replay payload")
            pending[key] = event
        elif event.get("method") == "serverRequest/resolved":
            request_id = event["params"]["requestId"]
            key = (type(request_id), request_id)
            retired.add(key)
            pending.pop(key, None)
    return view, list(pending.values())


class Tui:
    """Private PTY fixture; never uses the user's desktop focus or clipboard."""

    def __init__(self):
        self.master = None
        self.process = None
        self.output = bytearray()
        self.tail = b""

    async def start(self, arguments, environment, cwd):
        self.master, slave = pty.openpty()
        try:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 110, 0, 0))
            self.process = await asyncio.create_subprocess_exec(
                *arguments,
                stdin=slave,
                stdout=slave,
                stderr=slave,
                env={**environment, "TERM": "xterm-256color"},
                cwd=cwd,
                start_new_session=True,
            )
        finally:
            os.close(slave)
        os.set_blocking(self.master, False)
        asyncio.get_running_loop().add_reader(self.master, self.read)

    def read(self):
        try:
            chunk = os.read(self.master, 65536)
        except OSError:
            asyncio.get_running_loop().remove_reader(self.master)
            return
        if not chunk:
            asyncio.get_running_loop().remove_reader(self.master)
            return
        self.output.extend(chunk)
        self.output = self.output[-1048576:]
        combined = self.tail + chunk
        for question, answer in (
            (b"\x1b[6n", b"\x1b[1;1R"),
            (b"\x1b]10;?", b"\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
            (b"\x1b]11;?", b"\x1b]11;rgb:0000/0000/0000\x1b\\"),
            (b"\x1b[c", b"\x1b[?1;2c"),
        ):
            if question in combined:
                try:
                    os.write(self.master, answer)
                except OSError:
                    return
        self.tail = chunk[-8:]

    async def wait(self, needle):
        async with asyncio.timeout(15):
            while needle not in self.output:
                if self.process.returncode is not None:
                    raise RuntimeError(
                        "Fixture TUI exited before displaying its request"
                    )
                await asyncio.sleep(0.05)

    async def close(self):
        if self.master is not None:
            asyncio.get_running_loop().remove_reader(self.master)
        try:
            await stop(self.process)
        finally:
            if self.master is not None:
                os.close(self.master)
                self.master = None


def trust_fixture_directory(home, cwd):
    """Trust one disposable fixture in its new private Codex home."""
    with (home / "config.toml").open("x") as config:
        config.write(
            f'[projects.{json.dumps(str(cwd.resolve()))}]\ntrust_level = "trusted"\n'
        )


def probe_configuration_arguments():
    """Shared isolated GLM route for app-server and ordinary-TUI fixtures."""
    config = {
        "model": MODEL,
        "model_provider": "lapis_probe",
        "model_providers.lapis_probe.name": "lapis GLM probe",
        "model_providers.lapis_probe.base_url": "http://127.0.0.1:3456/v1",
        "model_providers.lapis_probe.wire_api": "responses",
        "model_providers.lapis_probe.requires_openai_auth": False,
        "model_providers.lapis_probe.supports_websockets": False,
        "model_providers.lapis_probe.experimental_bearer_token": "ccr-local",
        "model_providers.lapis_probe.request_max_retries": 0,
        "model_providers.lapis_probe.stream_max_retries": 0,
        "features.apps": False,
        "features.plugins": False,
        "agents.enabled": False,
        "features.multi_agent": False,
        "features.multi_agent_v2": False,
        "skills.include_instructions": False,
        "analytics.enabled": False,
    }
    arguments = []
    for key, value in config.items():
        arguments.extend(["-c", key + "=" + json.dumps(value)])
    return arguments


def server_arguments(binary, socket):
    return [
        str(binary),
        "app-server",
        "--listen",
        "unix://" + str(socket),
    ] + probe_configuration_arguments()


async def roundtrip(case, owner, observer, connect, cwd, receipt, tui_arguments):
    started = await owner.rpc(
        "thread/start",
        {
            "cwd": str(cwd),
            "model": MODEL,
            "modelProvider": "lapis_probe",
            "approvalPolicy": "untrusted",
            "approvalsReviewer": "user",
            "sandbox": "read-only",
            "ephemeral": False,
            "allowProviderModelFallback": False,
            "developerInstructions": "Bounded lapis protocol fixture. Do only the requested operation; do not explore files or spawn agents.",
        },
    )
    if started["model"] != MODEL or started["modelProvider"] != "lapis_probe":
        raise RuntimeError("Unexpected effective Codex model/provider")
    thread = started["thread"]["id"]
    params = {}
    if case == "input":
        prompt = "Use request_user_input now to ask one question with id color: choose Blue or Green. Do not use any other tool. After I answer, reply with just the selected color."
        params["collaborationMode"] = {
            "mode": "plan",
            "settings": {
                "model": MODEL,
                "reasoning_effort": "low",
                "developer_instructions": None,
            },
        }
    else:
        prompt = 'Use exec_command to run exactly python3 -c "print(123456789)". This protocol fixture requires approval before execution. Do not use other commands or tools. After execution reply DONE.'
    result = await owner.rpc(
        "turn/start",
        {"threadId": thread, "input": [{"type": "text", "text": prompt}], **params},
    )
    turn = result["turn"]["id"]

    def requested(message):
        if matches(message, "error", thread) or matches(
            message, "turn/completed", thread, turn=turn
        ):
            raise RuntimeError(
                "Turn ended or failed before the required attention request"
            )
        return matches(message, METHODS[case], thread)

    # Wait for a real request before resume: a fresh zero-turn thread may have no rollout.
    original = await owner.event(requested)
    await observer.rpc("thread/resume", {"threadId": thread, "excludeTurns": True})
    request = await observer.event(requested)
    if (
        type(original["id"]) is not type(request["id"])
        or original["id"] != request["id"]
    ):
        raise RuntimeError("Request identity changed across clients")
    await observer.close()
    observer = await connect()
    active_view, pending = await resume_snapshot(observer, thread)
    expected_flag = "waitingOnUserInput" if case == "input" else "waitingOnApproval"
    if (
        len(pending) != 1
        or active_view["status"]["type"] != "active"
        or expected_flag not in active_view["status"]["activeFlags"]
    ):
        raise RuntimeError(
            "Pending reconciliation boundary did not preserve blocking request"
        )
    replay = pending[0]
    if replay != request:
        raise RuntimeError("Pending request changed across source reconnect")
    if case == "input":
        questions = replay["params"]["questions"]
        if len(questions) != 1 or questions[0]["id"] != "color":
            raise RuntimeError("Unexpected input fixture question")
        label = questions[0]["options"][0]["label"]
        if not label.startswith("Blue"):
            raise RuntimeError("Unexpected input fixture answer")
        answer = {"answers": {"color": {"answers": [label]}}}
    else:
        if not approved_fixture(replay["params"].get("command")):
            raise RuntimeError("Refusing approval for a non-fixture command")
        answer = {"decision": "accept"}
    # This witness goes offline while a different client answers the request.
    witness = await connect()
    _, witnessed = await resume_snapshot(witness, thread)
    if witnessed != [replay]:
        raise RuntimeError("Disconnected witness did not observe the original request")
    await witness.close()
    tui = Tui()
    try:
        if tui_arguments:
            arguments, environment = tui_arguments
            await tui.start(
                [
                    arguments[0],
                    "resume",
                    thread,
                    "--remote",
                    arguments[3],
                    "--no-alt-screen",
                    "-C",
                    str(cwd),
                    *arguments[4:],
                ],
                environment,
                cwd,
            )
            needle = (
                b"Would you like to run the following command?"
                if case == "approval"
                else questions[0]["question"].encode()
            )
            await tui.wait(needle)
        await observer.send({"id": replay["id"], "result": answer})
        resolved = completed = False

        def completion(message):
            nonlocal resolved, completed
            if matches(message, "error", thread):
                raise RuntimeError("Source error during continuation")
            if matches(
                message, "serverRequest/resolved", thread, request_id=replay["id"]
            ):
                resolved = True
            if matches(message, "turn/completed", thread, turn=turn):
                if message["params"]["turn"]["status"] != "completed":
                    raise RuntimeError("Fixture turn did not complete successfully")
                completed = True
            return resolved and completed

        await observer.event(completion)
        witness = await connect()
        idle_view, remaining = await resume_snapshot(witness, thread)
        if remaining or idle_view["status"]["type"] != "idle":
            raise RuntimeError(
                "Resolved request remained pending after disconnected reconciliation"
            )
        if not any(
            t["id"] == turn and t["status"] == "completed" for t in idle_view["turns"]
        ):
            raise RuntimeError("Reconciled history did not retain the completed turn")
        await witness.close()
        if tui_arguments and tui.process.returncode is not None:
            raise RuntimeError("Fixture TUI exited during the round trip")
        receipt["cases"].append(
            {
                "case": case,
                "effective_model": started["model"],
                "effective_provider": started["modelProvider"],
                "passed": True,
                "request_id_type": type(replay["id"]).__name__,
                "owner_delivery": True,
                "observer_resume_replay": True,
                "reconnect_replay": True,
                "pending_at_serialized_read_boundary": True,
                "resolved_while_disconnected_absent_at_boundary": True,
                "reconciled_idle_and_completed_turn": True,
                "explicit_observer_response": True,
                "matching_resolution": True,
                "completed_turn": True,
                "tui_displayed_request": bool(tui_arguments),
            }
        )
    finally:
        await tui.close()
    return observer


async def run(args, receipt):
    binary = Path(shutil.which("codex") or "").resolve()
    if not binary.is_file():
        raise RuntimeError("codex is not installed")
    receipt["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    clients = []
    process = None
    (ROOT / "runtime").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="attention-", dir=ROOT / "runtime"
    ) as temporary:
        root = Path(temporary)
        home, cwd = root / "home", root / "fixture"
        home.mkdir()
        cwd.mkdir()
        trust_fixture_directory(home, cwd)
        environment = {
            **os.environ,
            "CODEX_HOME": str(home),
            "CODEX_APP_SERVER_MANAGED_CONFIG_PATH": str(home / "managed.toml"),
        }
        socket = root / "server.sock"
        arguments = server_arguments(binary, socket)
        try:
            process = await asyncio.create_subprocess_exec(
                *arguments,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
                env=environment,
                cwd=cwd,
                start_new_session=True,
            )
            async with asyncio.timeout(10):
                while not socket.exists():
                    if process.returncode is not None:
                        raise RuntimeError("Private server exited before listening")
                    await asyncio.sleep(0.05)

            async def connect():
                transport = await UnixWebSocketTransport.connect(
                    socket, connect_timeout=5
                )
                client = Client(transport)
                clients.append(client)
                await client.rpc(
                    "initialize",
                    {
                        "clientInfo": {
                            "name": "lapis_attention_fixture",
                            "version": "0.1",
                        },
                        "capabilities": {"experimentalApi": True},
                    },
                )
                await client.send({"method": "initialized"})
                return client

            owner, observer = await connect(), await connect()
            cases = ("input", "approval") if args.case == "both" else (args.case,)
            for case in cases:
                observer = await roundtrip(
                    case,
                    owner,
                    observer,
                    connect,
                    cwd,
                    receipt,
                    (arguments, environment) if args.with_tui else None,
                )
        finally:
            await asyncio.gather(*(c.close() for c in clients), return_exceptions=True)
            await stop(process)
    receipt["cleanup_complete"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live-glm",
        action="store_true",
        required=True,
        help="Authorize bounded live turns via local CCR zai,glm-5.3; no fallback model",
    )
    parser.add_argument("--with-tui", action="store_true")
    parser.add_argument("--case", choices=("input", "approval", "both"), default="both")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/codex-attention-live.json"
    )
    args = parser.parse_args()
    receipt = {
        "schema": "lapis.codex-attention-live/1",
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "passed": False,
        "configured_model": MODEL,
        "configured_provider": "CCR on loopback port 3456",
        "cases": [],
        "limits": [
            "Isolated protocol/TUI fixture, not lapis service/desktop integration",
            "Resume/read boundary depends on this Codex implementation's same-thread serialization; requalify changed binaries",
            "Only blocking command approval and tool user-input requests are qualified; other request kinds remain unsupported",
            "No claim of pixel-presentation or hardware keyboard latency",
        ],
    }
    try:
        asyncio.run(run(args, receipt))
        receipt["passed"] = True
    except (
        OSError,
        EOFError,
        ValueError,
        RuntimeError,
        TypeError,
        LookupError,
        asyncio.QueueFull,
    ) as error:
        receipt["error_type"] = type(error).__name__
        # Only locally constructed error messages are safe to publish.
        receipt["error"] = (
            str(error)
            if type(error) is RuntimeError
            else "Fixture failed; inspect local reproduction"
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"{'PASS' if receipt['passed'] else 'FAIL'}: {args.output}")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
