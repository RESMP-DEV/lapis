"""Probe installed Claude failure hooks with disposable, synthetic API replies."""

import argparse
import asyncio
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit

from probe_terminal import run_process


ROOT = Path(__file__).resolve().parents[1]
SUPPORTED_VERSION = "2.1.280"
MODEL = "claude-sonnet-4-5"
SYNTHETIC_PREFIX = "LAPIS_SYNTHETIC_MODEL_REPLY"
SYNTHETIC_TOOL_ID = "toolu_lapis_synthetic_read"
HOOK_EVENTS = (
    "SessionStart",
    "UserPromptSubmit",
    "PermissionRequest",
    "Notification",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "Stop",
    "StopFailure",
    "SessionEnd",
)
JSON_TYPES = {
    dict: "object",
    list: "array",
    str: "string",
    bool: "boolean",
    type(None): "null",
}


def json_type(value):
    if isinstance(value, int) and not isinstance(value, bool):
        return "number"
    return JSON_TYPES.get(type(value), "unknown")


def top_level_types(value):
    if not isinstance(value, dict):
        return {}
    return {str(key): json_type(item) for key, item in value.items()}


def write_json_line(path, value):
    encoded = (json.dumps(value, separators=(",", ":")) + "\n").encode()
    with path.open("ab") as stream:
        stream.write(encoded)


def capture_hook(output):
    raw = sys.stdin.buffer.read(1024 * 1024 + 1)
    if not raw or len(raw) > 1024 * 1024:
        raise ValueError("hook payload is empty or exceeds 1 MiB")
    payload = json.loads(raw)
    if not isinstance(payload, dict):
        raise ValueError("hook payload is not an object")
    event = payload.get("hook_event_name")
    if not isinstance(event, str) or event not in HOOK_EVENTS:
        raise ValueError("unknown hook event")
    write_json_line(
        output,
        {
            "hookEventName": event,
            "matchesSyntheticRead": (
                payload.get("tool_name") == "Read"
                and payload.get("tool_use_id") == SYNTHETIC_TOOL_ID
            ),
            "payloadTopLevelTypes": top_level_types(payload),
        },
    )


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def isolated_environment(home):
    env = {
        key: os.environ[key] for key in ("PATH", "TMPDIR", "LANG") if key in os.environ
    }
    config = home / "claude-config"
    config.mkdir(mode=0o700)
    (config / ".claude.json").write_text(
        json.dumps(
            {
                "hasCompletedOnboarding": True,
                "lastOnboardingVersion": SUPPORTED_VERSION,
                "projects": {
                    str(home.parent / "project"): {"hasTrustDialogAccepted": True}
                },
            }
        )
    )
    env.update(
        HOME=str(home / "project-home"),
        CLAUDE_CONFIG_DIR=str(config),
        ANTHROPIC_API_KEY="lapis-synthetic-key",
        CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1",
        DISABLE_AUTOUPDATER="1",
        CLAUDE_CODE_DISABLE_AUTOUPDATER="1",
        NO_COLOR="1",
    )
    (home / "project-home").mkdir(mode=0o700)
    return env


def hook_settings(output):
    command = shlex.join(
        [sys.executable, str(Path(__file__).resolve()), "--capture-hook", str(output)]
    )
    hooks = {
        event: [{"hooks": [{"type": "command", "command": command, "timeout": 5}]}]
        for event in HOOK_EVENTS
    }
    return json.dumps({"hooks": hooks}, separators=(",", ":"))


def sse(event, data):
    return (
        f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n".encode()
    )


