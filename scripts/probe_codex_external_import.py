"""Qualify Codex external-agent session import against a disposable HOME."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from .check_codex_attention import Client, RpcError, server_arguments, stop
    from .codex_probe_transport import UnixWebSocketTransport
except ImportError:
    from check_codex_attention import Client, RpcError, server_arguments, stop
    from codex_probe_transport import UnixWebSocketTransport

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_TITLE = "lapis synthetic import title"
PROVIDER_ID = "lapis-external-import-probe"


def required_object(value: Any, message: str) -> dict[str, Any]:
    """Require an RPC result object before accessing its keys."""
    if not isinstance(value, dict):
        raise RuntimeError(message)
    return value


async def initialize(client: Client) -> dict[str, Any]:
    """Initialize the private server and prove which CODEX_HOME it selected."""
    reply = await client.rpc(
        "initialize",
        {
            "clientInfo": {"name": "lapis_external_import_probe", "version": "0.1"},
            "capabilities": {"experimentalApi": True},
        },
    )
    reply = required_object(reply, "Malformed Codex initialization response")
    required = {"userAgent", "codexHome", "platformFamily", "platformOs"}
    if not required.issubset(reply):
        raise RuntimeError("Incomplete initialize response")
    await client.send({"method": "initialized"})
    return reply


def session_item(items: list[dict[str, Any]], home: Path) -> dict[str, Any]:
    """Select only the detected session item; config classes stay opt-in elsewhere."""
    if not isinstance(items, list) or any(not isinstance(item, dict) for item in items):
        raise RuntimeError("Malformed Codex detection items")
    sessions = [
        item
        for item in items
        if isinstance(item, dict) and item.get("itemType") == "SESSIONS"
    ]
    if len(sessions) != 1:
        raise RuntimeError("Expected exactly one session migration item")
    item = sessions[0]
    details = item.get("details")
    if not isinstance(details, dict):
        raise RuntimeError("Expected exactly one detected session")
    allowed_details = {
        "plugins",
        "skills",
        "sessions",
        "mcpServers",
        "hooks",
        "subagents",
        "commands",
        "memory",
    }
    if set(details) - allowed_details:
        raise RuntimeError("Session item has an unknown migration detail")
    for name, value in details.items():
        if name != "sessions" and (not isinstance(value, list) or value):
            raise RuntimeError("Session item carries a non-session migration class")
    sessions_detected = details.get("sessions")
    if not isinstance(sessions_detected, list) or len(sessions_detected) != 1:
        raise RuntimeError("Expected exactly one detected session")
    session = sessions_detected[0]
    if not isinstance(session, dict):
        raise RuntimeError("Malformed detected session")
    session_path = session.get("path")
    if not isinstance(session_path, str) or not Path(
        session_path
    ).resolve().is_relative_to(home.resolve()):
        raise RuntimeError("Detected session escapes the disposable home")
    session_cwd = session.get("cwd")
    if not isinstance(session_cwd, str) or not Path(
        session_cwd
    ).resolve().is_relative_to(home.resolve()):
        raise RuntimeError("Detected session cwd escapes the disposable home")
    return item


def import_item(
    item: dict[str, Any], *, provider_id: str = PROVIDER_ID
) -> dict[str, Any]:
    """Return a sessions-only import request carrying the server's exact item."""
    return {
        "migrationItems": [item],
        "source": "lapis external import probe",
        "providerId": provider_id,
        "migrationSource": "claude",
    }


def completed_result(message: dict[str, Any], import_id: str) -> str:
    """Validate the matching terminal notification without retaining source paths."""
    params = message.get("params")
    if message.get(
        "method"
    ) != "externalAgentConfig/import/completed" or not isinstance(params, dict):
        raise RuntimeError("Unexpected import notification")
    if params.get("importId") != import_id:
        raise RuntimeError("Import completion matched another import")
    results = params.get("itemTypeResults")
    if not isinstance(results, list) or len(results) != 1:
        raise RuntimeError("Unexpected import result count")
    result = results[0]
    if (
        not isinstance(result, dict)
        or result.get("itemType") != "SESSIONS"
        or len(result.get("successes", [])) != 1
        or result.get("failures", []) != []
    ):
        raise RuntimeError("Session import did not complete exactly once")
    success = result["successes"][0]
    target = success.get("target") if isinstance(success, dict) else None
    if not isinstance(target, str) or not target:
        raise RuntimeError("Session import did not complete exactly once")
    if success.get("title") != EXPECTED_TITLE:
        raise RuntimeError("Import completed with unexpected session identity")
    return target


