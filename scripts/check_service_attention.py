"""Qualify managed Codex service attention in a disposable macOS fixture."""

import argparse
import asyncio
import contextlib
import hashlib
import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from check_cli_launch import (
    ATTENTION_DECISION,
    ATTENTION_RETRY,
    ATTENTION_SNAPSHOT,
    SNAPSHOT,
    STATUS,
    TEXT,
    VERSION,
    CheckError,
    Service,
    decode_snapshot,
    require,
)
from check_codex_attention import (
    MODEL,
    Client,
    approved_fixture,
    probe_configuration_arguments,
)
from codex_probe_transport import UnixWebSocketTransport
from probe_codex_attention import initialize
from probe_terminal import run_process

ROOT = Path(__file__).resolve().parents[1]


class Reader:
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


def snapshot(data):
    reader = Reader(data)
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


def string(value):
    data = value.encode("utf-8")
    return struct.pack(">I", len(data)) + data


def decision(request, choice, answers=None, revision=None):
    identifier = request["id"]
    identity = (
        b"\0" + struct.pack(">q", identifier)
        if type(identifier) is int
        else b"\1" + string(identifier)
    )
    payload = (
        struct.pack(">IQ", VERSION, request["epoch"])
        + identity
        + struct.pack(">Q", request["revision"] if revision is None else revision)
        + string(choice)
        + string(
            json.dumps(
                answers or {}, ensure_ascii=False, sort_keys=True, separators=(",", ":")
            )
        )
    )
    require(len(payload) <= 65536, "Oversized decision fixture")
    return payload


class View:
    """Consume terminal and attention frames without blocking the RPC client."""

    def __init__(self, client):
        self.client = client
        self.attention = None
        self.retry = None
        self.screen = (client.cached_snapshot or {}).get("text", "")
        self.running = True
        self.task = asyncio.create_task(self.pump())

    async def pump(self):
        while self.running:
            try:
                kind, data = await asyncio.to_thread(self.client.receive, 0.2)
            except socket.timeout:
                continue
            except CheckError as error:
                if str(error) != "Frame deadline expired":
                    raise
                continue
            if kind == ATTENTION_SNAPSHOT:
                value = snapshot(data)
                require(
                    value["attachment"] == self.client.attachment,
                    "Attention attachment mismatch",
                )
                self.attention = value
            elif kind == SNAPSHOT:
                require(
                    data[:40] == self.client.attachment, "Terminal attachment mismatch"
                )
                self.screen = decode_snapshot(data[72:])["text"]
            elif kind == ATTENTION_RETRY:
                require(
                    data[:40] == self.client.attachment, "Rejection attachment mismatch"
                )
                self.retry = data[40:]
            elif kind == STATUS:
                raise RuntimeError(
                    "Service returned status: " + data[1:].decode("utf-8")
                )
            else:
                raise RuntimeError("Unexpected service frame")

    async def wait(self, predicate, timeout=90):
        async with asyncio.timeout(timeout):
            while not predicate():
                if self.task.done():
                    await self.task
                if self.attention and not self.attention["connected"]:
                    raise RuntimeError(self.attention["diagnostic"])
                await asyncio.sleep(0.02)

    async def close(self):
        self.running = False
        try:
            await self.task
        finally:
            self.client.close()


def process_groups(parent=None):
    rows = subprocess.check_output(
        ["ps", "-axo", "pid=,ppid=,pgid="], text=True, timeout=5
    )
    values = [tuple(map(int, row.split())) for row in rows.splitlines()]
    return {
        group
        for pid, ppid, group in values
        if parent is None or (ppid == parent and pid == group)
    }


async def wait_groups_gone(groups):
    async with asyncio.timeout(5):
        while groups & await asyncio.to_thread(process_groups):
            await asyncio.sleep(0.05)