def message_response(missing_path=None, sequence=1):
    """Emit the same Anthropic SSE envelope for text and optional Read blocks."""
    message = {
        "id": f"msg_lapis_synthetic_{sequence}",
        "type": "message",
        "role": "assistant",
        "model": MODEL,
        "content": [],
        "stop_reason": None,
        "stop_sequence": None,
        "usage": {"input_tokens": 1, "output_tokens": 2},
    }
    blocks = [
        (
            {"type": "text", "text": ""},
            {"type": "text_delta", "text": f"{SYNTHETIC_PREFIX}: fixture reply."},
        )
    ]
    if missing_path is not None:
        blocks.append(
            (
                {
                    "type": "tool_use",
                    "id": SYNTHETIC_TOOL_ID,
                    "name": "Read",
                    "input": {},
                },
                {
                    "type": "input_json_delta",
                    "partial_json": json.dumps({"file_path": str(missing_path)}),
                },
            )
        )
    events = [sse("message_start", {"type": "message_start", "message": message})]
    for index, (block, delta) in enumerate(blocks):
        events.extend(
            [
                sse(
                    "content_block_start",
                    {
                        "type": "content_block_start",
                        "index": index,
                        "content_block": block,
                    },
                ),
                sse(
                    "content_block_delta",
                    {"type": "content_block_delta", "index": index, "delta": delta},
                ),
                sse(
                    "content_block_stop", {"type": "content_block_stop", "index": index}
                ),
            ]
        )
    events.extend(
        [
            sse(
                "message_delta",
                {
                    "type": "message_delta",
                    "delta": {
                        "stop_reason": "tool_use"
                        if missing_path is not None
                        else "end_turn",
                        "stop_sequence": None,
                    },
                    "usage": {"output_tokens": 2},
                },
            ),
            sse("message_stop", {"type": "message_stop"}),
        ]
    )
    return b"".join(events)


class SyntheticAnthropicEndpoint:
    def __init__(self, scenario, missing_path):
        self.scenario = scenario
        self.missing_path = missing_path
        self.requests = []

    async def handle(self, reader, writer):
        try:
            async with asyncio.timeout(15):
                head = await reader.readuntil(b"\r\n\r\n")
            lines = head.decode("ascii").split("\r\n")
            method, path, _ = lines[0].split()
            headers = {
                key.lower(): value.strip()
                for key, value in (line.split(":", 1) for line in lines[1:] if line)
            }
            length = int(headers.get("content-length", "0"))
            if (
                method != "POST"
                or urlsplit(path).path != "/v1/messages"
                or not 0 < length <= 8 * 1024 * 1024
            ):
                body, response = b"", self.error(404, "unexpected endpoint")
            else:
                body = json.loads(await reader.readexactly(length))
                request = {
                    "path": urlsplit(path).path,
                    "topLevelTypes": top_level_types(body),
                    "model": body.get("model")
                    if isinstance(body.get("model"), str)
                    else None,
                    "message_roles": [
                        item.get("role")
                        for item in body.get("messages", [])
                        if isinstance(item, dict)
                    ],
                    "message_content_types": [
                        block.get("type")
                        for item in body.get("messages", [])
                        if isinstance(item, dict)
                        for block in (
                            item.get("content", [])
                            if isinstance(item.get("content"), list)
                            else []
                        )
                        if isinstance(block, dict)
                    ],
                    "tool_result_errors": [
                        block.get("is_error") is True
                        and block.get("tool_use_id") == SYNTHETIC_TOOL_ID
                        for item in body.get("messages", [])
                        if isinstance(item, dict)
                        and isinstance(item.get("content"), list)
                        for block in item["content"]
                        if isinstance(block, dict)
                        and block.get("type") == "tool_result"
                    ],
                    "toolNames": [
                        tool.get("name")
                        for tool in body.get("tools", [])
                        if isinstance(tool, dict) and isinstance(tool.get("name"), str)
                    ],
                }
                self.requests.append(request)
                if self.scenario == "post_tool_failure" and len(self.requests) == 1:
                    response = message_response(self.missing_path)
                elif self.scenario == "post_tool_failure":
                    response = message_response(sequence=len(self.requests))
                else:
                    response = self.error(
                        400, f"{SYNTHETIC_PREFIX}: intentional HTTP 400"
                    )
                if self.scenario == "post_tool_failure":
                    response = self.sse_headers(response)
                request["response_status"] = (
                    200 if self.scenario == "post_tool_failure" else 400
                )
            writer.write(response)
            await writer.drain()
        except (
            OSError,
            ValueError,
            TypeError,
            TimeoutError,
            asyncio.IncompleteReadError,
            asyncio.LimitOverrunError,
        ):
            pass
        finally:
            writer.close()
            try:
                async with asyncio.timeout(1):
                    await writer.wait_closed()
            except (OSError, TimeoutError):
                pass

    @staticmethod
    def error(status, label):
        body = json.dumps(
            {
                "type": "error",
                "error": {"type": "invalid_request_error", "message": label},
            }
        ).encode()
        reason = {400: "Bad Request", 404: "Not Found"}[status]
        return (
            f"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
        ).encode() + body

    @staticmethod
    def sse_headers(body):
        return (
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n"
            f"Content-Length: {len(body)}\r\n\r\n"
        ).encode() + body