def imported_thread(result: dict[str, Any], expected_thread_id: str) -> str:
    """Find exactly the thread identified by the import completion."""
    threads = result.get("data")
    if not isinstance(threads, list) or len(threads) != 1:
        raise RuntimeError("Expected one persisted thread in the private Codex home")
    thread = threads[0]
    if not isinstance(thread, dict):
        raise RuntimeError("Malformed persisted-thread entry")
    thread_id = thread.get("id")
    if thread_id != expected_thread_id:
        raise RuntimeError("Persisted thread did not match the import target")
    return thread_id


def import_history(result: dict[str, Any], import_id: str) -> int:
    """Validate migration provenance and return only its count."""
    histories = result.get("data")
    if not isinstance(histories, list):
        raise RuntimeError("Invalid import history response")
    if any(not isinstance(history, dict) for history in histories):
        raise RuntimeError("Malformed import history entry")
    matching = [
        history
        for history in histories
        if history.get("importId") == import_id
        and history.get("providerId") == PROVIDER_ID
    ]
    if len(matching) != 1:
        raise RuntimeError("Expected exactly one matching import history")
    return len(matching)


def write_fixture(home: Path) -> Path:
    """Create one synthetic Claude transcript whose cwd is also disposable."""
    claude = home / ".claude"
    project = home / "fixture-project"
    project.mkdir()
    projects = claude / "projects" / "fixture"
    projects.mkdir(parents=True)
    timestamp = datetime.now(timezone.utc).isoformat()
    records = [
        {
            "type": "user",
            "cwd": str(project),
            "timestamp": timestamp,
            "message": {"content": "synthetic import input"},
        },
        {
            "type": "assistant",
            "cwd": str(project),
            "timestamp": timestamp,
            "message": {"content": "synthetic import output"},
        },
        {"type": "custom-title", "customTitle": EXPECTED_TITLE},
    ]
    path = projects / "session.jsonl"
    path.write_text(
        "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records)
    )
    return project


async def connect(
    server_socket: Path, codex_home: Path, receipt: dict[str, Any]
) -> Client:
    client = Client(await UnixWebSocketTransport.connect(server_socket))
    try:
        reply = await initialize(client)
        if Path(reply["codexHome"]).resolve() != codex_home.resolve():
            raise RuntimeError("Server did not use the private Codex home")
        receipt["observed"]["initialize"] = {"passed": True}
        return client
    except BaseException:
        await client.close()
        raise


async def run_probe(binary: Path, receipt: dict[str, Any]) -> None:
    """Run one dedicated server against only synthetic HOME and CODEX_HOME state."""
    runtime_root = ROOT / "runtime"
    runtime_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="external-import-", dir=runtime_root
    ) as temporary:
        runtime = Path(temporary).resolve()
        home = runtime / "home"
        codex_home = runtime / "codex-home"
        home.mkdir()
        codex_home.mkdir()
        project = write_fixture(home)
        receipt["fixture"]["transcripts"] = 1
        receipt["fixture"]["session_title_present"] = True
        server_socket = runtime / "server.sock"
        environment = os.environ.copy()
        for name in (
            "CLAUDE_CONFIG_DIR",
            "CODEX_APP_SERVER_MANAGED_CONFIG_PATH",
        ):
            environment.pop(name, None)
        environment.update(
            {
                "HOME": str(home),
                "CODEX_HOME": str(codex_home),
                "XDG_CONFIG_HOME": str(home / ".config"),
            }
        )
        arguments = server_arguments(binary, server_socket)
        process = None
        client = None
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
                while not server_socket.exists():
                    if process.returncode is not None:
                        raise RuntimeError("Private server exited before listening")
                    await asyncio.sleep(0.05)

            client = await connect(server_socket, codex_home, receipt)
            detected = required_object(
                await client.rpc(
                    "externalAgentConfig/detect",
                    {
                        "includeHome": True,
                        "cwds": [str(project)],
                        "maxSessionAgeDays": 1,
                        "maxSessions": 1,
                        "migrationSource": "claude",
                    },
                ),
                "Malformed Codex detection response",
            )
            item = session_item(detected.get("items", []), home)
            receipt["isolation"]["real_claude_files_read"] = False
            receipt["observed"]["externalAgentConfig/detect"] = {
                "passed": True,
                "migration_source": "claude",
                "item_types": [
                    entry.get("itemType") for entry in detected.get("items", [])
                ],
                "session_count": 1,
            }

            request = import_item(item)
            accepted = required_object(
                await client.rpc("externalAgentConfig/import", request),
                "Malformed Codex import acceptance",
            )
            import_id = accepted.get("importId")
            if not isinstance(import_id, str) or not import_id:
                raise RuntimeError("Import acceptance did not contain an import ID")
            completion = await client.event(
                lambda message: (
                    message.get("method") == "externalAgentConfig/import/completed"
                ),
                timeout=30,
            )
            expected_thread_id = completed_result(completion, import_id)
            receipt["observed"]["externalAgentConfig/import"] = {
                "accepted": True,
                "import_id_present": True,
            }
            receipt["observed"]["externalAgentConfig/import/completed"] = {
                "observed": True,
                "successes": {"SESSIONS": 1},
                "failures": 0,
            }

            threads = required_object(
                await client.rpc("thread/list", {"limit": 10}),
                "Malformed persisted-thread response",
            )
            thread_id = imported_thread(threads, expected_thread_id)
            histories = required_object(
                await client.rpc("externalAgentConfig/import/readHistories", None),
                "Malformed import-history response",
            )
            receipt["observed"]["thread/list"] = {
                "passed": True,
                "thread_count": len(threads.get("data", [])),
                "imported_thread_id": thread_id,
            }
            receipt["observed"]["externalAgentConfig/import/readHistories"] = {
                "passed": True,
                "history_count": import_history(histories, import_id),
            }
            receipt["fixture"]["transcript_records"] = 3
            receipt["fixture"]["message_records"] = 2
            receipt["fixture"]["custom_title_present"] = True
            receipt["files"]["session_rollout_jsonl"] = len(
                list((codex_home / "sessions").rglob("*.jsonl"))
            )
            receipt["passed"] = True
        finally:
            try:
                if client is not None:
                    await client.close()
            finally:
                await stop(process)
        receipt["isolation"]["cleanup_complete"] = True


