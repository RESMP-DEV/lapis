#!/usr/bin/env python3
"""Loopback OpenAI Responses and Anthropic Messages endpoints for QA.

Real Codex and Claude Code binaries talk to this server instead of a model
provider, so interactive testing spends no model usage. Replies are scripted
from the latest user message:

- "slow": stream text for about 25 seconds (working state, finish pulse).
- "approve" or "run": request a shell command that needs approval.
- "ask" or "question": ask a structured multiple-choice question (Codex).
- "flood": a long reply of many lines.
- "fail": an HTTP 500 for this turn.
- anything else: a short reply after one second.

After a tool result arrives, the reply is a short confirmation. Every request
is logged, without message text, to --log for diagnosing protocol mismatches.
"""

import argparse
import asyncio
import itertools
import json
import time
from pathlib import Path

COMMAND = "touch lapis-fake-approval.txt"
counter = itertools.count(1)


def sse(event, data):
    return (
        f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n".encode()
    )


def plan(text, has_tool_result):
    """Pick a scripted reply for the latest user text."""
    lowered = text.lower()
    if has_tool_result:
        return "text", ["Fake model: the command finished."], 0.3
    if "fail" in lowered:
        return "fail", [], 0
    if "approve" in lowered or "run" in lowered:
        return "tool", [], 0.5
    if "ask" in lowered or "question" in lowered:
        return "question", [], 0.5
    if "flood" in lowered:
        lines = [
            f"Fake model line {n}: " + "lorem ipsum " * 6 + "\n" for n in range(400)
        ]
        return "text", lines, 0.005
    if "slow" in lowered:
        return "text", [f"Working step {n}. " for n in range(50)], 0.5
    return "text", [f"Fake model reply to: {text[:80]}"], 1.0


# OpenAI Responses (Codex) ----------------------------------------------------


def responses_input(body):
    """Latest user text and whether the newest item is a tool result."""
    items = body.get("input") if isinstance(body.get("input"), list) else []
    text, tool_result = "", False
    for item in items:
        if not isinstance(item, dict):
            continue
        kind = item.get("type")
        if kind in (
            "function_call_output",
            "custom_tool_call_output",
            "local_shell_call_output",
        ):
            tool_result = True
        elif item.get("role") == "user":
            tool_result = False
            content = item.get("content")
            if isinstance(content, str):
                text = content
            elif isinstance(content, list):
                parts = [
                    part.get("text", "") for part in content if isinstance(part, dict)
                ]
                text = "".join(parts) or text
    return text, tool_result


def shell_tool(body):
    names = [
        tool.get("name") or tool.get("type")
        for tool in body.get("tools", [])
        if isinstance(tool, dict)
    ]
    for name in ("exec_command", "shell_command", "shell", "local_shell"):
        if name in names:
            return name
    return None


def shell_arguments(name):
    if name == "request_user_input":
        return {
            "questions": [
                {
                    "id": "lapis_fixture",
                    "header": "Fixture",
                    "question": "Which fixture answer should the fake model use?",
                    "options": [
                        {
                            "label": "First (Recommended)",
                            "description": "Pick the first fixture answer.",
                        },
                        {
                            "label": "Second",
                            "description": "Pick the second fixture answer.",
                        },
                    ],
                }
            ]
        }
    if name == "exec_command":
        return {
            "cmd": COMMAND,
            "sandbox_permissions": "require_escalated",
            "justification": "lapis QA approval fixture",
        }
    if name == "shell_command":
        return {
            "command": COMMAND,
            "sandbox_permissions": "require_escalated",
            "justification": "lapis QA approval fixture",
        }
    return {
        "command": ["bash", "-lc", COMMAND],
        "sandbox_permissions": "require_escalated",
        "justification": "lapis QA approval fixture",
    }