async def cleanup_service(service, groups, receipt, original_error):
    errors = []
    try:
        await asyncio.to_thread(service.stop)
        receipt["service_reaped"] = service.process.poll() is not None
    except Exception as error:
        errors.append(f"service cleanup: {str(error) or type(error).__name__}")
    if groups:
        try:
            await wait_groups_gone(groups)
            receipt["owned_process_groups_cleaned"] = True
        except Exception as error:
            errors.append(
                f"process-group cleanup: {str(error) or type(error).__name__}"
            )
    if errors:
        receipt["cleanup_errors"] = errors
        if original_error is None:
            raise CheckError("; ".join(errors))
        for error in errors:
            original_error.add_note(error)


async def finished_turn(owner, thread, turn, expected="completed"):
    async with asyncio.timeout(90):
        while True:
            result = await owner.rpc(
                "thread/read", {"threadId": thread, "includeTurns": True}
            )
            found = [item for item in result["thread"]["turns"] if item["id"] == turn]
            if found and found[0]["status"] != "inProgress":
                require(
                    found[0]["status"] == expected,
                    f"Expected fixture turn status {expected}, got {found[0]['status']}; "
                    f"error kind: {(found[0].get('error') or {}).get('codexErrorInfo')}",
                )
                return
            await asyncio.sleep(0.1)


async def question_turn(owner, thread, question_id):
    return await owner.rpc(
        "turn/start",
        {
            "threadId": thread,
            "input": [
                {
                    "type": "text",
                    "text": (
                        "Use request_user_input now to ask two questions in one "
                        f"request with ids {question_id}_first and {question_id}_second. "
                        f"For {question_id}_first choose Blue or Green; for "
                        f"{question_id}_second choose Red or Yellow. Do not use any "
                        "other tool. After I answer both, reply with just the selected "
                        "colors separated by a space."
                    ),
                }
            ],
            "collaborationMode": {
                "mode": "plan",
                "settings": {
                    "model": MODEL,
                    "reasoning_effort": "low",
                    "developer_instructions": None,
                },
            },
        },
    )


async def desktop_response(args, service, artifacts, request, choice, answers=None):
    """Exercise the production QML controls against this exact disposable request."""
    labels = (
        selected_question_answers(request["details"].get("questions", []))
        if choice == "submit"
        else {}
    )
    questions_by_id = {
        question["id"]: question for question in request["details"].get("questions", [])
    }
    option_indices = {
        identifier: next(
            index
            for index, option in enumerate(questions_by_id[identifier]["options"])
            if option["label"] == label
        )
        for identifier, label in labels.items()
    }
    config = artifacts / f"desktop-{choice}.json"
    config.write_text(
        json.dumps(
            {
                "endpoint": str(service.endpoint),
                "program": service.program,
                "arguments": service.arguments,
                "directory": str(service.directory),
                "details": request["details"],
                "choice": choice,
                "answers": answers or {},
                "optionIndices": option_indices,
                "capture": str(artifacts / f"desktop-{choice}.png"),
            }
        )
    )
    with (artifacts / f"desktop-{choice}.log").open("w") as log:
        result = await asyncio.to_thread(
            run_process,
            [args.build_dir / "apps/desktop/lapis_attention_ui_probe", config],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=60,
        )
    require(result.returncode == 0, f"Desktop {choice} probe failed; see its log")


def selected_question_answers(questions):
    identifiers = [question.get("id") for question in questions]
    require(
        len(questions) == 2
        and len(set(identifiers)) == len(identifiers)
        and set(identifiers) == {"color_first", "color_second"},
        "Unexpected question fixture",
    )
    expected_labels = {"color_first": "Blue", "color_second": "Red"}
    labels = {}
    for identifier, expected_label in expected_labels.items():
        question = next(
            question for question in questions if question["id"] == identifier
        )
        matches = [
            option.get("label")
            for option in question.get("options", [])
            if option.get("label")
            in (expected_label, f"{expected_label} (Recommended)")
        ]
        require(len(matches) == 1, "Unexpected question answers")
        labels[identifier] = matches[0]
    return labels


