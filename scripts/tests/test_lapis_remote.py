"""The iPhone gateway: snapshots, launch identity, admission and a live service."""

import http.client
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps" / "remote"))
import lapis_remote as remote  # noqa: E402

SERVICE = ROOT / "build" / "desktop" / "services" / "session" / "lapis_session_service"


def snapshot(columns, rows, cells, *, cursor=(0, 0), palette=None, alternate=False):
    """Encode a terminal snapshot the way local_protocol.cpp does."""
    graphemes, encoded = [], b""
    for text, kind, foreground, background, underline, flags in cells:
        points = [ord(character) for character in text]
        encoded += struct.pack(">IIB", len(graphemes), len(points), kind) + struct.pack(
            ">BIBIBIBH", *foreground, *background, 0, 0, underline, flags
        )
        graphemes += points
    palette = palette or [0] * 256
    header = struct.pack(">QHHHH", 7, columns, rows, *cursor)
    header += bytes([1, 1, 0, 0, 0, int(alternate), 0, 1])
    header += struct.pack(">II", 0xDDDDDD, 0x111111) + b"\0" + struct.pack(">I", 0)
    header += struct.pack(">QQQ", rows, 0, rows) + struct.pack(">256I", *palette)
    return (
        header
        + struct.pack(">I", len(graphemes))
        + struct.pack(f">{len(graphemes)}I", *graphemes)
        + struct.pack(">I", len(cells))
        + encoded
    )


DEFAULT = (0, 0)


def cell(text, kind=0, foreground=DEFAULT, background=DEFAULT, underline=0, flags=0):
    return (text, kind, foreground, background, underline, flags)


class SnapshotTests(unittest.TestCase):
    def test_runs_colors_cursor_and_wide_cells(self):
        palette = [0] * 256
        palette[1] = 0xFF0000
        payload = snapshot(
            4,
            2,
            [
                cell("a"),
                cell("b", foreground=(1, 1)),
                cell("漢", kind=1),
                cell("", kind=2),
                cell("x", flags=1),
                cell("y", flags=16),
                cell(""),
                cell("z", background=(2, 0x00FF00), underline=1),
            ],
            cursor=(1, 1),
            palette=palette,
            alternate=True,
        )
        frame = remote.render_snapshot(payload)
        self.assertEqual((frame["columns"], frame["rows"]), (4, 2))
        self.assertTrue(frame["alternateScreen"] and frame["applicationCursor"])
        self.assertEqual(
            frame["lines"][0],
            [["a", None, None, 0], ["b", "#ff0000", None, 0], ["漢", None, None, 0]],
        )
        # Inverse swaps in the defaults; the cursor cell is its own run.
        self.assertEqual(
            frame["lines"][1],
            [
                ["x", None, None, remote.BOLD],
                ["y", "#111111", "#dddddd", remote.CURSOR],
                [" ", None, None, 0],
                ["z", None, "#00ff00", remote.UNDERLINE],
            ],
        )
        self.assertEqual(frame["cursor"], {"x": 1, "y": 1, "visible": True})

    def test_malformed_snapshots_are_refused(self):
        payload = snapshot(2, 1, [cell("a"), cell("b")])
        for broken in (payload[:-1], payload[:100], payload + b"\0"):
            with self.assertRaises(remote.GatewayError):
                remote.render_snapshot(broken)