async def stream_responses(writer, body):
    serial = next(counter)
    response_id = f"resp_lapis_{serial}"
    model = body.get("model", "lapis-fake")
    text, tool_result = responses_input(body)
    kind, chunks, delay = plan(text, tool_result)
    tool = shell_tool(body)
    offered = {t.get("name") for t in body.get("tools", []) if isinstance(t, dict)}
    if kind == "question" and "request_user_input" in offered:
        kind, tool = "tool", "request_user_input"
    elif kind == "question":
        kind, chunks, delay = "text", ["Fake model: no question tool was offered."], 0.3
    if kind == "tool" and tool is None:
        kind, chunks, delay = "text", ["Fake model: no shell tool was offered."], 0.3
    if kind == "fail":
        return await http_error(writer, 500, "lapis fake model: intentional failure")
    await start_stream(writer)
    base = {
        "id": response_id,
        "object": "response",
        "created_at": int(time.time()),
        "model": model,
        "status": "in_progress",
        "output": [],
    }
    await send(
        writer, sse("response.created", {"type": "response.created", "response": base})
    )
    output = []
    if kind == "tool":
        await asyncio.sleep(delay)
        item = {
            "id": f"fc_lapis_{serial}",
            "type": "function_call",
            "status": "completed",
            "call_id": f"call_lapis_{serial}",
            "name": tool,
            "arguments": json.dumps(shell_arguments(tool)),
        }
        if tool == "local_shell":
            item = {
                "id": f"lsc_lapis_{serial}",
                "type": "local_shell_call",
                "status": "completed",
                "call_id": f"call_lapis_{serial}",
                "action": {"type": "exec", "command": ["bash", "-lc", COMMAND]},
            }
        await send(
            writer,
            sse(
                "response.output_item.added",
                {"type": "response.output_item.added", "output_index": 0, "item": item},
            ),
        )
        await send(
            writer,
            sse(
                "response.output_item.done",
                {"type": "response.output_item.done", "output_index": 0, "item": item},
            ),
        )
        output.append(item)
    else:
        message_id = f"msg_lapis_{serial}"
        added = {
            "id": message_id,
            "type": "message",
            "status": "in_progress",
            "role": "assistant",
            "content": [],
        }
        await send(
            writer,
            sse(
                "response.output_item.added",
                {
                    "type": "response.output_item.added",
                    "output_index": 0,
                    "item": added,
                },
            ),
        )
        await send(
            writer,
            sse(
                "response.content_part.added",
                {
                    "type": "response.content_part.added",
                    "item_id": message_id,
                    "output_index": 0,
                    "content_index": 0,
                    "part": {"type": "output_text", "text": "", "annotations": []},
                },
            ),
        )
        for chunk in chunks:
            await asyncio.sleep(delay)
            await send(
                writer,
                sse(
                    "response.output_text.delta",
                    {
                        "type": "response.output_text.delta",
                        "item_id": message_id,
                        "output_index": 0,
                        "content_index": 0,
                        "delta": chunk,
                    },
                ),
            )
        full = "".join(chunks)
        done = {
            **added,
            "status": "completed",
            "content": [{"type": "output_text", "text": full, "annotations": []}],
        }
        await send(
            writer,
            sse(
                "response.output_text.done",
                {
                    "type": "response.output_text.done",
                    "item_id": message_id,
                    "output_index": 0,
                    "content_index": 0,
                    "text": full,
                },
            ),
        )
        await send(
            writer,
            sse(
                "response.output_item.done",
                {"type": "response.output_item.done", "output_index": 0, "item": done},
            ),
        )
        output.append(done)
    usage = {
        "input_tokens": 1,
        "input_tokens_details": {"cached_tokens": 0},
        "output_tokens": 1,
        "output_tokens_details": {"reasoning_tokens": 0},
        "total_tokens": 2,
    }
    completed = {**base, "status": "completed", "output": output, "usage": usage}
    await send(
        writer,
        sse(
            "response.completed", {"type": "response.completed", "response": completed}
        ),
    )