def read_hooks(path):
    if not path.is_file():
        return []
    events = []
    for line in path.read_text().splitlines():
        value = json.loads(line)
        if value.get("hookEventName") in HOOK_EVENTS:
            events.append(value)
    return events


def hook_names(events):
    names = []
    for event in events:
        name = event.get("hookEventName")
        if name not in names:
            names.append(name)
    return names


async def run_claude(binary, environment, arguments, timeout):
    return await asyncio.to_thread(
        run_process,
        [str(binary), *arguments],
        cwd=str(environment["LAPIS_FIXTURE_CWD"]),
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


async def scenario(binary, output, scenario_name, prompt, timeout):
    output.parent.mkdir(parents=True, exist_ok=True)
    hook_output = (output.parent / f"{scenario_name}.hooks.jsonl").resolve()
    hook_output.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f"claude-{scenario_name}-", dir=output.parent
    ) as temporary:
        root = Path(temporary)
        home = root / "home"
        home.mkdir(mode=0o700)
        project = root / "project"
        project.mkdir(mode=0o700)
        environment = isolated_environment(home)
        environment["LAPIS_FIXTURE_CWD"] = str(project)
        missing = project / "lapis-definitely-missing.txt"
        endpoint = SyntheticAnthropicEndpoint(scenario_name, missing)
        server = await asyncio.start_server(endpoint.handle, "127.0.0.1", 0)
        async with server:
            address = server.sockets[0].getsockname()
            environment["ANTHROPIC_BASE_URL"] = f"http://127.0.0.1:{address[1]}"
            arguments = [
                "--print",
                "--verbose",
                "--output-format",
                "stream-json",
                "--include-hook-events",
                "--no-session-persistence",
                "--setting-sources",
                "",
                "--strict-mcp-config",
                "--mcp-config",
                '{"mcpServers":{}}',
                "--tools",
                "Read" if scenario_name == "post_tool_failure" else "",
                "--model",
                MODEL,
                "--permission-mode",
                "default",
                "--settings",
                hook_settings(hook_output),
                "--",
                prompt,
            ]
            started = time.monotonic()
            timed_out = False
            try:
                completed = await run_claude(binary, environment, arguments, timeout)
                stderr = completed.stderr
            except subprocess.TimeoutExpired as error:
                timed_out = True
                stderr = error.stderr or b""
            elapsed = round(time.monotonic() - started, 3)
        events = read_hooks(hook_output)
        names = hook_names(events)
        known_names = [
            name
            for name in names
            if name != "PostToolUseFailure" and name != "StopFailure"
        ]
        result = {
            "scenario": scenario_name,
            "elapsed_seconds": elapsed,
            "timed_out": timed_out,
            "exit_code": None if timed_out else completed.returncode,
            "stderr_bytes": len(stderr),
            "observed_hook_names": names,
            "hook_payload_inventory": events,
            "synthetic_api_requests": endpoint.requests,
            "synthetic_responses": (
                [
                    f"{SYNTHETIC_PREFIX}: one Read tool_use",
                    f"{SYNTHETIC_PREFIX}: final text",
                ]
                if scenario_name == "post_tool_failure"
                else [f"{SYNTHETIC_PREFIX}: intentional HTTP 400"]
            ),
            "fixture_valid": not timed_out and bool(known_names),
        }
        if scenario_name == "post_tool_failure":
            result["fixture_valid"] = (
                result["fixture_valid"]
                and completed.returncode == 0
                and any(
                    event["hookEventName"] == "PostToolUseFailure"
                    and event["matchesSyntheticRead"]
                    for event in events
                )
                and any(
                    any(request["tool_result_errors"]) for request in endpoint.requests
                )
                and len(endpoint.requests) >= 2
                and "Read"
                in (endpoint.requests[0].get("toolNames") if endpoint.requests else [])
            )
            result["postToolUseFailureObserved"] = "PostToolUseFailure" in names
            result["support"] = (
                "observed" if "PostToolUseFailure" in names else "not_observed"
            )
            result["limitation"] = (
                None
                if "PostToolUseFailure" in names
                else "The failed-Read hook contract was not established by this run."
            )
        else:
            result["fixture_valid"] = (
                result["fixture_valid"]
                and bool(endpoint.requests)
                and completed.returncode == 1
            )
            result["stopFailureObserved"] = "StopFailure" in names
            result["support"] = "observed" if "StopFailure" in names else "not_observed"
            result["limitation"] = (
                None
                if "StopFailure" in names
                else "HTTP 400 reached the CLI and failed the turn, but StopFailure was not emitted in this print-mode fixture."
            )
        return result