def source_revision() -> tuple[str | None, bool | None]:
    """Record only the current commit and dirty flag for this checkout."""
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout.strip()
        dirty = bool(
            subprocess.run(
                ["git", "status", "--short"],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
                timeout=5,
            ).stdout.strip()
        )
    except (OSError, subprocess.SubprocessError):
        return None, None
    return revision, dirty


def reported_version(binary: Path) -> str | None:
    """Ask the selected binary for its version in a minimal disposable environment."""
    try:
        result = subprocess.run(
            [str(binary), "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
            env={"PATH": os.environ.get("PATH", ""), "HOME": tempfile.gettempdir()},
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() or None


def base_receipt() -> dict[str, Any]:
    revision, dirty = source_revision()
    return {
        "schema": "lapis.codex-external-import-probe/1",
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "passed": False,
        "purpose": "Exercise Codex external-agent session-only import against a synthetic Claude fixture without reading or modifying real Claude Code or Codex user state.",
        "source": {
            "lapis_revision": revision,
            "lapis_dirty": dirty,
            "codex_repository": "PATH-resolved executable; source repository not asserted",
            "binary_path": "PATH-resolved codex executable",
            "reported_version": None,
        },
        "isolation": {
            "home": "private temporary HOME containing only the synthetic .claude fixture",
            "codex_home": "private temporary CODEX_HOME",
            "server": "dedicated codex app-server listening on a private Unix socket",
            "inference_used": False,
            "real_claude_files_read": False,
            "real_codex_files_modified": False,
            "cleanup_complete": False,
        },
        "fixture": {
            "settings": False,
            "instructions": False,
            "skills": 0,
            "transcripts": 0,
            "session_title_present": False,
        },
        "observed": {},
        "files": {},
        "limits": [
            "Session-only import; no configuration, instructions, skills, agents, hooks, commands, MCP, plugins or memory",
            "No model turn, lapis UI, launch/resume, duplicate reconciliation or failure/cancellation qualification",
        ],
        "non_claims": [
            "Does not prove every real Claude transcript variant is importable.",
            "Does not qualify production import UI, consent flow, retries or managed-daemon coexistence.",
            "One passing synthetic session does not establish broader external-agent migration support.",
        ],
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", default="codex")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/codex-external-import.json"
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    receipt = base_receipt()
    try:
        executable = shutil.which(args.codex)
        if not executable:
            raise RuntimeError("Codex executable not found")
        binary = Path(executable).resolve()
        receipt["source"]["binary_sha256"] = hashlib.sha256(
            binary.read_bytes()
        ).hexdigest()
        receipt["source"]["reported_version"] = reported_version(binary)
        asyncio.run(run_probe(binary, receipt))
    except (OSError, RpcError, RuntimeError, ValueError, TypeError, LookupError):
        # Source errors can contain absolute fixture paths or transcript text.
        receipt["passed"] = False
        receipt["isolation"]["cleanup_complete"] = False
        receipt["error_type"] = "ProbeError"
        receipt["error"] = "Probe failed without recording source error text"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"{'PASS' if receipt['passed'] else 'FAIL'}: {args.output}")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
