"""Qualify Claude Code hooks through a disposable real service and terminal."""

import argparse
import asyncio
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from check_cli_launch import TEXT, Service, require
from check_service_attention import View, cleanup_service, process_groups
from probe_terminal import run_process

ROOT = Path(__file__).resolve().parents[1]
SUPPORTED_VERSION = "2.1.280"
MODEL = "zai,glm-5.3"
FIXTURE_COMMAND = 'python3 -c "print(123456789)"'
FIXTURE_PROMPT = (
    f"Use Bash to run exactly {FIXTURE_COMMAND} and reply DONE. Do not use other tools."
)
QUESTION_PROMPT = (
    "Use AskUserQuestion now with exactly two options labeled A and B. "
    "Ask me to choose A or B, then reply DONE. Do not use any other tools."
)


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def settings_identity():
    # Inspect only the settings file, never recursively scan private transcripts/caches.
    path = Path.home() / ".claude/settings.json"
    return file_hash(path) if path.is_file() else None


def claude_version(executable):
    result = run_process(
        [executable, "--version"],
        cwd=ROOT,
        timeout=15,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    output = (result.stdout + result.stderr).strip()
    require(result.returncode == 0, f"claude --version failed: {output}")
    match = re.search(r"\d+\.\d+\.\d+", output)
    require(
        match is not None and match.group() == SUPPORTED_VERSION,
        f"This fixture expects Claude {SUPPORTED_VERSION}; installed: {output}",
    )
    return match.group()


def launch_arguments(prompt):
    return [
        "--setting-sources",
        "local",
        "--strict-mcp-config",
        "--mcp-config",
        '{"mcpServers":{}}',
        "--model",
        MODEL,
        "--permission-mode",
        "default",
        "--",
        prompt,
    ]


def fixture_config(config, directory):
    config.mkdir(mode=0o700)
    project_settings = directory / ".claude"
    project_settings.mkdir(mode=0o700)
    (project_settings / "settings.local.json").write_text(
        json.dumps({"permissions": {"ask": ["Bash"]}})
    )
    # Trust only this disposable fixture; tool approval remains default/manual.
    (config / ".claude.json").write_text(
        json.dumps(
            {
                "hasCompletedOnboarding": True,
                "theme": "dark",
                "lastOnboardingVersion": SUPPORTED_VERSION,
                "projects": {str(directory): {"hasTrustDialogAccepted": True}},
            }
        )
    )


def terminal_notice(request, tool):
    details = request.get("details", {})
    require(
        details.get("adapter") == "claude-code"
        and details.get("responseLocation") == "terminal"
        and details.get("observationOnly") is True
        and details.get("toolName") == tool
        and request.get("choices") == []
        and not request.get("submitted"),
        "Expected an unsubmitted observation-only Claude terminal notice",
    )
    require(
        not ({"tool_input", "toolInput", "prompt", "tool_response"} & details.keys()),
        "Attention details leaked private tool/prompt content",
    )
    return request


def notice(view, tool):
    if not view.attention or not view.attention["ready"]:
        return None
    return next(
        (r for r in view.attention["requests"] if r["details"].get("toolName") == tool),
        None,
    )


def stable_notice(before, after):
    require(
        all(after[key] == before[key] for key in ("id", "epoch", "revision"))
        and not after["submitted"],
        "Reconnection or observation changed the pending request identity",
    )


def screen_contains(screen, marker):
    return re.sub(r"\s+", "", marker) in re.sub(r"\s+", "", screen)


async def wait_initial_hook(view):
    async with asyncio.timeout(30):
        while not view.attention or not view.attention["connected"]:
            if view.task.done():
                await view.task
            if view.attention and view.attention["epoch"]:
                raise RuntimeError(view.attention["diagnostic"])
            await asyncio.sleep(0.02)


async def wait_notice(view, tool):
    await view.wait(lambda: notice(view, tool) is not None, 120)
    return terminal_notice(notice(view, tool), tool)


async def clear_session(view, receipt):
    previous_epoch = view.attention["epoch"]
    view.client.send(TEXT, b"/clear")
    await asyncio.sleep(0.25)
    view.client.send(TEXT, b"\r")
    try:
        async with asyncio.timeout(15):
            while not (
                view.attention
                and view.attention["connected"]
                and view.attention["ready"]
                and view.attention["epoch"] > previous_epoch
            ):
                if view.task.done():
                    await view.task
                await asyncio.sleep(0.02)
    except TimeoutError as error:
        raise RuntimeError(
            "Claude /clear did not start a fresh observation epoch: "
            + str(view.attention.get("diagnostic") if view.attention else None)
        ) from error
    require(not view.attention["requests"], "Cleared session retained old notices")
    receipt["clear_epochs"] = [previous_epoch, view.attention["epoch"]]


async def capture_desktop(args, service, receipt):
    if not args.capture:
        return
    binary = (
        args.build_dir / "apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop"
    )
    command = [
        binary,
        "--discover",
        "--socket",
        service.endpoint,
        "--cwd",
        service.directory,
        "--claude",
        "--capture",
        args.capture,
        "--",
        service.program,
        *service.arguments,
    ]
    result = await asyncio.to_thread(
        run_process,
        command,
        cwd=ROOT,
        timeout=30,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    (args.output.parent / "desktop.log").write_text(result.stdout + result.stderr)
    require(
        result.returncode == 0 and args.capture.is_file(), "Claude GUI capture failed"
    )
    receipt["desktop_capture"] = str(args.capture)


async def exercise(args, receipt):
    executable = shutil.which("claude")
    require(executable is not None, "Claude Code is not installed")
    executable = Path(executable).resolve()
    binary = args.build_dir / "services/session/lapis_session_service"
    require(binary.is_file(), "Build the desktop/service first")
    receipt.update(
        version=claude_version(executable),
        binary_sha256=file_hash(executable),
        service_sha256=file_hash(binary),
        model=MODEL,
        fixture={
            "base_url": "http://127.0.0.1:3456",
            "production_default": False,
            "private_config": True,
            "permission_mode": "default",
            "project_permission_ask": ["Bash"],
            "setting_sources": ["local"],
        },
        source_revision=subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, timeout=10
        ).strip(),
        source_dirty=bool(
            subprocess.check_output(
                ["git", "status", "--porcelain"], cwd=ROOT, text=True, timeout=10
            ).strip()
        ),
    )
    before_settings = settings_identity()
    (ROOT / "runtime").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ch-", dir=ROOT / "runtime") as temporary:
        runtime = Path(temporary)
        directory = runtime / "fixture"
        directory.mkdir(mode=0o700)
        config = runtime / "config"
        fixture_config(config, directory)
        service = Service(
            binary,
            runtime,
            args.output.parent,
            "claude",
            str(executable),
            launch_arguments(FIXTURE_PROMPT),
            directory,
            {
                "CLAUDE_CONFIG_DIR": str(config),
                "ANTHROPIC_BASE_URL": "http://127.0.0.1:3456",
                "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
            },
            claude=True,
        )
        view = None
        groups = set()
        try:
            view = View(await asyncio.to_thread(service.connect))
            groups = await asyncio.to_thread(process_groups, service.child_pid)
            await wait_initial_hook(view)
            before = await wait_notice(view, "Bash")
            require(not before["item"], "PermissionRequest unexpectedly has a tool ID")
            receipt["permission_notice"] = before
            await asyncio.sleep(1)
            stable_notice(before, notice(view, "Bash"))
            await view.close()
            view = None
            await capture_desktop(args, service, receipt)
            view = View(await asyncio.to_thread(service.connect))
            after = await wait_notice(view, "Bash")
            stable_notice(before, after)
            receipt["checks"].append(
                "same child and pending notice after detach/reattach"
            )
            await view.wait(
                lambda: (
                    screen_contains(view.screen, FIXTURE_COMMAND)
                    and screen_contains(view.screen, "Do you want")
                ),
                15,
            )
            # Cancel this permission screen. No hook or desktop decision is sent.
            view.client.send(TEXT, b"\x1b")
            await asyncio.sleep(1)
            view.client.send(TEXT, QUESTION_PROMPT.encode())
            await asyncio.sleep(0.25)
            view.client.send(TEXT, b"\r")
            question = await wait_notice(view, "AskUserQuestion")
            require(
                question["item"] and question["turn"] != before["turn"],
                "Expected a new turn and exact AskUserQuestion tool ID",
            )
            require(
                all(r["id"] != before["id"] for r in view.attention["requests"]),
                "New prompt did not retire the prior advisory notice",
            )
            receipt["question_notice"] = question
            await view.wait(
                lambda: (
                    screen_contains(view.screen, "1. A")
                    and screen_contains(view.screen, "2. B")
                    and screen_contains(view.screen, "Enter to select")
                ),
                15,
            )
            view.client.send(TEXT, b"\r")
            await view.wait(
                lambda: (
                    notice(view, "AskUserQuestion") is None
                    or screen_contains(view.screen, "Submit answers")
                ),
                15,
            )
            if notice(view, "AskUserQuestion") is not None:
                view.client.send(TEXT, b"\r")
            await view.wait(lambda: notice(view, "AskUserQuestion") is None, 15)
            receipt["question_retirement_activity"] = view.attention["activity"]
            await view.wait(lambda: view.attention["activity"] == 3, 30)
            receipt["checks"].append(
                "terminal answer retired question and turn completed"
            )
            receipt["checks"].extend(
                [
                    "permission remained terminal-only",
                    "new prompt retired prior permission advisory",
                    "live AskUserQuestion has exact tool identity",
                ]
            )
            await clear_session(view, receipt)
            view.client.send(TEXT, FIXTURE_PROMPT.encode())
            await asyncio.sleep(0.25)
            view.client.send(TEXT, b"\r")
            fresh = await wait_notice(view, "Bash")
            require(
                fresh["thread"] != before["thread"]
                and fresh["epoch"] > question["epoch"],
                "Claude /clear did not replace the source identity",
            )
            receipt["post_clear_notice"] = fresh
            receipt["checks"].append(
                "same Claude child delivered fresh permission attention after /clear"
            )
        finally:
            original_error = sys.exception()
            try:
                if view is not None:
                    try:
                        (args.output.parent / "fixture-screen.txt").write_text(
                            view.screen
                        )
                    except OSError as error:
                        receipt["optional_screen_error"] = str(error)
                    await view.close()
            finally:
                await cleanup_service(
                    service, groups, receipt, original_error or sys.exception()
                )
    after_settings = settings_identity()
    receipt["global_settings_hashes"] = {
        "before": before_settings,
        "after": after_settings,
    }
    require(
        after_settings == before_settings,
        "Global Claude settings changed during the probe; writer not attributed",
    )
    receipt["global_settings_unchanged"] = True


async def bounded_exercise(args, receipt):
    async with asyncio.timeout(args.timeout):
        await exercise(args, receipt)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/claude-hooks/live-receipt.json"
    )
    parser.add_argument(
        "--capture", type=Path, help="Capture the real pending notice in the GUI"
    )
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args(argv)
    if not 30 <= args.timeout <= 900:
        parser.error("--timeout must be between 30 and 900 seconds")
    args.build_dir = args.build_dir.resolve()
    args.output = args.output.resolve()
    if args.capture:
        args.capture = args.capture.resolve()
        args.capture.parent.mkdir(parents=True, exist_ok=True)
    return args


def main(argv=None):
    args = parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    receipt = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "passed": False,
        "checks": [],
    }
    started = time.monotonic()
    try:
        asyncio.run(bounded_exercise(args, receipt))
        receipt["passed"] = True
    except BaseException as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
        receipt["error_details"] = getattr(error, "__notes__", [])
    receipt["elapsed_seconds"] = round(time.monotonic() - started, 3)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