def version_identity(binary, output):
    environment = {
        key: os.environ[key] for key in ("PATH", "TMPDIR", "LANG") if key in os.environ
    }
    with tempfile.TemporaryDirectory(
        prefix="claude-version-", dir=output.parent
    ) as temporary:
        result = run_process(
            [str(binary), "--version"],
            cwd=temporary,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=15,
            text=True,
        )
        reported = (result.stdout + result.stderr).strip()
        if result.returncode != 0 or SUPPORTED_VERSION not in reported:
            raise RuntimeError(
                f"unexpected Claude version or version failure: {reported}"
            )
        return reported


async def collect(binary, output, timeout):
    names = ("post_tool_failure", "stop_failure")
    results = await asyncio.gather(
        *(
            scenario(
                binary,
                output,
                name,
                "Probe fixture: execute the synthetic reply.",
                timeout,
            )
            for name in names
        )
    )
    return dict(zip(names, results, strict=True))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build/claude-hooks/failure-contract.json",
    )
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--capture-hook", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.capture_hook:
        return args
    if not 20 <= args.timeout <= 180:
        parser.error("--timeout must be between 20 and 180 seconds")
    args.output = args.output.resolve()
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.capture_hook:
        capture_hook(args.capture_hook)
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    executable = shutil.which("claude")
    if executable is None:
        raise RuntimeError("claude is not installed")
    binary = Path(executable).resolve(strict=True)
    binary_hash = file_hash(binary)
    started = time.monotonic()
    receipt = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "binary": {
            "path": str(binary),
            "version": version_identity(binary, args.output),
            "sha256": binary_hash,
        },
        "registered_hook_events": list(HOOK_EVENTS),
        "settings_acceptance_check": "Require known hooks from the same ten-event candidate settings.",
        "isolation": {
            "home": "disposable temporary HOME",
            "config": "disposable CLAUDE_CONFIG_DIR",
            "setting_sources": "none; explicit --settings only",
            "credentials": "synthetic ANTHROPIC_API_KEY only",
            "api": "loopback 127.0.0.1 only",
            "project_trust": "disposable project only",
            "global_settings_written": False,
        },
        "model_replies": "synthetic SSE/text generated by this probe; no external inference",
        "transcript_policy": "hook and CLI records are reduced to names and JSON key/type inventories",
    }
    try:
        receipt["results"] = asyncio.run(collect(binary, args.output, args.timeout))
        receipt["passed"] = all(
            result["fixture_valid"] for result in receipt["results"].values()
        )
        receipt["postToolUseFailure"] = receipt["results"]["post_tool_failure"][
            "support"
        ]
        receipt["stopFailure"] = receipt["results"]["stop_failure"]["support"]
    except Exception as error:
        receipt["passed"] = False
        receipt["error"] = f"{type(error).__name__}: {error}"
    finally:
        receipt["elapsed_seconds"] = round(time.monotonic() - started, 3)
        if file_hash(binary) != binary_hash:
            receipt["binary_changed_during_probe"] = True
            receipt["passed"] = False
        temporary = args.output.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(receipt, indent=2) + "\n")
        temporary.replace(args.output)
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
