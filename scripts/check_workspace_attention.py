"""Qualify two real managed Codex sources through the production workspace."""

import argparse
import asyncio
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from check_cli_launch import CheckError, Service, require
from check_codex_attention import (
    MODEL,
    Client,
    approved_fixture,
    probe_configuration_arguments,
)
from check_service_attention import (
    TEXT,
    View,
    finished_turn,
    question_turn,
    process_groups,
    selected_question_answers,
    wait_groups_gone,
)
from codex_probe_transport import UnixWebSocketTransport
from probe_codex_attention import initialize
from probe_terminal import run_process

ROOT = Path(__file__).resolve().parents[1]
PROBE_TIMEOUT_SECONDS = 90
QUESTION_TIMEOUT_SECONDS = 120


@dataclass(frozen=True)
class SourceFixture:
    role: str
    service: Service
    owner: Client
    thread: str
    process_groups: frozenset[int]


class PendingMonitors:
    """Own monitor connections until qualification is ready to detach."""

    def __init__(self):
        self.monitors = {}
        self.closed = False

    async def close(self, receipt, original_error=None):
        await close_monitors(self, receipt, original_error)


def launch_options(prompt):
    options = probe_configuration_arguments() + [
        "-c",
        'approval_policy="on-request"',
        "-c",
        'approvals_reviewer="user"',
        "-c",
        'sandbox_mode="read-only"',
        "--no-alt-screen",
    ]
    return options + (["--", prompt] if prompt else [])


def approval_prompt():
    return (
        'Use exec_command now to run exactly python3 -c "print(123456789)". '
        "Set sandbox_permissions to require_escalated and justification to "
        '"Verify lapis workspace approval routing" so this harmless fixture asks '
        "for approval before execution. Do not use other commands or tools. "
        "After execution reply DONE."
    )


def request_by_role(requests, role, thread):
    matching = [request for request in requests if request["thread"] == thread]
    require(len(matching) == 1, f"Expected one live {role} request")
    request = matching[0]
    if role == "approval":
        require(
            approved_fixture(request["details"].get("command")),
            "Refusing non-fixture approval command",
        )
    else:
        selected_question_answers(request["details"].get("questions", []))
    return request


def report_cleanup_errors(receipt, original_error, failures):
    if not failures:
        return
    messages = [message for message, _ in failures]
    receipt.setdefault("cleanup_errors", []).extend(messages)
    if original_error is not None:
        for message in messages:
            original_error.add_note(message)
        return
    interruption = next(
        (error for _, error in failures if not isinstance(error, Exception)), None
    )
    if interruption is not None:
        for message in messages:
            interruption.add_note(message)
        raise interruption
    raise CheckError("; ".join(messages))


async def start_source(args, binary, runtime, artifacts, role, prompt):
    home = runtime / f"{role}-home"
    cwd = runtime / f"{role}-cwd"
    home.mkdir(mode=0o700)
    cwd.mkdir(mode=0o700)
    service = Service(
        args.build_dir / "services/session/lapis_session_service",
        runtime,
        artifacts,
        role,
        str(binary),
        launch_options(prompt),
        cwd,
        {
            "CODEX_HOME": str(home),
            "CODEX_APP_SERVER_MANAGED_CONFIG_PATH": str(home / "managed.toml"),
        },
        codex=True,
    )
    view = None
    owner = None
    completed = False
    try:
        view = View(await asyncio.to_thread(service.connect))
        await view.wait(lambda: view.attention is not None, 15)
        if "Press enter to continue" in view.screen:
            view.client.send(TEXT, b"\r")
        owner = Client(
            await UnixWebSocketTransport.connect(Path(str(service.endpoint) + ".codex"))
        )
        await initialize(owner)
        async with asyncio.timeout(15):
            while True:
                loaded = await owner.rpc("thread/loaded/list", {"limit": 2})
                if loaded.get("data"):
                    break
                if "Press enter to continue" in view.screen:
                    view.client.send(TEXT, b"\r")
                await asyncio.sleep(0.1)
        persistent = []
        for candidate in loaded["data"]:
            found = await owner.rpc(
                "thread/read", {"threadId": candidate, "includeTurns": False}
            )
            if not found["thread"]["ephemeral"]:
                persistent.append(candidate)
        require(
            len(persistent) == 1,
            f"Fixture {role} must expose one persistent TUI thread",
        )
        groups = frozenset(await asyncio.to_thread(process_groups, service.process.pid))
        await view.close()
        view = None
        fixture = SourceFixture(role, service, owner, persistent[0], groups)
        completed = True
        return fixture
    finally:
        if not completed:
            original = sys.exception()
            failures = []
            for name, connection in (("owner", owner), ("view", view)):
                if connection is not None:
                    try:
                        await connection.close()
                    except BaseException as error:
                        failures.append((f"{role} {name} cleanup: {error}", error))
            try:
                await asyncio.to_thread(service.stop)
            except BaseException as error:
                failures.append((f"{role} service cleanup: {error}", error))
            report_cleanup_errors({}, original, failures)


