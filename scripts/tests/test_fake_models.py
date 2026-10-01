"""Protocol diagnostics for the QA model fixture."""

import asyncio
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fake_models import Server, messages_input, plan, stream_messages


class ProtocolDiagnosticsTests(unittest.IsolatedAsyncioTestCase):
    def test_parallel_scenario_uses_two_bash_calls_then_completes(self):
        prompt = "Use exactly two distinct Bash tool calls now."
        self.assertEqual(plan(prompt, False), ("parallel_tools", [], 0.5))
        self.assertEqual(
            plan(prompt, True),
            ("parallel_done", ["Fake model: both parallel commands finished."], 0.3),
        )
        self.assertEqual(messages_input({"messages": []}), ("", 0))

    async def test_parallel_stream_has_distinct_bash_blocks_then_completion(self):
        class FakeWriter:
            def __init__(self):
                self.chunks = []

            def write(self, data):
                self.chunks.append(data)

            async def drain(self):
                return None

        async def response(body):
            writer = FakeWriter()
            await stream_messages(writer, body)
            parsed = []
            for item in b"".join(writer.chunks).split(b"\n\n"):
                if not item.startswith(b"event: "):
                    continue
                name, raw_data = item.split(b"\ndata: ", 1)
                parsed.append(
                    (name.removeprefix(b"event: ").decode(), json.loads(raw_data))
                )
            return parsed

        prompt = "Use exactly two distinct Bash tool calls now."
        events = await response(
            {
                "stream": True,
                "model": "lapis-fake",
                "tools": [{"name": "Bash"}],
                "messages": [
                    {"role": "user", "content": [{"type": "text", "text": prompt}]}
                ],
            }
        )
        starts = [event[1] for event in events if event[0] == "content_block_start"]
        self.assertEqual([item["index"] for item in starts], [0, 1])
        self.assertEqual(
            [item["content_block"]["name"] for item in starts], ["Bash", "Bash"]
        )
        ids = [item["content_block"]["id"] for item in starts]
        self.assertEqual(len(ids), len(set(ids)))
        arguments = [
            json.loads(event[1]["delta"]["partial_json"])
            for event in events
            if event[0] == "content_block_delta"
        ]
        self.assertEqual(
            [item["command"] for item in arguments],
            ["echo lapis-parallel-one", "echo lapis-parallel-two"],
        )
        self.assertEqual(events[-2][1]["delta"]["stop_reason"], "tool_use")
        self.assertEqual(events[-1][0], "message_stop")

        completion = await response(
            {
                "stream": True,
                "model": "lapis-fake",
                "tools": [{"name": "Bash"}],
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": prompt},
                            {"type": "tool_result", "tool_use_id": ids[0]},
                            {"type": "tool_result", "tool_use_id": ids[1]},
                        ],
                    }
                ],
            }
        )
        self.assertEqual(completion[-2][1]["delta"]["stop_reason"], "end_turn")
        self.assertEqual(completion[-1][0], "message_stop")
        self.assertIn(
            "both parallel commands finished.",
            completion[-4][1]["delta"]["text"],
        )

    async def test_unknown_request_fields_do_not_change_the_parallel_stream(self):
        class Writer:
            def __init__(self):
                self.body = b""

            def write(self, data):
                self.body += data

            async def drain(self):
                return None

        prompt = "Use exactly two distinct Bash tool calls now."
        writer = Writer()
        await stream_messages(
            writer,
            {
                "stream": True,
                "unknown_top_level": "private-top-sentinel",
                "model": "lapis-fake",
                "tools": [
                    {"name": "Bash", "unknown_tool": "private-tool-sentinel"},
                    {"unknown_tool_entry": True},
                ],
                "messages": [
                    {
                        "role": "user",
                        "unknown_message": "private-message-sentinel",
                        "content": [
                            {
                                "type": "text",
                                "text": prompt,
                                "unknown_block": "private-block-sentinel",
                            }
                        ],
                    }
                ],
            },
        )
        self.assertEqual(writer.body.count(b'"content_block_start"'), 2)
        self.assertIn(b"echo lapis-parallel-one", writer.body)
        self.assertIn(b"echo lapis-parallel-two", writer.body)
        self.assertNotIn(b"private-", writer.body)

    async def test_parse_failures_are_logged_while_server_stays_available(self):
        malformed_requests = [
            b"NOT-HTTP\r\n\r\n",
            b"POST /responses HTTP/1.1\r\nContent-Length: private-prompt-sentinel\r\n\r\n",
            b'POST /responses HTTP/1.1\r\nContent-Length: 8\r\n\r\n{"bad": ',
        ]
        for body in (b"[]", b'"private-prompt-sentinel"', b"42", b"null"):
            malformed_requests.append(
                b"POST /responses HTTP/1.1\r\nContent-Length: "
                + str(len(body)).encode()
                + b"\r\n\r\n"
                + body
            )
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "requests.jsonl"
            server = await asyncio.start_server(Server(log).handle, "127.0.0.1", 0)
            try:
                for raw in malformed_requests:
                    reader, writer = await asyncio.open_connection(
                        *server.sockets[0].getsockname()
                    )
                    try:
                        writer.write(raw)
                        writer.write_eof()
                        await writer.drain()
                        # Server.handle logs before closing its side. Client
                        # closure alone does not establish that completion.
                        self.assertEqual(await asyncio.wait_for(reader.read(), 2), b"")
                    finally:
                        writer.close()
                        await writer.wait_closed()
                reader, writer = await asyncio.open_connection(
                    *server.sockets[0].getsockname()
                )
                try:
                    writer.write(b"GET /models HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
                    await writer.drain()
                    response = await asyncio.wait_for(reader.read(), 2)
                finally:
                    writer.close()
                    await writer.wait_closed()
                head, body = response.split(b"\r\n\r\n", 1)
                self.assertTrue(head.startswith(b"HTTP/1.1 200 OK\r\n"))
                self.assertIn(b"Content-Type: application/json", head)
                self.assertEqual(
                    json.loads(body), {"object": "list", "data": [], "models": []}
                )
            finally:
                server.close()
                await server.wait_closed()

            entries = [json.loads(line) for line in log.read_text().splitlines()]
            errors = [entry for entry in entries if "error" in entry]
            records = [entry for entry in entries if "method" in entry]
            self.assertEqual(len(errors), len(malformed_requests))
            self.assertEqual(
                [entry["error"].split(":", 1)[0] for entry in errors],
                ["ValueError", "ValueError", "JSONDecodeError"] + ["ValueError"] * 4,
            )
            self.assertTrue(all(set(entry) == {"time", "error"} for entry in errors))
            self.assertEqual([entry["method"] for entry in records], ["GET"])
            self.assertEqual(
                len(log.read_text().splitlines()), len(malformed_requests) + 1
            )
            self.assertNotIn("private-prompt-sentinel", log.read_text())


if __name__ == "__main__":
    unittest.main()