class IdentityTests(unittest.TestCase):
    # Pinned in services/session/tests/launch_spec_test.cpp.
    PINNED = {
        "": "7843c56fa9e8312702dc9138b4738a8a380b6e911386f6baa62ac54787ab42d4",
        "codex": "3481055a2227832731bcea701d32fa2747c082778bda7712bea8b554ceb0f83d",
        "claude": "dd6176abd6e16ec30ef870d91a43209995ca94bcd7440c5f9f8474872b68ef17",
    }

    def test_fingerprints_match_the_service(self):
        for mode, expected in self.PINNED.items():
            with self.subTest(mode=mode):
                digest = remote.fingerprint("/bin/sh", ["resume", "abc"], "/", mode)
                self.assertEqual(digest.hex(), expected)

    def test_registry_is_read_as_the_desktop_reads_it(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            registry = runtime / "workspace.json"

            def entry(identifier, **fields):
                return {
                    "id": identifier,
                    "endpoint": str(runtime / (identifier + ".sock")),
                    "program": "/bin/sh",
                    "directory": "/",
                    "category": "general",
                    **fields,
                }

            registry.write_text(
                json.dumps(
                    {
                        "categories": [{"id": "general", "name": "General"}],
                        "agents": [
                            entry("a", harness="codex", resumeThread="t"),
                            entry(
                                "b", harness="claude", mode="claude", arguments=["-x"]
                            ),
                            entry("c", harness="claude", mode=""),
                            entry("d", harness="grok", arguments=[]),
                            {**entry("e"), "endpoint": "/tmp/elsewhere.sock"},
                        ],
                    }
                )
            )
            agents = {
                agent["id"]: agent
                for agent in remote.load_workspace(registry)["agents"]
            }
        self.assertEqual(sorted(agents), ["a", "b", "c", "d"])
        self.assertEqual(
            (agents["a"]["mode"], agents["a"]["arguments"]), ("codex", ["resume", "t"])
        )
        self.assertEqual(
            (agents["b"]["mode"], agents["b"]["arguments"]), ("claude", ["-x"])
        )
        self.assertEqual(agents["c"]["mode"], "")
        self.assertEqual(agents["d"]["mode"], "")


def fake_tailscale(peers):
    """A runner answering `tailscale status/whois --json` from a table."""

    def runner(command):
        if command[1] == "status":
            return {
                "Self": {
                    "UserID": 7,
                    "TailscaleIPs": ["100.64.0.1"],
                    "DNSName": "mac.example.ts.net.",
                },
                "User": {"7": {"LoginName": "owner@example.com"}},
            }
        login, system = peers[command[-1]]
        return {
            "UserProfile": {"LoginName": login},
            "Node": {"Hostinfo": {"OS": system}},
        }

    return runner


class AdmissionTests(unittest.TestCase):
    def test_only_the_owners_ios_devices_and_this_mac(self):
        auth = remote.TailnetAuth(
            runner=fake_tailscale(
                {
                    "100.64.0.2": ("owner@example.com", "iOS"),
                    "100.64.0.3": ("owner@example.com", "linux"),
                    "100.64.0.4": ("someone@example.com", "iOS"),
                }
            )
        )
        self.assertTrue(auth.allowed("100.64.0.1"))
        self.assertTrue(auth.allowed("100.64.0.2"))
        self.assertFalse(auth.allowed("100.64.0.3"))
        self.assertFalse(auth.allowed("100.64.0.4"))
        self.assertFalse(auth.allowed("127.0.0.1"))
        self.assertIn("mac.example.ts.net:7349", auth.hosts(7349))
        self.assertNotIn("localhost:7349", auth.hosts(7349))

    def test_browsers_and_unknown_hosts_are_refused(self):
        with Server(self, "{}") as server:
            for headers, expected in (
                ({}, 403),
                ({"Origin": "https://example.com", "X-Lapis-Client": "ios"}, 403),
                ({"X-Lapis-Client": "ios", "Host": "evil.example:1"}, 421),
                ({"X-Lapis-Client": "ios"}, 200),
            ):
                with self.subTest(headers=headers):
                    status, _ = server.request(
                        "GET", "/api/health", headers=headers, client=False
                    )
                    self.assertEqual(status, expected)


class Server:
    """A gateway on 127.0.0.1 with a local-only admission rule."""

    def __init__(self, test, registry_text, runtime=None):
        self.test = test
        self.directory = runtime or Path(tempfile.mkdtemp(prefix="lr-", dir="/tmp"))
        self.registry = self.directory / "workspace.json"
        self.registry.write_text(registry_text)

    def __enter__(self):
        auth = remote.TailnetAuth(allow_local=True, runner=fake_tailscale({}))
        remote.Handler.log_message = lambda *_: None
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), remote.Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        remote.Handler.gateway = remote.Gateway(self.registry, auth, self.port)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def __exit__(self, *_):
        self.httpd.shutdown()
        self.httpd.server_close()

    def connection(self, timeout=10):
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)

    def request(self, method, path, body=None, headers=None, client=True):
        connection = self.connection()
        headers = {**({"X-Lapis-Client": "ios"} if client else {}), **(headers or {})}
        connection.request(
            method,
            path,
            body=None if body is None else json.dumps(body),
            headers=headers,
        )
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, json.loads(data) if data else None