async def stop_source(source, receipt, original_error=None):
    errors = []
    try:
        await source.owner.close()
    except BaseException as error:
        errors.append(
            (
                f"{source.role}: owner cleanup: {str(error) or type(error).__name__}",
                error,
            )
        )
    try:
        await asyncio.to_thread(source.service.stop)
        receipt.setdefault("services_reaped", []).append(
            source.service.process.poll() is not None
        )
    except BaseException as error:
        errors.append(
            (
                f"{source.role}: service cleanup: {str(error) or type(error).__name__}",
                error,
            )
        )
    if source.process_groups:
        try:
            await wait_groups_gone(source.process_groups)
            receipt.setdefault("process_groups_cleaned", []).append(True)
        except BaseException as error:
            errors.append(
                (
                    f"{source.role}: process-group cleanup: {str(error) or type(error).__name__}",
                    error,
                )
            )
    report_cleanup_errors(receipt, original_error, errors)


async def stop_sources(sources, receipt, original_error=None):
    failures = await asyncio.gather(
        *(stop_source(source, receipt, original_error) for source in sources),
        return_exceptions=True,
    )
    failures = [failure for failure in failures if isinstance(failure, BaseException)]
    if failures and original_error is None:
        interruption = next(
            (error for error in failures if not isinstance(error, Exception)), None
        )
        if interruption is not None:
            raise interruption
        raise CheckError(
            "; ".join(str(error) or type(error).__name__ for error in failures)
        )


async def close_monitors(monitors, receipt, original_error=None):
    if monitors is None or monitors.closed:
        return
    errors = []
    monitors.closed = True
    for role, monitor in monitors.monitors.items():
        try:
            await monitor.close()
        except BaseException as error:
            errors.append(
                (f"{role} monitor cleanup: {str(error) or type(error).__name__}", error)
            )
    report_cleanup_errors(receipt, original_error, errors)


def probe_config(runtime, manifest, sessions):
    return {
        "version": 1,
        "manifest": str(manifest),
        "sessions": sessions,
        "output": str(runtime / "workspace-probe-report.json"),
    }


async def wait_for_pending(sources, receipt):
    monitors = PendingMonitors()
    try:
        for source in sources:
            monitor = View(await asyncio.to_thread(source.service.connect))
            monitors.monitors[source.role] = monitor
            await monitor.wait(
                lambda: (
                    monitor.attention is not None
                    and monitor.attention["ready"]
                    and bool(monitor.attention["requests"])
                )
            )
            request_by_role(monitor.attention["requests"], source.role, source.thread)
        receipt["checks"].append(
            "safe approval and structured-input fixtures validated before answering"
        )
        return monitors
    except BaseException:
        await monitors.close(receipt, sys.exception())
        raise


