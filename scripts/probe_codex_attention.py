"""Probe a dedicated Codex shared server without submitting a model turn."""

import argparse
import asyncio
import hashlib
import json
import os
import shutil
import tempfile
from datetime import datetime, timezone
from pathlib import Path

try:
    from .check_codex_attention import Client, RpcError, server_arguments, stop
    from .codex_probe_transport import UnixWebSocketTransport
except ImportError:
    from check_codex_attention import Client, RpcError, server_arguments, stop
    from codex_probe_transport import UnixWebSocketTransport

ROOT = Path(__file__).resolve().parents[1]


async def initialize(client):
    reply = await client.rpc(
        "initialize",
        {
            "clientInfo": {"name": "lapis_no_turn_probe", "version": "0.1"},
            "capabilities": {"experimentalApi": True},
        },
    )
    required = {"userAgent", "codexHome", "platformFamily", "platformOs"}
    if not required.issubset(reply):
        raise RuntimeError("Incomplete initialize response")
    await client.send({"method": "initialized"})
    return reply


async def inspect_thread(client, thread, method, phases):
    """Absent zero-turn history is a specific limit, not transport success."""
    parameters = {"threadId": thread}
    parameters["excludeTurns" if method == "thread/resume" else "includeTurns"] = (
        method == "thread/resume"
    )
    try:
        result = await client.rpc(method, parameters)
    except RpcError as error:
        if (
            error.code != -32600
            or error.source_message != f"no rollout found for thread id {thread}"
        ):
            raise
        phases.append(
            {
                "method": method,
                "status": "unavailable_for_zero_turn_fixture",
                "rpc_code": error.code,
            }
        )
        return False
    if result.get("thread", {}).get("id") != thread:
        raise RuntimeError("Observer received a different thread")
    phases.append({"method": method, "status": "passed"})
    return True


async def run_probe(binary, receipt):
    (ROOT / "runtime").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="no-turn-", dir=ROOT / "runtime"
    ) as temporary:
        runtime = Path(temporary)
        home = runtime / "home"
        home.mkdir()
        socket = runtime / "server.sock"
        environment = {
            **os.environ,
            "CODEX_HOME": str(home),
            "CODEX_APP_SERVER_MANAGED_CONFIG_PATH": str(home / "managed.toml"),
        }
        arguments = server_arguments(binary, socket)
        # No prompt or turn/start is sent. The placeholder cannot select a real model.
        arguments.extend(["-c", 'model="probe-no-model"'])
        clients = []
        process = None
        try:
            process = await asyncio.create_subprocess_exec(
                *arguments,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
                cwd=home,
                env=environment,
                start_new_session=True,
            )
            async with asyncio.timeout(10):
                while not socket.exists():
                    if process.returncode is not None:
                        raise RuntimeError("Private server exited before listening")
                    await asyncio.sleep(0.05)

            async def connect():
                client = Client(
                    await UnixWebSocketTransport.connect(socket, connect_timeout=5)
                )
                clients.append(client)
                reply = await initialize(client)
                if Path(reply["codexHome"]).resolve() != home.resolve():
                    raise RuntimeError("Server did not use the private Codex home")
                receipt["phases"].append(
                    {
                        "method": "initialize",
                        "status": "passed",
                        "platform_os": reply["platformOs"],
                    }
                )
                return client

            owner, observer = await connect(), await connect()
            started = await owner.rpc(
                "thread/start",
                {
                    "cwd": str(home),
                    "ephemeral": True,
                    "model": "probe-no-model",
                    "approvalPolicy": "never",
                    "sandbox": "read-only",
                    "allowProviderModelFallback": False,
                },
            )
            thread = started["thread"]["id"]
            receipt["phases"].append(
                {
                    "method": "thread/start",
                    "status": "passed",
                    "ephemeral": started["thread"]["ephemeral"],
                }
            )
            await inspect_thread(observer, thread, "thread/read", receipt["phases"])
            await observer.close()
            observer = await connect()
            await inspect_thread(observer, thread, "thread/read", receipt["phases"])
            await inspect_thread(observer, thread, "thread/resume", receipt["phases"])
            receipt["passed"] = True
        finally:
            await asyncio.gather(
                *(client.close() for client in clients), return_exceptions=True
            )
            await stop(process)
    receipt["cleanup_complete"] = True


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", default="codex")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/codex-shared-server.json"
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    receipt = {
        "schema": "lapis.codex-shared-server/1",
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "passed": False,
        "turns_started": 0,
        "real_attention_qualified": False,
        "phases": [],
        "limits": [
            "No-turn transport/metadata probe only",
            "Zero-turn history may not support read/resume",
            "No inference, TUI, response or complete reconciliation evidence",
        ],
    }
    try:
        executable = shutil.which(args.codex)
        if not executable:
            raise RuntimeError("Codex executable not found")
        binary = Path(executable).resolve()
        receipt["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        asyncio.run(run_probe(binary, receipt))
    except (
        OSError,
        EOFError,
        ValueError,
        RuntimeError,
        TypeError,
        LookupError,
    ) as error:
        receipt["error_type"] = type(error).__name__
        receipt["error"] = str(error) if type(error) is RuntimeError else "Probe failed"
        if isinstance(error, RpcError):
            receipt["rpc_error"] = {"method": error.method, "code": error.code}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"{'PASS' if receipt['passed'] else 'FAIL'}: {args.output}")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