class Events:
    """Server-sent events from one stream request."""

    def __init__(self, server, path):
        self.connection = server.connection()
        self.connection.request("GET", path, headers={"X-Lapis-Client": "ios"})
        self.response = self.connection.getresponse()

    def next(self):
        name, data = None, None
        while True:
            line = self.response.fp.readline()
            if not line:
                return None, None
            line = line.decode().rstrip("\n")
            if line.startswith("event: "):
                name = line[7:]
            elif line.startswith("data: "):
                data = json.loads(line[6:])
            elif line == "" and name:
                return name, data

    def until(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            name, data = self.next()
            if name is None:
                break
            if predicate(name, data):
                return name, data
        raise AssertionError(f"stream condition not met within {timeout}s")

    def close(self):
        self.response.close()
        self.connection.close()


def screen_text(frame):
    return "\n".join("".join(run[0] for run in line) for line in frame["lines"])


@unittest.skipUnless(SERVICE.exists(), "needs the built session service")
class LiveServiceTests(unittest.TestCase):
    def setUp(self):
        self.runtime = Path(tempfile.mkdtemp(prefix="lr-", dir="/tmp"))
        self.addCleanup(shutil.rmtree, self.runtime, True)
        self.identifier = "11111111-2222-4333-8444-555555555555"
        self.endpoint = self.runtime / (self.identifier + ".sock")
        script = (
            'printf "ready\\n"; while read line; do printf "got:%s\\n" "$line"; done'
        )
        self.service = subprocess.Popen(
            [str(SERVICE), str(self.endpoint), "/", "/bin/sh", "-c", script],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            env={**os.environ, "LAPIS_HISTORY_ROOT": str(self.runtime / "history")},
        )
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 10
        while not remote.service_answers(str(self.endpoint)):
            self.assertLess(time.monotonic(), deadline, "service did not start")
            time.sleep(0.05)
        self.registry = json.dumps(
            {
                "categories": [{"id": "general", "name": "General"}],
                "activeCategory": "general",
                "agents": [
                    {
                        "id": self.identifier,
                        "title": "echo",
                        "category": "general",
                        "harness": "grok",
                        "endpoint": str(self.endpoint),
                        "program": "/bin/sh",
                        "arguments": ["-c", script],
                        "directory": "/",
                    }
                ],
            }
        )

    def stop(self):
        if self.service.poll() is None:
            os.killpg(self.service.pid, 15)
            self.service.wait(10)

    def test_list_stream_type_resize_and_replace(self):
        with Server(self, self.registry, self.runtime) as server:
            status, listing = server.request("GET", "/api/agents")
            self.assertEqual(status, 200)
            agent = listing["categories"][0]["agents"][0]
            self.assertEqual((agent["title"], agent["running"]), ("echo", True))

            path = f"/api/agents/{self.identifier}"
            events = Events(server, path + "/stream?columns=40&rows=12")
            _, frame = events.until(lambda name, data: name == "frame")
            events.until(
                lambda name, data: (
                    name == "frame"
                    and data["columns"] == 40
                    and "ready" in screen_text(data)
                )
            )
            status, _ = server.request(
                "POST", path + "/input", {"paste": "hello phone", "key": "enter"}
            )
            self.assertEqual(status, 200)
            events.until(
                lambda name, data: (
                    name == "frame" and "got:hello phone" in screen_text(data)
                )
            )
            status, _ = server.request("POST", path + "/input", {"resize": [60, 20]})
            self.assertEqual(status, 200)
            events.until(lambda name, data: name == "frame" and data["columns"] == 60)

            # A second view takes over; the first is told and input follows.
            second = Events(server, path + "/stream?columns=50&rows=10")
            second.until(lambda name, data: name == "frame")
            _, moved = events.until(lambda name, data: name == "status")
            self.assertEqual(moved["state"], "released")
            status, _ = server.request("POST", path + "/input", {"text": "again\r"})
            self.assertEqual(status, 200)
            second.until(
                lambda name, data: name == "frame" and "got:again" in screen_text(data)
            )
            events.close()
            second.close()
            deadline = time.monotonic() + 5
            while remote.Handler.gateway.session(self.identifier) is not None:
                self.assertLess(time.monotonic(), deadline, "session not released")
                time.sleep(0.05)
            status, _ = server.request("POST", path + "/input", {"text": "x"})
            self.assertEqual(status, 409)

    def test_the_desktop_attachment_is_replaced(self):
        with Server(self, self.registry, self.runtime) as server:
            agent = remote.load_workspace(server.registry)["agents"][0]
            desktop = remote.WireSession(agent)
            self.addCleanup(desktop.close)
            events = Events(server, f"/api/agents/{self.identifier}/stream")
            events.until(lambda name, data: name == "frame")
            deadline = time.monotonic() + 5
            replaced = None
            while replaced is None and time.monotonic() < deadline:
                received = desktop.receive(0.5)
                if received and received[0] == remote.STATUS:
                    replaced = remote.status_message(received[1])
            self.assertTrue(replaced and replaced.startswith("replaced"))
            events.close()


if __name__ == "__main__":
    unittest.main()