async def current_turn(source):
    async with asyncio.timeout(QUESTION_TIMEOUT_SECONDS):
        while True:
            result = await source.owner.rpc(
                "thread/read", {"threadId": source.thread, "includeTurns": True}
            )
            in_progress = [
                item
                for item in result["thread"]["turns"]
                if item["status"] == "inProgress"
            ]
            if in_progress:
                return in_progress[0]["id"]
            await asyncio.sleep(0.1)


async def run_ui_probe(args, config, artifacts):
    path = artifacts / "workspace-attention-config.json"
    path.write_text(json.dumps(config, indent=2) + "\n")
    with (artifacts / "workspace-attention-gui.log").open("w") as log:
        result = await asyncio.to_thread(
            run_process,
            [
                args.build_dir / "apps/desktop/lapis_workspace_attention_ui_probe",
                "--config",
                path,
            ],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=PROBE_TIMEOUT_SECONDS,
        )
    capture = Path(config["output"]).parent / "workspace-queue.png"
    if capture.is_file():
        shutil.copy2(capture, artifacts / capture.name)
    report_path = Path(config["output"])
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    (artifacts / "workspace-gui-report.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    require(
        result.returncode == 0,
        f"Workspace GUI probe failed: {report.get('error', 'see its log')}",
    )
    require(report.get("passed") is True, "Workspace GUI probe did not pass")
    return report


def expected_response(monitor, source):
    state = monitor.attention
    request = request_by_role(state["requests"], source.role, source.thread)
    identifier = request["id"]
    typed = (
        "n" + str(identifier)
        if type(identifier) is int
        else "s" + identifier.encode().hex()
    )
    return {
        "sessionId": state["attachment"][:16].hex(),
        "serviceEpoch": state["attachment"][16:32].hex(),
        "requestSuffix": f"{request['epoch']}:{request['revision']}:{typed}",
        "choice": "accept" if source.role == "approval" else "submit",
    }


def validate_responses(responses, expected):
    require(len(responses) == len(expected), "GUI must report each source once")
    seen = set()
    for response in responses:
        role = response.get("sourceId")
        require(
            role in expected and role not in seen,
            "Duplicate or unknown response source",
        )
        seen.add(role)
        wanted = expected[role]
        parts = response.get("token", "").split(":", 3)
        require(
            len(parts) == 4
            and parts[0] == response.get("sessionId") == wanted["sessionId"]
            and parts[1] == wanted["serviceEpoch"]
            and parts[2].isdigit()
            and int(parts[2]) > 0
            and parts[3] == wanted["requestSuffix"]
            and response.get("choice") == wanted["choice"],
            "GUI response identity or decision differs from the live source request",
        )


def provenance(build_dir):
    paths = [
        ROOT / "scripts/check_workspace_attention.py",
        ROOT / "scripts/check_service_attention.py",
        ROOT / "scripts/check_codex_attention.py",
        ROOT / "apps/desktop/tests/workspace_attention_ui_probe.cpp",
        ROOT / "apps/desktop/src/workspace_supervisor.cpp",
        ROOT / "apps/desktop/qml/Main.qml",
        ROOT / "apps/desktop/qml/AttentionDialog.qml",
        build_dir / "apps/desktop/lapis_workspace_attention_ui_probe",
        build_dir / "services/session/lapis_session_service",
    ]
    return {
        "revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "sha256": {
            str(path.relative_to(ROOT))
            if path.is_relative_to(ROOT)
            else str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in paths
        },
    }


