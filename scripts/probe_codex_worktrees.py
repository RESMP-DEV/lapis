"""Qualify Codex worktree exposure and fail-close thread behavior."""

import argparse
import asyncio
import hashlib
import json
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.request import urlopen

from check_cli_launch import ATTENTION_DECISION, Service, TEXT
from check_codex_attention import (
    MODEL,
    Client,
    RpcError,
    probe_configuration_arguments,
    trust_fixture_directory,
)
from check_service_attention import (
    View,
    cleanup_service,
    decision,
    fixture_trust_prompt,
    initialize_fixture_owner,
    process_groups,
    record_launch_contract,
)
from codex_probe_transport import UnixWebSocketTransport


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SHA256 = "1e6d87e0d5b0e862e48f35fd816a7b9425c7988ef7c185d56be07d632a65ffef"
EXPECTED_REVISION = "481280ec261ba1a1f8f0c205ed301825fe28f9bc"
RELEVANT_EVENTS = {
    "thread/started",
    "thread/status/changed",
    "turn/started",
    "turn/completed",
    "item/tool/requestUserInput",
    "serverRequest/resolved",
    "thread/archived",
}


async def eventually(predicate, timeout=90, description="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        await asyncio.sleep(0.05)
    raise RuntimeError(f"Timed out waiting for {description}")


class Journal:
    """Consume the owner's broadcast queue and retain bounded protocol facts."""

    def __init__(self, owner):
        self.owner = owner
        self.events = []
        self.running = True
        self.task = asyncio.create_task(self.run())

    async def run(self):
        while self.running:
            try:
                message = await asyncio.wait_for(self.owner.events.get(), 0.1)
            except TimeoutError:
                continue
            method = message.get("method")
            if method not in RELEVANT_EVENTS:
                continue
            params = message.get("params", {})
            entry = {
                "method": method,
                "thread": params.get("threadId") or params.get("thread", {}).get("id"),
                "ephemeral": params.get("thread", {}).get("ephemeral"),
            }
            if method == "turn/completed":
                entry["status"] = params.get("turn", {}).get("status")
            if "id" in message:
                entry["request"] = True
            self.events.append(entry)

    async def close(self):
        self.running = False
        await self.task


def sanitized_events(events, original, second, limit=256):
    """Replace thread identities without retaining private conversation IDs."""
    identities = {original: "original", second: "second"}
    result = []
    for event in events:
        if len(result) >= limit:
            break
        thread = event.get("thread")
        entry = dict(event)
        entry["thread"] = identities.get(thread, "other" if thread else None)
        result.append(entry)
    return result


def run_git(repository, *arguments):
    subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def create_repository(home, repository):
    repository.mkdir()
    run_git(repository, "init", "--quiet", "-b", "main")
    run_git(repository, "config", "user.name", "Lapis Probe")
    run_git(repository, "config", "user.email", "probe@example.invalid")
    (repository / "README.md").write_text("disposable worktree fixture\n")
    run_git(repository, "add", "README.md")
    run_git(repository, "commit", "--quiet", "--message", "fixture")
    trust_fixture_directory(home, repository)


async def model_turn(owner, thread, text, *, plan=False):
    params = {
        "threadId": thread,
        "input": [{"type": "text", "text": text}],
    }
    if plan:
        params["collaborationMode"] = {
            "mode": "plan",
            "settings": {"model": MODEL, "reasoning_effort": "low"},
        }
    return await owner.rpc("turn/start", params)


async def wait_turn(owner, thread, turn, timeout=120):
    """Wait for a terminal turn while tolerating only a brief startup RPC race."""
    deadline = time.monotonic() + timeout
    grace = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            result = await owner.rpc(
                "thread/read", {"threadId": thread, "includeTurns": True}
            )
        except RpcError as error:
            if error.code != -32603 or time.monotonic() >= grace:
                raise
            await asyncio.sleep(0.1)
            continue
        found = [
            item
            for item in result["thread"]["turns"]
            if item["id"] == turn and item["status"] != "inProgress"
        ]
        if found:
            return result
        await asyncio.sleep(0.1)
    raise RuntimeError(f"Timed out waiting for turn {turn} completion")


def flatten_items(thread_result):
    values = []
    for turn in thread_result.get("thread", {}).get("turns", []):
        values.extend(turn.get("items", []))
    return values


def verify_model_fixture():
    with urlopen("http://127.0.0.1:3456/v1/models", timeout=3) as response:
        payload = json.load(response)
    models = [entry.get("id") for entry in payload.get("data", [])]
    if MODEL not in models:
        raise RuntimeError(f"Local GLM fixture does not advertise {MODEL}")


def source_state():
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    dirty = subprocess.check_output(
        ["git", "status", "--short"], cwd=ROOT, text=True
    ).splitlines()
    return revision, dirty


def sanitize_launch_contract(receipt):
    contract = receipt.get("launch_contract", {})
    contract["managed_daemon_state_paths_checked"] = [
        "<disposable CODEX_HOME>/app-server-daemon",
        "<disposable CODEX_HOME>/packages/app-server-daemon",
    ]
    contract.pop("backend_process_group", None)


async def exercise(args, receipt):
    binary = args.codex.resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest != args.expected_sha256:
        raise RuntimeError(f"Unexpected Codex executable SHA-256: {digest}")
    verify_model_fixture()
    receipt["codex_sha256"] = digest
    artifacts = args.output.parent
    artifacts.mkdir(parents=True, exist_ok=True)
    (ROOT / "runtime").mkdir(mode=0o700, exist_ok=True)
    view = owner = journal = None
    service = None
    groups = set()
    with tempfile.TemporaryDirectory(
        prefix="codex-p2-", dir=ROOT / "runtime"
    ) as temporary:
        runtime = Path(temporary)
        home, repository = runtime / "home", runtime / "repository"
        home.mkdir()
        create_repository(home, repository)
        options = probe_configuration_arguments() + [
            "-c",
            "features.worktrees=true",
            "-c",
            'approval_policy="on-request"',
            "-c",
            'approvals_reviewer="user"',
            "-c",
            'sandbox_mode="read-only"',
            "--no-alt-screen",
        ]
        service = Service(
            args.build_dir / "services/session/lapis_session_service",
            runtime,
            artifacts,
            "attention",
            str(binary),
            options,
            repository,
            {
                "CODEX_HOME": str(home),
                "CODEX_APP_SERVER_MANAGED_CONFIG_PATH": str(home / "managed.toml"),
            },
            codex=True,
        )
        try:
            view = View(await asyncio.to_thread(service.connect))
            groups = await asyncio.to_thread(process_groups, service.process.pid)
            if len(groups) != 2:
                raise RuntimeError("Expected independently guarded TUI/backend groups")
            await view.wait(lambda: view.attention is not None, 15)
            owner = Client(
                await UnixWebSocketTransport.connect(
                    Path(str(service.endpoint) + ".codex")
                )
            )
            await initialize_fixture_owner(owner, home, False)
            await eventually(
                lambda: "Codex" in view.screen or "context left" in view.screen,
                15,
                "TUI startup",
            )
            if fixture_trust_prompt(view.screen, repository):
                view.client.send(TEXT, b"\r")
            journal = Journal(owner)
            async with asyncio.timeout(15):
                while True:
                    loaded = await owner.rpc("thread/loaded/list", {"limit": 10})
                    if loaded.get("data"):
                        break
                    await asyncio.sleep(0.1)
            persistent = []
            for candidate in loaded["data"]:
                found = await owner.rpc(
                    "thread/read", {"threadId": candidate, "includeTurns": False}
                )
                if not found["thread"]["ephemeral"]:
                    persistent.append(candidate)
            if len(persistent) != 1:
                raise RuntimeError("Fixture must start with one persistent TUI thread")
            original = persistent[0]
            await asyncio.to_thread(
                record_launch_contract, service, home, artifacts, receipt
            )
            sanitize_launch_contract(receipt)
            receipt["checks"].append("dedicated managed TUI launch contract")

            unavailable_result = None
            receipt["failed_attempts"] = []
            for attempt in range(2):
                unavailable = await model_turn(
                    owner,
                    original,
                    "Attempt to use create_worktree exactly once with allowAsync true "
                    "and ref refs/heads/main. If that tool is unavailable, reply with "
                    "just WORKTREE_UNAVAILABLE; do not use exec_command or other tools.",
                )
                result = await wait_turn(owner, original, unavailable["turn"]["id"])
                attachments = await owner.rpc(
                    "thread/attachment/list", {"threadId": original, "limit": 10}
                )
                messages = [
                    item.get("text", "")
                    for item in flatten_items(result)
                    if item.get("type") == "agentMessage"
                ]
                commands = [
                    item
                    for item in flatten_items(result)
                    if item.get("type") == "commandExecution"
                ]
                if (
                    not attachments["data"]
                    and not commands
                    and any("WORKTREE_UNAVAILABLE" in message for message in messages)
                ):
                    unavailable_result = result
                    break
                receipt["failed_attempts"].append(
                    {
                        "attempt": attempt + 1,
                        "terminal_status": next(
                            item["status"]
                            for item in result["thread"]["turns"]
                            if item["id"] == unavailable["turn"]["id"]
                        ),
                    }
                )
            if unavailable_result is None:
                raise RuntimeError("Explicit-remote worktree exposure changed")
            receipt["worktree_tools"] = "unavailable-on-explicit-remote"
            receipt["checks"].append("model observed no worktree tools or attachment")

            restored = await model_turn(
                owner,
                original,
                "Use request_user_input now to ask one question with id "
                "restore_choice: choose Continue or Cancel. Do not use other "
                "tools. Wait for my answer.",
                plan=True,
            )
            await view.wait(
                lambda: view.attention["ready"] and bool(view.attention["requests"])
            )
            restore_request = view.attention["requests"][0]
            if restore_request["thread"] != original:
                raise RuntimeError("Restore request belongs to another thread")

            second_started = await owner.rpc(
                "thread/start",
                {
                    "cwd": str(repository),
                    "model": MODEL,
                    "modelProvider": "lapis_probe",
                    "approvalPolicy": "on-request",
                    "approvalsReviewer": "user",
                    "sandbox": "read-only",
                    "ephemeral": False,
                    "allowProviderModelFallback": False,
                    "developerInstructions": "Bounded lapis P2 fixture.",
                },
            )
            second = second_started["thread"]["id"]
            second_turn = await model_turn(
                owner, second, "Reply with just ATTACHED_DONE. Do not use tools."
            )
            await wait_turn(owner, second, second_turn["turn"]["id"])
            receipt["persistent_threads"] = 2
            await eventually(
                lambda: view.attention is not None and not view.attention["connected"],
                15,
                "second persistent thread fail-close",
            )
            if (
                view.attention["diagnostic"]
                != "Codex observer requires one stable thread"
                or view.attention["ready"]
            ):
                raise RuntimeError("Second persistent thread did not fail closed")
            receipt["checks"].append("second persistent thread disabled observer")
            disabled = decision(
                restore_request,
                "submit",
                {"restore_choice": {"answers": ["Continue"]}},
            )
            view.client.send(ATTENTION_DECISION, disabled)
            await eventually(
                lambda: (
                    "Request changed or response is unsupported"
                    in view.attention["diagnostic"]
                ),
                5,
                "disabled response rejection",
            )
            receipt["checks"].append("disabled observer rejected structured response")

            for thread in (original, second):
                resumed = await owner.rpc(
                    "thread/resume", {"threadId": thread, "excludeTurns": True}
                )
                read = await owner.rpc(
                    "thread/read", {"threadId": thread, "includeTurns": True}
                )
                if resumed["thread"]["id"] != thread or read["thread"]["id"] != thread:
                    raise RuntimeError(
                        "Per-thread reconciliation returned another thread"
                    )
            receipt["checks"].append("both threads resume/read by identity")

            await owner.rpc("thread/archive", {"threadId": second})
            await view.close()
            view = View(await asyncio.to_thread(service.connect))
            await view.wait(
                lambda: view.attention is not None and view.attention["ready"]
            )
            await view.wait(
                lambda: view.attention["ready"] and bool(view.attention["requests"])
            )
            recovered = view.attention["requests"][0]
            if (
                recovered["id"] != restore_request["id"]
                or type(recovered["id"]) is not type(restore_request["id"])
                or recovered["thread"] != original
            ):
                raise RuntimeError("Original request did not reconcile by identity")
            view.client.send(
                ATTENTION_DECISION,
                decision(
                    recovered,
                    "submit",
                    {"restore_choice": {"answers": ["Continue"]}},
                ),
            )
            await view.wait(
                lambda: view.attention["ready"] and not view.attention["requests"]
            )
            await wait_turn(owner, original, restored["turn"]["id"])
            receipt["checks"].append(
                "original pending request reconciled and answered after second archive"
            )

            await model_turn(
                owner,
                original,
                "Use request_user_input now to ask one question with id "
                "archive_choice: choose Continue or Cancel. Do not use other "
                "tools. Wait for my answer.",
                plan=True,
            )
            await view.wait(
                lambda: view.attention["ready"] and bool(view.attention["requests"])
            )
            pending = view.attention["requests"][0]
            if pending["thread"] != original:
                raise RuntimeError("Archive request belongs to another thread")
            await owner.rpc("thread/archive", {"threadId": original})
            await eventually(
                lambda: view.attention is not None and not view.attention["connected"],
                15,
                "mid-turn archive fail-close",
            )
            stale = decision(
                pending, "submit", {"archive_choice": {"answers": ["Continue"]}}
            )
            view.client.send(ATTENTION_DECISION, stale)
            await eventually(
                lambda: (
                    "Request changed or response is unsupported"
                    in view.attention["diagnostic"]
                ),
                5,
                "stale response rejection",
            )
            if view.attention["requests"] or view.retry not in (None, stale):
                raise RuntimeError("Archived source exposed stale request state")
            receipt["checks"].append("mid-turn archive disabled source and stale reply")
            receipt["events"] = sanitized_events(journal.events, original, second)
            receipt["passed"] = True
        finally:
            if journal:
                await journal.close()
            if view:
                (artifacts / "screen.txt").write_text(view.screen)
                if view.attention:
                    (artifacts / "attention.json").write_text(
                        json.dumps(view.attention, indent=2, default=str) + "\n"
                    )
                await view.close()
            if owner:
                await owner.close()
            await cleanup_service(service, groups, receipt, sys.exception())


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--codex",
        type=Path,
        default=Path(shutil.which("codex") or ""),
        help="Exact qualified Codex executable",
    )
    parser.add_argument("--source-revision", default=EXPECTED_REVISION)
    parser.add_argument("--expected-sha256", default=EXPECTED_SHA256)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/codex-p2/receipt.json"
    )
    args = parser.parse_args(argv)
    if not args.codex:
        parser.error("a Codex executable is required")
    args.codex = args.codex.resolve()
    args.build_dir = args.build_dir.resolve()
    args.output = args.output.resolve()
    return args


def main():
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    revision, dirty = source_state()
    receipt = {
        "schema": "lapis.codex-p2-worktree-probe/1",
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "passed": False,
        "source": {
            "lapis_revision": revision,
            "lapis_dirty_files": dirty,
            "candidate_revision": args.source_revision,
            "binary_sha256": args.expected_sha256,
            "platform": f"{platform.system()} {platform.machine()}",
        },
        "model": MODEL,
        "model_provider": "lapis_probe loopback fixture",
        "scope": (
            "P2 live probe of worktool exposure on Lapis's explicit-remote managed "
            "TUI, followed by two persistent threads, per-thread replay, response "
            "fail-close and mid-turn archive. It does not qualify embedded or "
            "local-daemon worktree creation."
        ),
        "checks": [],
        "non_claims": [
            "No managed worktree was created because the explicit-remote TUI did not expose its tools.",
            "No worktree attachment, task-tool delegation, native input, GPU, package or Linux behavior is claimed.",
            "A future Codex target that exposes worktree tools on explicit remote requires a new probe.",
        ],
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