async def simultaneous_approvals(owner, view, thread, receipt):
    started = await owner.rpc(
        "turn/start",
        {
            "threadId": thread,
            "input": [
                {
                    "type": "text",
                    "text": 'Call exec_command twice in parallel, as two separate tool calls. Each call must run exactly python3 -c "print(123456789)" with sandbox_permissions require_escalated and justification "Verify lapis simultaneous approvals". Issue both calls before waiting for either result. Do not combine commands or use any other command. After both finish reply DONE.',
                }
            ],
            "collaborationMode": {
                "mode": "default",
                "settings": {
                    "model": MODEL,
                    "reasoning_effort": "low",
                    "developer_instructions": None,
                },
            },
        },
    )
    await view.wait(
        lambda: view.attention["ready"] and len(view.attention["requests"]) == 2, 30
    )
    first, second = view.attention["requests"]
    for request in (first, second):
        require(
            request["thread"] == thread and request["turn"] == started["turn"]["id"],
            "Parallel request context mismatch",
        )
        require(
            approved_fixture(request["details"].get("command")),
            "Refusing non-fixture parallel command",
        )
    require(
        type(first["id"]) is not type(second["id"]) or first["id"] != second["id"],
        "Parallel identities collided",
    )
    view.client.send(ATTENTION_DECISION, decision(first, "accept"))
    await view.wait(lambda: len(view.attention["requests"]) == 1)
    remaining = view.attention["requests"][0]
    require(
        type(remaining["id"]) is type(second["id"]) and remaining["id"] == second["id"],
        "First resolution removed the other request",
    )
    require(not remaining["submitted"], "First decision submitted the other request")
    view.client.send(ATTENTION_DECISION, decision(remaining, "accept"))
    await view.wait(lambda: view.attention["ready"] and not view.attention["requests"])
    await finished_turn(owner, thread, started["turn"]["id"])
    receipt["checks"].append(
        "two simultaneous real approvals retain distinct identities and resolve independently"
    )