# Anthropic Messages (Claude Code) ---------------------------------------------


def messages_input(body):
    text, tool_result = "", False
    for item in body.get("messages", []):
        if not isinstance(item, dict) or item.get("role") != "user":
            continue
        content = item.get("content")
        if isinstance(content, str):
            text, tool_result = content, False
            continue
        if not isinstance(content, list):
            continue
        blocks = [block for block in content if isinstance(block, dict)]
        tool_result = any(block.get("type") == "tool_result" for block in blocks)
        texts = [
            block.get("text", "") for block in blocks if block.get("type") == "text"
        ]
        # Claude Code prepends system reminders; the typed prompt is the last text block.
        if texts:
            text = texts[-1]
    return text, tool_result


def claude_message(serial, model, content):
    return {
        "id": f"msg_lapis_{serial}",
        "type": "message",
        "role": "assistant",
        "model": model,
        "content": content,
        "stop_reason": None,
        "stop_sequence": None,
        "usage": {"input_tokens": 1, "output_tokens": 1},
    }


async def stream_messages(writer, body):
    serial = next(counter)
    model = body.get("model", "lapis-fake")
    text, tool_result = messages_input(body)
    tools = [
        tool.get("name") for tool in body.get("tools", []) if isinstance(tool, dict)
    ]
    kind, chunks, delay = plan(text, tool_result)
    if kind == "tool" and "Bash" not in tools:
        kind, chunks, delay = "text", ["Fake model: no Bash tool was offered."], 0.3
    if kind == "fail":
        return await http_error(writer, 500, "lapis fake model: intentional failure")
    if not body.get("stream"):
        reply = "".join(chunks) if kind == "text" else "Fake model."
        message = claude_message(serial, model, [{"type": "text", "text": reply}])
        message["stop_reason"] = "end_turn"
        return await http_json(writer, message)
    await start_stream(writer)
    await send(
        writer,
        sse(
            "message_start",
            {"type": "message_start", "message": claude_message(serial, model, [])},
        ),
    )
    if kind == "tool":
        await asyncio.sleep(delay)
        block = {
            "type": "tool_use",
            "id": f"toolu_lapis_{serial}",
            "name": "Bash",
            "input": {},
        }
        arguments = json.dumps(
            {"command": COMMAND, "description": "lapis QA approval fixture"}
        )
        await send(
            writer,
            sse(
                "content_block_start",
                {"type": "content_block_start", "index": 0, "content_block": block},
            ),
        )
        await send(
            writer,
            sse(
                "content_block_delta",
                {
                    "type": "content_block_delta",
                    "index": 0,
                    "delta": {"type": "input_json_delta", "partial_json": arguments},
                },
            ),
        )
        stop = "tool_use"
    else:
        await send(
            writer,
            sse(
                "content_block_start",
                {
                    "type": "content_block_start",
                    "index": 0,
                    "content_block": {"type": "text", "text": ""},
                },
            ),
        )
        for chunk in chunks:
            await asyncio.sleep(delay)
            await send(
                writer,
                sse(
                    "content_block_delta",
                    {
                        "type": "content_block_delta",
                        "index": 0,
                        "delta": {"type": "text_delta", "text": chunk},
                    },
                ),
            )
        stop = "end_turn"
    await send(
        writer, sse("content_block_stop", {"type": "content_block_stop", "index": 0})
    )
    await send(
        writer,
        sse(
            "message_delta",
            {
                "type": "message_delta",
                "delta": {"stop_reason": stop, "stop_sequence": None},
                "usage": {"output_tokens": 1},
            },
        ),
    )
    await send(writer, sse("message_stop", {"type": "message_stop"}))


# HTTP plumbing ------------------------------------------------------------------


async def send(writer, data):
    writer.write(data)
    await writer.drain()