async def exercise(args, receipt):
    executable = shutil.which("codex")
    require(executable is not None, "codex is not installed")
    binary = Path(executable).resolve()
    args.build_dir = args.build_dir.resolve()
    probe_binary = args.build_dir / "apps/desktop/lapis_workspace_attention_ui_probe"
    require(probe_binary.is_file(), "Workspace GUI probe is not built")
    receipt.update(
        codex_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        codex_path=str(binary),
        model=MODEL,
        source_count=2,
        provenance=provenance(args.build_dir),
    )
    artifacts = args.output.parent
    (ROOT / "runtime").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="m3-workspace-", dir=ROOT / "runtime"
    ) as temporary:
        runtime = Path(temporary)
        owner = runtime / "owner"
        owner.mkdir(mode=0o700)
        manifest = owner / "workspace.json"
        sources = []
        monitors = None
        try:
            results = await asyncio.gather(
                start_source(
                    args, binary, runtime, artifacts, "approval", approval_prompt()
                ),
                start_source(args, binary, runtime, artifacts, "input", None),
                return_exceptions=True,
            )
            sources = [
                result for result in results if isinstance(result, SourceFixture)
            ]
            failures = [
                result for result in results if isinstance(result, BaseException)
            ]
            if failures:
                raise failures[0]
            structured = next(source for source in sources if source.role == "input")
            await question_turn(structured.owner, structured.thread, "color")
            sessions = [
                {
                    "name": source.role,
                    "role": source.role,
                    "endpoint": str(source.service.endpoint),
                    "program": source.service.program,
                    "arguments": source.service.arguments,
                    "directory": str(source.service.directory),
                }
                for source in sources
            ]
            monitors = await wait_for_pending(sources, receipt)
            for source in sources:
                actual = await source.owner.rpc(
                    "thread/resume", {"threadId": source.thread, "excludeTurns": True}
                )
                require(
                    actual["model"] == MODEL
                    and actual["modelProvider"] == "lapis_probe",
                    "Unexpected live model/provider",
                )
            receipt["runtime_model_provider"] = "lapis_probe"
            turns = {source.role: await current_turn(source) for source in sources}
            expected = {
                source.role: expected_response(monitors.monitors[source.role], source)
                for source in sources
            }
            await monitors.close(receipt)
            gui_report = await run_ui_probe(
                args, probe_config(runtime, manifest, sessions), artifacts
            )
            receipt["checks"].append(
                "two service-owned sources remained live while the GUI attached"
            )
            receipt["gui"] = gui_report
            responses = gui_report.get("responses", [])
            require(len(responses) == 2, "GUI must report both source responses")
            validate_responses(responses, expected)
            for source in sources:
                await finished_turn(source.owner, source.thread, turns[source.role])
            receipt["checks"].extend(
                [
                    "two real managed sources were concurrently pending",
                    "production dialogs answered the exact source requests",
                    "both real turns continued successfully after matching responses",
                ]
            )
        finally:
            original_error = sys.exception()
            try:
                await close_monitors(monitors, receipt, original_error)
            finally:
                await stop_sources(sources, receipt, original_error or sys.exception())
                receipt["fixtures_cleaned"] = not receipt.get("cleanup_errors")
    require(not receipt.get("cleanup_errors"), "Fixture cleanup failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live-glm",
        action="store_true",
        help="Require two harmless real GLM turns; no fallback model",
    )
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/m3-workspace/receipt.json"
    )
    args = parser.parse_args()
    if not args.live_glm:
        parser.error("workspace attention qualification requires --live-glm")
    args.build_dir = args.build_dir.resolve()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    receipt = {
        "schema": "lapis.workspace-attention/1",
        "command": [sys.executable, *sys.argv],
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "limitations": [
            "Two controlled Codex turns, not arbitrary request kinds",
            "Codex route is verified live; upstream CCR routing needs corroborating router evidence",
        ],
        "checks": [],
        "passed": False,
        "live_model_turn": True,
    }
    started = time.monotonic()
    try:
        asyncio.run(exercise(args, receipt))
        receipt["passed"] = True
    except Exception as error:
        receipt["error"] = str(error) or type(error).__name__
        receipt["error_notes"] = list(getattr(error, "__notes__", []))
    finally:
        receipt["elapsed_seconds"] = round(time.monotonic() - started, 3)
        args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