async def exercise(args, receipt):
    executable = shutil.which("codex")
    require(executable is not None, "codex is not installed")
    binary = Path(executable).resolve()
    receipt["codex_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    receipt["source"] = (
        "ordinary TUI created thread, service-owned backend and observer"
    )
    artifacts = args.output.parent
    (ROOT / "runtime").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="m2-", dir=ROOT / "runtime") as temporary:
        runtime = Path(temporary)
        home, cwd = runtime / "home", runtime / "cwd"
        home.mkdir()
        cwd.mkdir()
        options = probe_configuration_arguments()
        options += [
            "-c",
            'approval_policy="on-request"',
            "-c",
            'approvals_reviewer="user"',
            "-c",
            'sandbox_mode="read-only"',
            "--no-alt-screen",
        ]
        if args.live_glm:
            prompt = 'Use exec_command to run exactly python3 -c "print(123456789)". Set sandbox_permissions to require_escalated and justification to "Verify lapis approval routing" so this harmless fixture asks for approval before execution. Do not use other commands or tools. After execution reply DONE.'
            options += ["--", prompt]
        service = Service(
            args.build_dir / "services/session/lapis_session_service",
            runtime,
            artifacts,
            "attention",
            str(binary),
            options,
            cwd,
            {
                "CODEX_HOME": str(home),
                "CODEX_APP_SERVER_MANAGED_CONFIG_PATH": str(home / "managed.toml"),
            },
            codex=True,
        )
        view = None
        owner = None
        groups = set()
        try:
            view = View(await asyncio.to_thread(service.connect))
            groups = await asyncio.to_thread(process_groups, service.process.pid)
            require(
                len(groups) == 2,
                "Expected independently guarded TUI and backend groups",
            )
            await view.wait(lambda: view.attention is not None, 15)
            # Only this newly created empty fixture can receive trust confirmation.
            await view.wait(
                lambda: (
                    "Press enter to continue" in view.screen
                    or "context left" in view.screen
                    or "Codex" in view.screen
                ),
                15,
            )
            if "Press enter to continue" in view.screen:
                view.client.send(TEXT, b"\r")
            owner = Client(
                await UnixWebSocketTransport.connect(
                    Path(str(service.endpoint) + ".codex")
                )
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
            require(len(persistent) == 1, "Fixture must have one persistent TUI thread")
            thread = persistent[0]
            receipt["checks"].append("managed TUI startup and attention attachment")
            if not args.live_glm:
                await view.close()
                view = View(await asyncio.to_thread(service.connect))
                await view.wait(lambda: view.attention is not None, 15)
                receipt["checks"].append(
                    "same-child detach and reattach without a model turn"
                )
                await view.close()
                view = None
                await owner.close()
                owner = None
                service.process.kill()
                await asyncio.to_thread(service.process.wait, timeout=5)
                await wait_groups_gone(groups)
                receipt["checks"].append(
                    "abrupt service death cleans up TUI and backend groups"
                )
                return
            await view.wait(
                lambda: view.attention["ready"] and bool(view.attention["requests"])
            )
            request = view.attention["requests"][0]
            require(request["thread"] == thread, "Attention belongs to another thread")
            require(
                approved_fixture(request["details"].get("command")),
                "Refusing non-fixture command",
            )
            require("accept" in request["choices"], "No qualified approval response")
            live = await owner.rpc(
                "thread/resume", {"threadId": thread, "excludeTurns": True}
            )
            require(
                live["model"] == MODEL and live["modelProvider"] == "lapis_probe",
                "Unexpected runtime model/provider",
            )
            receipt["model"] = live["model"]
            receipt["model_provider"] = live["modelProvider"]
            # Detach with the approval pending; restored state must be actionable.
            await view.close()
            view = View(await asyncio.to_thread(service.connect))
            await view.wait(
                lambda: (
                    view.attention is not None
                    and view.attention["ready"]
                    and bool(view.attention["requests"])
                )
            )
            restored = view.attention["requests"][0]
            require(
                restored["id"] == request["id"]
                and type(restored["id"]) is type(request["id"]),
                "Request changed across detach",
            )
            request = restored
            view.client.send(
                ATTENTION_DECISION,
                decision(request, "accept", revision=request["revision"] + 100),
            )
            await view.wait(lambda: "Request changed" in view.attention["diagnostic"])
            require(
                not view.attention["requests"][0]["submitted"],
                "Stale decision consumed token",
            )
            if args.desktop:
                await view.close()
                view = None
                await desktop_response(args, service, artifacts, request, "accept")
                view = View(await asyncio.to_thread(service.connect))
                await view.wait(lambda: view.attention is not None)
                receipt["checks"].append(
                    "real command approval through desktop controls"
                )
            else:
                payload = decision(request, "accept")
                view.client.send(ATTENTION_DECISION, payload)
                view.client.send(ATTENTION_DECISION, payload)
                receipt["checks"].append("duplicate decision sent after first decision")
            await view.wait(
                lambda: view.attention["ready"] and not view.attention["requests"]
            )
            await finished_turn(owner, thread, request["turn"])
            receipt["checks"] += [
                "real command approval over service IPC",
                "pending request survives same-child reattachment",
                "stale token rejected",
                "source resolution and successful continuation",
            ]
            started = await question_turn(owner, thread, "color")
            await view.wait(
                lambda: view.attention["ready"] and bool(view.attention["requests"])
            )
            request = view.attention["requests"][0]
            require(
                request["thread"] == thread
                and request["turn"] == started["turn"]["id"],
                "Question context mismatch",
            )
            questions = request["details"].get("questions", [])
            labels = selected_question_answers(questions)
            # Invalid answers are rejected before source submission, with an
            # explicit retry receipt; queued older snapshots are not that receipt.
            invalid = decision(request, "submit")
            view.client.send(ATTENTION_DECISION, invalid)
            await view.wait(lambda: view.retry == invalid)
            require(
                not view.attention["requests"][0]["submitted"],
                "Rejected answers consumed token",
            )
            receipt["checks"].append(
                "invalid answers rejected with an exact retry token"
            )
            answers = {
                "color_first": {"answers": [labels["color_first"]]},
                "color_second": {"answers": [labels["color_second"]]},
            }
            if args.desktop:
                await view.close()
                view = None
                await desktop_response(
                    args, service, artifacts, request, "submit", answers
                )
                view = View(await asyncio.to_thread(service.connect))
                await view.wait(lambda: view.attention is not None)
                receipt["checks"].append(
                    "real user-input answer through desktop controls"
                )
            else:
                view.client.send(
                    ATTENTION_DECISION, decision(request, "submit", answers)
                )
            await view.wait(
                lambda: view.attention["ready"] and not view.attention["requests"]
            )
            await finished_turn(owner, thread, request["turn"])
            receipt["checks"].append(
                "real user-input answer over service IPC and successful continuation"
            )
            previous_epoch = view.attention["epoch"]
            await owner.rpc("thread/archive", {"threadId": thread})
            await view.wait(lambda: not view.attention["connected"], 15)
            require(not view.attention["ready"], "Archived source still allows replies")
            await owner.rpc("thread/unarchive", {"threadId": thread})
            await owner.rpc("thread/resume", {"threadId": thread, "excludeTurns": True})
            await view.close()
            view = View(await asyncio.to_thread(service.connect))
            await view.wait(
                lambda: view.attention is not None and view.attention["ready"]
            )
            require(
                view.attention["epoch"] > previous_epoch,
                "Source reconnect reused its epoch",
            )
            require(
                not view.attention["requests"],
                "Resolved requests resurrected on reconnect",
            )
            receipt["checks"].append(
                "archived source disables replies; restored source reconciles in a fresh epoch"
            )
            interrupted = await question_turn(owner, thread, "cancel_check")
            await view.wait(
                lambda: view.attention["ready"] and bool(view.attention["requests"])
            )
            cancelled = view.attention["requests"][0]
            require(
                cancelled["thread"] == thread
                and cancelled["turn"] == interrupted["turn"]["id"],
                "Cancellation request context mismatch",
            )
            cancellation_ids = [
                question.get("id")
                for question in cancelled["details"].get("questions", [])
            ]
            require(
                len(cancellation_ids) == 2
                and len(set(cancellation_ids)) == len(cancellation_ids)
                and set(cancellation_ids)
                == {"cancel_check_first", "cancel_check_second"},
                "Unexpected cancellation fixture",
            )
            await owner.rpc(
                "turn/interrupt", {"threadId": thread, "turnId": cancelled["turn"]}
            )
            await view.wait(
                lambda: view.attention["ready"] and not view.attention["requests"]
            )
            await finished_turn(owner, thread, cancelled["turn"], "interrupted")
            receipt["checks"].append(
                "new request after source recovery is cancelled by an observed turn interruption"
            )
            await simultaneous_approvals(owner, view, thread, receipt)
        finally:
            original_error = sys.exception()
            if view and view.attention:
                receipt["last_attention"] = {
                    key: view.attention[key]
                    for key in ("ready", "connected", "diagnostic", "activity")
                }
                receipt["last_request_count"] = len(view.attention["requests"])
                with contextlib.suppress(OSError):
                    (artifacts / "fixture-screen.txt").write_text(view.screen)
                    (artifacts / "pending-fixture.json").write_text(
                        json.dumps(view.attention["requests"], indent=2) + "\n"
                    )
            if owner:
                with contextlib.suppress(Exception):
                    await owner.close()
            if view:
                with contextlib.suppress(Exception):
                    await view.close()
            await cleanup_service(service, groups, receipt, original_error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live-glm",
        action="store_true",
        help="Explicitly run the harmless GLM approval fixture",
    )
    parser.add_argument(
        "--desktop",
        action="store_true",
        help="Use Qt desktop controls for live decisions",
    )
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/m2-service/receipt.json"
    )
    args = parser.parse_args()
    if args.desktop and not args.live_glm:
        parser.error("--desktop requires --live-glm")
    args.build_dir = args.build_dir.resolve()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    receipt = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "checks": [],
        "passed": False,
        "live_model_turn": args.live_glm,
        "desktop_controls": args.desktop,
    }
    started = time.monotonic()
    try:
        asyncio.run(exercise(args, receipt))
        receipt["passed"] = True
    except Exception as error:
        receipt["error"] = str(error) or type(error).__name__
    finally:
        receipt["elapsed_seconds"] = round(time.monotonic() - started, 3)
        args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