async def start_stream(writer):
    await send(
        writer,
        b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        b"Cache-Control: no-store\r\nConnection: close\r\n\r\n",
    )


async def http_json(writer, value, status="200 OK"):
    body = json.dumps(value).encode()
    await send(
        writer,
        (
            f"HTTP/1.1 {status}\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
        ).encode()
        + body,
    )


async def http_error(writer, status, message):
    reason = {404: "Not Found", 500: "Internal Server Error"}[status]
    await http_json(
        writer,
        {"error": {"type": "api_error", "message": message}},
        f"{status} {reason}",
    )


class Server:
    def __init__(self, log):
        self.log = log

    def record(self, method, path, body):
        entry = {"time": time.strftime("%H:%M:%S"), "method": method, "path": path}
        tools_file = self.log.with_name(
            self.log.stem + f"-tools{path.replace('/', '-')}.json"
        )
        if isinstance(body, dict) and body.get("tools") and not tools_file.exists():
            tools_file.write_text(json.dumps(body["tools"], indent=1))
        if isinstance(body, dict):
            entry["keys"] = sorted(body)
            entry["tools"] = [
                tool.get("name") or tool.get("type")
                for tool in body.get("tools", [])
                if isinstance(tool, dict)
            ]
            entry["stream"] = body.get("stream")
            entry["input_types"] = [
                item.get("type") or item.get("role")
                for item in body.get("input", [])
                if isinstance(item, dict)
            ]
            # How much conversation a Messages request carries (restore checks).
            if isinstance(body.get("messages"), list):
                entry["messages"] = len(body["messages"])
        with self.log.open("a") as stream:
            stream.write(json.dumps(entry) + "\n")

    def record_error(self, error):
        with self.log.open("a") as stream:
            stream.write(
                json.dumps(
                    {
                        "time": time.strftime("%H:%M:%S"),
                        # Exception text can include raw request data (e.g. a
                        # malformed Content-Length). Log only its safe category.
                        "error": type(error).__name__,
                    }
                )
                + "\n"
            )

    async def handle(self, reader, writer):
        try:
            head = await reader.readuntil(b"\r\n\r\n")
            lines = head.decode("latin-1").split("\r\n")
            method, target, _ = lines[0].split(" ", 2)
            headers = {
                key.lower(): value.strip()
                for key, value in (
                    line.split(":", 1) for line in lines[1:] if ":" in line
                )
            }
            length = int(headers.get("content-length", "0"))
            raw = await reader.readexactly(length) if length else b""
            body = json.loads(raw) if raw else {}
            if not isinstance(body, dict):
                raise ValueError("Request body must be a JSON object")
            path = target.split("?", 1)[0]
            self.record(method, path, body)
            if method == "POST" and path.endswith("/responses"):
                await stream_responses(writer, body)
            elif method == "POST" and path.endswith("/messages/count_tokens"):
                await http_json(writer, {"input_tokens": 1})
            elif method == "POST" and path.endswith("/messages"):
                await stream_messages(writer, body)
            elif method == "GET" and path.endswith("/models"):
                await http_json(writer, {"object": "list", "data": [], "models": []})
            else:
                await http_error(writer, 404, f"lapis fake model: no route for {path}")
        except (
            OSError,
            ValueError,
            asyncio.IncompleteReadError,
            asyncio.LimitOverrunError,
        ) as error:
            self.record_error(error)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=43110)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument(
        "--ready-file", type=Path, help="Write the bound loopback port after startup"
    )
    args = parser.parse_args()
    server = await asyncio.start_server(
        Server(args.log).handle, "127.0.0.1", args.port, limit=16 * 1024 * 1024
    )
    async with server:
        if args.ready_file:
            ready = args.ready_file.with_suffix(args.ready_file.suffix + ".tmp")
            ready.write_text(
                json.dumps({"port": server.sockets[0].getsockname()[1]}) + "\n"
            )
            ready.replace(args.ready_file)
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
