"""Protocol diagnostics for the QA model fixture."""

import asyncio
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fake_models import Server


class ProtocolDiagnosticsTests(unittest.IsolatedAsyncioTestCase):
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
