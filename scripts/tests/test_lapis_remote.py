"""The iPhone gateway: snapshots, launch identity, admission and a live service."""

import base64
import gzip
import http.client
import itertools
import io
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps" / "remote"))
import lapis_remote as remote  # noqa: E402

SERVICE = ROOT / "build" / "desktop" / "services" / "session" / "lapis_session_service"
DESKTOP = next(
    (
        path
        for path in (
            ROOT
            / "build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop",
            ROOT / "build/desktop/apps/desktop/lapis_desktop",
        )
        if path.is_file()
    ),
    None,
)


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


class StreamFailureTests(unittest.TestCase):
    def test_malformed_live_snapshot_reports_disconnection(self):
        valid = snapshot(1, 1, [cell("x")])
        for broken in (
            snapshot(1, 1, [cell("x")], alternate=2),
            valid[:20],
            valid[:-1],
        ):
            with self.subTest(length=len(broken)):
                session = object.__new__(remote.WireSession)
                session.first = valid
                session.closed = False
                session.attachment = b"x" * remote.ATTACHMENT_BYTES
                session.sequence = 0
                session.wheel_accepted = False
                message = session.attachment + struct.pack(">QQQQ", 1, 0, 0, 0) + broken
                session.receive = Mock(
                    side_effect=[(remote.SNAPSHOT, message), EOFError()]
                )
                handler = object.__new__(remote.Handler)
                handler.wfile = io.BytesIO()
                handler.phone_left = lambda: False
                # Advance beyond the frame interval to exercise render validation too.
                with patch.object(
                    remote.time, "monotonic", side_effect=itertools.count()
                ):
                    handler.pump(session)
                events = handler.wfile.getvalue().decode().split("\n\n")
                self.assertEqual(
                    sum(event.startswith("event: frame\n") for event in events), 1
                )
                status = next(
                    event for event in events if event.startswith("event: status\n")
                )
                payload = json.loads(status.split("data: ", 1)[1])
                self.assertEqual(payload["state"], "disconnected")
                self.assertTrue(payload["message"])


class SnapshotTests(unittest.TestCase):
    def test_wheel_requires_the_alternate_screen_bit(self):
        for flags in (0, 1, 2, 3, 4):
            with self.subTest(flags=flags):
                payload = snapshot(1, 1, [cell("x")], alternate=flags)
                session = object.__new__(remote.WireSession)
                session.attachment = b"x" * remote.ATTACHMENT_BYTES
                session.sequence = 0
                session.wheel_accepted = False
                message = (
                    session.attachment + struct.pack(">QQQQ", 1, 0, 0, 0) + payload
                )
                if flags not in (0, 1, 3):
                    with self.assertRaises(remote.GatewayError):
                        remote.render_snapshot(payload)
                    with self.assertRaises(remote.GatewayError):
                        session.accept_snapshot(remote.SNAPSHOT, message)
                    self.assertEqual(session.sequence, 0)
                    self.assertFalse(session.wheel_accepted)
                else:
                    self.assertEqual(
                        remote.render_snapshot(payload)["wheel"], flags == 3
                    )
                    self.assertEqual(
                        session.accept_snapshot(remote.SNAPSHOT, message), payload
                    )
                    self.assertEqual(session.wheel_accepted, flags == 3)

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
        # Runs carry their column and width in cells; the wide cell spans two.
        self.assertEqual(
            frame["lines"][0],
            [
                ["a", None, None, 0, 0, 1],
                ["b", "#ff0000", None, 0, 1, 1],
                ["漢", None, None, 0, 2, 2],
            ],
        )
        # Inverse swaps in the defaults; the cursor cell is its own run.
        self.assertEqual(
            frame["lines"][1],
            [
                ["x", None, None, remote.BOLD, 0, 1],
                ["y", "#111111", "#dddddd", remote.CURSOR, 1, 1],
                [" ", None, None, 0, 2, 1],
                ["z", None, "#00ff00", remote.UNDERLINE, 3, 1],
            ],
        )
        self.assertEqual(frame["cursor"], {"x": 1, "y": 1, "visible": True})

    def test_cursor_on_a_wide_tail_keeps_the_two_cell_glyph(self):
        payload = snapshot(
            3,
            1,
            [cell("漢", kind=1), cell("", kind=2), cell("x")],
            cursor=(1, 0),
        )
        frame = remote.render_snapshot(payload)
        self.assertEqual(
            frame["lines"][0],
            [
                ["漢", None, None, 0, 0, 2],
                ["", None, None, remote.CURSOR, 1, 1],
                ["x", None, None, 0, 2, 1],
            ],
        )

    def test_history_pages_hide_the_cursor(self):
        payload = snapshot(2, 1, [cell("a"), cell("b")], cursor=(0, 0))
        self.assertEqual(
            remote.render_snapshot(payload)["lines"][0][0][3], remote.CURSOR
        )
        page = remote.render_snapshot(payload, show_cursor=False)
        self.assertEqual(page["lines"][0], [["ab", None, None, 0, 0, 2]])

    def test_malformed_snapshots_are_refused(self):
        payload = snapshot(2, 1, [cell("a"), cell("b")])
        for broken in (payload[:-1], payload[:100], payload + b"\0"):
            with self.assertRaises(remote.GatewayError):
                remote.render_snapshot(broken)

    def test_invalid_grapheme_points_are_refused(self):
        grapheme_offset = remote.POOL_OFFSET + 4
        surrogate = bytearray(snapshot(1, 1, [cell("\ud800")]))
        distant = bytearray(snapshot(1, 1, [cell("a")]))
        struct.pack_into(">I", distant, grapheme_offset, 0x110000)
        for payload in (surrogate, distant):
            with self.subTest(
                point=struct.unpack_from(">I", payload, grapheme_offset)[0]
            ):
                with self.assertRaises(remote.GatewayError):
                    remote.render_snapshot(payload)


class HistoryReplyTests(unittest.TestCase):
    def test_bounded_reply_and_truncated_place(self):
        attachment = b"s" * remote.ATTACHMENT_BYTES
        header = attachment + struct.pack(">QQI", 1, 4, 0)
        screen = snapshot(2, 1, [cell("o"), cell("k")])
        request_id, reply = remote.decode_history_reply(header + screen, attachment)
        self.assertEqual((request_id, reply["page"], reply["snapshot"]), (1, 4, screen))
        for bad in (
            header[:47],
            header + b"x",
            header + bytes(60),
            header[:-4] + struct.pack(">I", 50),
        ):
            with self.subTest(size=len(bad)), self.assertRaises(remote.GatewayError):
                remote.decode_history_reply(bad, attachment)
        for length in (0, 36, 60):
            with (
                self.subTest(place_bytes=length),
                self.assertRaises(remote.GatewayError),
            ):
                remote.page_place(bytes(length))

        # Exercise waiter completion with an in-process transport. A bad page
        # fails its request promptly; the same session can then request a page.
        session = object.__new__(remote.WireSession)
        session.attachment = attachment
        session.lock = threading.Lock()
        session.history_ids = itertools.count(1)
        session.history_waiters = {}
        session.scrubbable = False

        def answer(payload):
            def send(kind, request):
                self.assertEqual(kind, remote.HISTORY_REQUEST)
                request_id = struct.unpack_from(">Q", request)[0]
                session.deliver_history(
                    attachment + struct.pack(">QQI", request_id, 4, 0) + payload
                )

            return send

        session.send = answer(b"short")
        with self.assertRaisesRegex(remote.GatewayError, "Truncated history snapshot"):
            session.request_history(timeout=0.1)
        session.send = answer(screen)
        self.assertEqual(session.request_history(timeout=0.1)["snapshot"], screen)


class ResizeTests(unittest.TestCase):
    def test_resize_refuses_the_service_cell_limit(self):
        sent = []

        class Session:
            def send(self, kind, payload):
                sent.append((kind, payload))

        with self.assertRaises(remote.GatewayError):
            remote.WireSession.resize(Session(), 182, 181)
        remote.WireSession.resize(Session(), 500, 65)
        self.assertEqual(len(sent), 1)


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


class PlaceTests(unittest.TestCase):
    def test_places_read_like_paths(self):
        home = str(Path.home())

        def place(program, arguments, directory="/tmp"):
            return remote.display_place(
                {"program": program, "arguments": arguments, "directory": directory}
            )

        self.assertEqual(place("/bin/codex", [], home + "/dev/x"), ("", "~/dev/x"))
        self.assertEqual(place("/bin/codex", [], "/opt/x"), ("", "/opt/x"))
        self.assertEqual(
            place(
                "/usr/bin/ssh", ["-t", "-p", "22", "me@devbox", "cd ~/lapis && codex"]
            ),
            ("devbox", "devbox:~/lapis"),
        )
        self.assertEqual(place("/usr/bin/ssh", ["gpubox"]), ("gpubox", "gpubox:"))


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


def fake_zerotier(networks):
    """A runner answering `zerotier-cli -j listnetworks` with these networks."""

    def runner(command):
        return networks

    return runner


def missing_cli(command):
    raise FileNotFoundError(2, "No such file or directory", command[0])


# The shape this Mac's ZeroTier network has in `zerotier-cli -j listnetworks`,
# with the identifiers sanitized.
ZT_NETWORK = {
    "id": "7d319a9c02e4f5b6",
    "name": "VPN",
    "status": "OK",
    "type": "PRIVATE",
    "assignedAddresses": ["10.243.203.187/16"],
    "routes": [{"flags": 0, "metric": 0, "target": "10.243.0.0/16", "via": None}],
}


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

    def test_android_devices_of_the_owner(self):
        auth = remote.TailnetAuth(
            runner=fake_tailscale(
                {
                    "100.64.0.5": ("owner@example.com", "android"),
                    "100.64.0.6": ("owner@example.com", "Android"),
                }
            )
        )
        self.assertTrue(auth.allowed("100.64.0.5"))
        self.assertTrue(auth.allowed("100.64.0.6"))
        # Other systems and other owners' Android devices are still refused.
        auth = remote.TailnetAuth(
            runner=fake_tailscale(
                {
                    "100.64.0.3": ("owner@example.com", "linux"),
                    "100.64.0.4": ("someone@example.com", "android"),
                }
            )
        )
        self.assertFalse(auth.allowed("100.64.0.3"))
        self.assertFalse(auth.allowed("100.64.0.4"))

    def test_zerotier_members_and_this_mac(self):
        auth = remote.ZeroTierAuth(runner=fake_zerotier([ZT_NETWORK]))
        self.assertTrue(auth.allowed("10.243.9.7"))  # a member inside the route
        self.assertTrue(auth.allowed("10.243.203.187"))  # this Mac's address
        self.assertFalse(auth.allowed("10.244.0.1"))  # outside every route
        self.assertFalse(auth.allowed("never-an-address"))
        ipv6 = {
            **ZT_NETWORK,
            "assignedAddresses": ["fd5b:1a3c:9e22::99/64"],
            "routes": [
                {"flags": 0, "metric": 0, "target": "fd5b:1a3c:9e22::/64", "via": None}
            ],
        }
        auth = remote.ZeroTierAuth(runner=fake_zerotier([ipv6]))
        self.assertTrue(auth.allowed("fd5b:1a3c:9e22::1"))

    def test_public_or_unjoined_networks_admit_nobody(self):
        for network in (
            {**ZT_NETWORK, "type": "PUBLIC"},
            {**ZT_NETWORK, "status": "REQUESTING_CONFIGURATION"},
        ):
            with self.subTest(status=network["status"], type=network["type"]):
                auth = remote.ZeroTierAuth(runner=fake_zerotier([network]))
                self.assertFalse(auth.allowed("10.243.9.7"))
                self.assertEqual(auth.hosts(7349), set())

    def test_hosts_carry_the_assigned_addresses(self):
        auth = remote.ZeroTierAuth(runner=fake_zerotier([ZT_NETWORK]))
        self.assertIn("10.243.203.187:7349", auth.hosts(7349))
        ipv6 = {**ZT_NETWORK, "assignedAddresses": ["fd5b:1a3c:9e22::99/64"]}
        auth = remote.ZeroTierAuth(runner=fake_zerotier([ipv6]))
        self.assertIn("[fd5b:1a3c:9e22::99]:7349", auth.hosts(7349))

    def test_a_silent_zerotier_admits_nobody(self):
        auth = remote.ZeroTierAuth(runner=missing_cli)
        self.assertEqual(auth.current(), [])
        self.assertFalse(auth.allowed("10.243.9.7"))
        self.assertEqual(auth.hosts(7349), set())

    def test_the_first_question_fetches_even_near_a_zero_clock(self):
        # time.monotonic() has no defined origin; right after a boot it can
        # be small, and the cache must still fetch instead of answering the
        # never-built state as an empty list.
        calls = []

        def runner(command):
            calls.append(command)
            return [ZT_NETWORK]

        auth = remote.ZeroTierAuth(runner=runner, clock=lambda: 5.0)
        self.assertTrue(auth.allowed("10.243.9.7"))
        self.assertEqual(len(calls), 1)

    def test_a_fresh_list_then_silence_is_trusted_only_a_while(self):
        class Clock:  # a movable monotonic clock
            now = 1000.0

        clock = Clock()
        state = {"answered": True}

        def runner(command):
            if state["answered"]:
                return [ZT_NETWORK]
            raise FileNotFoundError(2, "No such file or directory", command[0])

        auth = remote.ZeroTierAuth(runner=runner, clock=lambda: clock.now)
        self.assertTrue(auth.allowed("10.243.9.7"))
        state["answered"] = False
        clock.now += remote.ZeroTierAuth.REFRESH + 1  # retried, silent, still fresh
        self.assertTrue(auth.allowed("10.243.9.7"))
        clock.now += remote.ZeroTierAuth.STALE + 1  # silent past the trust window
        self.assertFalse(auth.allowed("10.243.9.7"))
        self.assertEqual(auth.hosts(7349), set())
        state["answered"] = True
        clock.now += remote.ZeroTierAuth.REFRESH + 1  # recovered
        self.assertTrue(auth.allowed("10.243.9.7"))

    def test_one_fetch_at_a_time(self):
        started = threading.Semaphore(0)
        release = threading.Semaphore(0)
        calls = []
        gate = threading.Lock()

        def runner(command):
            with gate:
                calls.append(command)
            started.release()
            release.acquire(timeout=10)
            return [ZT_NETWORK]

        auth = remote.ZeroTierAuth(runner=runner)
        answers = []

        def ask():
            answers.append(auth.allowed("10.243.9.7"))

        threads = [threading.Thread(target=ask) for _ in range(3)]
        for thread in threads:
            thread.start()
        self.assertTrue(started.acquire(timeout=10))  # the first fetch is in flight
        for _ in range(2):
            release.release()
        for thread in threads:
            thread.join(10)
        self.assertEqual(len(calls), 1)  # no second fetch, so no out-of-order one
        self.assertEqual(answers, [True, True, True])

    def test_either_overlay_admits(self):
        admission = remote.Admission(
            tailnet=remote.TailnetAuth(
                runner=fake_tailscale({"100.64.0.2": ("owner@example.com", "iOS")})
            ),
            zerotier=remote.ZeroTierAuth(runner=fake_zerotier([ZT_NETWORK])),
        )
        self.assertTrue(admission.allowed("100.64.0.2"))  # a tailnet device
        self.assertTrue(admission.allowed("10.243.9.7"))  # a ZeroTier member
        self.assertFalse(admission.allowed("10.244.0.1"))
        self.assertEqual(admission.dns_name, "mac.example.ts.net")
        hosts = admission.hosts(7349)
        self.assertIn("mac.example.ts.net:7349", hosts)
        self.assertIn("10.243.203.187:7349", hosts)

    def test_either_overlay_admits_a_request(self):
        auth = remote.Admission(
            tailnet=remote.TailnetAuth(allow_local=True, runner=fake_tailscale({})),
            zerotier=remote.ZeroTierAuth(runner=fake_zerotier([ZT_NETWORK])),
        )
        with Server(self, "{}", auth=auth) as server:
            for host, expected in (
                (f"10.243.203.187:{server.port}", 200),  # a ZeroTier address
                (f"mac.example.ts.net:{server.port}", 200),  # the tailnet name
                (f"10.244.0.1:{server.port}", 421),
            ):
                with self.subTest(host=host):
                    status, _ = server.request(
                        "GET", "/api/health", headers={"Host": host}
                    )
                    self.assertEqual(status, expected)

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


class StartupTests(unittest.TestCase):
    def test_zerotier_serves_alone_when_tailscale_is_absent(self):
        tailnet, zerotier = remote.build_admission(
            tailnet_runner=missing_cli, zerotier_runner=fake_zerotier([ZT_NETWORK])
        )
        self.assertIsNone(tailnet)
        self.assertTrue(zerotier.allowed("10.243.9.7"))

    def test_both_overlays_are_built_when_both_answer(self):
        tailnet, zerotier = remote.build_admission(
            tailnet_runner=fake_tailscale({}),
            zerotier_runner=fake_zerotier([ZT_NETWORK]),
        )
        self.assertEqual(tailnet.dns_name, "mac.example.ts.net")
        self.assertTrue(zerotier.allowed("10.243.9.7"))

    def test_without_any_overlay_the_error_names_both(self):
        with self.assertRaises(remote.GatewayError) as raised:
            remote.build_admission(
                tailnet_runner=missing_cli, zerotier_runner=missing_cli
            )
        self.assertIn("Tailscale", str(raised.exception))
        self.assertIn("ZeroTier", str(raised.exception))


class Server:
    """A gateway on 127.0.0.1 with a local-only admission rule."""

    def __init__(self, test, registry_text, runtime=None, auth=None, **gateway):
        self.test = test
        self.auth = auth
        self.gateway = gateway
        if runtime is None:
            self.directory = Path(tempfile.mkdtemp(prefix="lr-", dir="/tmp"))
            self.test.addCleanup(shutil.rmtree, self.directory, True)
        else:
            self.directory = runtime
        self.registry = self.directory / "workspace.json"
        self.registry.write_text(registry_text)

    def __enter__(self):
        if self.auth is None:
            self.auth = remote.TailnetAuth(allow_local=True, runner=fake_tailscale({}))
        remote.Handler.log_message = lambda *_: None
        self.httpd = remote.BoundedHTTPServer(("127.0.0.1", 0), remote.Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        remote.Handler.gateway = remote.Gateway(
            self.registry, self.auth, self.port, **self.gateway
        )
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


class ListingTests(unittest.TestCase):
    """The phone lists categories in the Mac's order, and a change there
    reaches a phone that waits on the version it has at once."""

    @staticmethod
    def registry(*names):
        return json.dumps(
            {
                "version": 2,
                "activeCategory": names[0],
                "categories": [{"id": name, "name": name} for name in names],
                "agents": [],
            }
        )

    def test_running_follows_the_agent_not_the_listing_order(self):
        """Probes answer in registry order while the listing groups by
        category; an agents array that interleaves categories must not let a
        running answer land on the neighbouring agent."""
        runtime = Path(tempfile.mkdtemp(prefix="lr-", dir="/tmp"))
        self.addCleanup(shutil.rmtree, runtime, True)
        registry = json.dumps(
            {
                "version": 2,
                "activeCategory": "one",
                "categories": [
                    {"id": "one", "name": "one"},
                    {"id": "two", "name": "two"},
                ],
                "agents": [
                    {
                        "id": "later",
                        "title": "later",
                        "category": "two",
                        "endpoint": str(runtime / "later.sock"),
                    },
                    {
                        "id": "first",
                        "title": "first",
                        "category": "one",
                        "endpoint": str(runtime / "first.sock"),
                    },
                ],
            }
        )

        def probes(targets, *, deadline):
            return [target.endswith("first.sock") for target in targets]

        real_probes = remote.run_bounded_probes
        remote.run_bounded_probes = probes
        self.addCleanup(setattr, remote, "run_bounded_probes", real_probes)
        with Server(self, registry, runtime=runtime) as server:
            status, listed = server.request("GET", "/api/agents")
            self.assertEqual(status, 200)
            running = {
                agent["id"]: agent["running"]
                for category in listed["categories"]
                for agent in category["agents"]
            }
            self.assertEqual(running, {"first": True, "later": False})

    def test_a_change_on_the_mac_answers_a_waiting_phone(self):
        wait = remote.LISTING_WAIT
        remote.LISTING_WAIT = 1.5
        self.addCleanup(setattr, remote, "LISTING_WAIT", wait)
        with Server(self, self.registry("Build", "Later", "Ideas")) as server:
            status, listed = server.request("GET", "/api/agents")
            self.assertEqual(status, 200)
            self.assertEqual(
                [category["name"] for category in listed["categories"]],
                ["Build", "Later", "Ideas"],
            )
            version = listed["version"]
            self.assertTrue(version)

            # Nothing changes: the gateway waits, then answers the same.
            began = time.monotonic()
            status, same = server.request("GET", f"/api/agents?after={version}")
            self.assertGreaterEqual(time.monotonic() - began, 1.3)
            self.assertEqual((status, same["version"]), (200, version))

            # The Mac replaces its registry, as QSaveFile does.
            def reorder():
                time.sleep(0.4)
                moved = server.registry.with_name("workspace.json.new")
                moved.write_text(self.registry("Later", "Ideas", "Build"))
                os.replace(moved, server.registry)

            mover = threading.Thread(target=reorder)
            began = time.monotonic()
            mover.start()
            status, changed = server.request("GET", f"/api/agents?after={version}")
            mover.join()
            self.assertLess(time.monotonic() - began, 1.3)
            self.assertEqual(status, 200)
            self.assertEqual(
                [category["name"] for category in changed["categories"]],
                ["Later", "Ideas", "Build"],
            )
            self.assertNotEqual(changed["version"], version)

            # A phone with an older version is answered at once.
            began = time.monotonic()
            status, _ = server.request("GET", f"/api/agents?after={version}")
            self.assertEqual(status, 200)
            self.assertLess(time.monotonic() - began, 1.0)


class CaptureTests(unittest.TestCase):
    def test_captures_are_saved_privately_beside_the_workspace(self):
        png = b"\x89PNG\r\n\x1a\n" + b"0" * 32
        with Server(self, "{}") as server:
            status, reply = server.request(
                "POST",
                "/api/captures",
                {"png": base64.b64encode(png).decode(), "frame": {"columns": 3}},
            )
            self.assertEqual(status, 200)
            saved = server.directory / "phone-captures" / (reply["saved"] + ".png")
            self.assertEqual(saved.read_bytes(), png)
            self.assertEqual(saved.stat().st_mode & 0o777, 0o600)
            status, _ = server.request("POST", "/api/captures", {"png": "bm90IGEgcG5n"})
            self.assertEqual(status, 400)

    def test_malformed_capture_bodies_are_rejected_without_object_methods(self):
        with Server(self, "{}") as server:
            for body in ([], "png", 12):
                with self.subTest(body=body):
                    status, _ = server.request("POST", "/api/captures", body)
                    self.assertEqual(status, 400)

    def test_rapid_captures_do_not_replace_each_other(self):
        png = b"\x89PNG\r\n\x1a\n" + b"1" * 32
        with (
            Server(self, "{}") as server,
            patch("time.strftime", return_value="20260925-000000"),
        ):
            replies = [
                server.request(
                    "POST",
                    "/api/captures",
                    {"png": base64.b64encode(png + bytes([number])).decode()},
                )
                for number in range(2)
            ]
            saved = [reply["saved"] for _, reply in replies]
            self.assertEqual(len(set(saved)), 2)
            for number, name in enumerate(saved):
                path = server.directory / "phone-captures" / (name + ".png")
                self.assertEqual(path.read_bytes(), png + bytes([number]))


class FakeDesktop:
    """A stand-in for the lapis window's workspace-control socket."""

    def __init__(self, directory, answer):
        self.path = Path(directory) / remote.CONTROL_NAME
        self.answer = answer
        self.requests = []

    def __enter__(self):
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(self.path))
        self.listener.listen()
        threading.Thread(target=self.serve, daemon=True).start()
        return self

    def serve(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            with connection:
                data = b""
                while not data.endswith(b"\n"):
                    chunk = connection.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                request = json.loads(data)
                self.requests.append(request)
                connection.sendall(json.dumps(self.answer(request)).encode() + b"\n")

    def __exit__(self, *_):
        self.listener.close()
        self.path.unlink(missing_ok=True)


class StartAgentTests(unittest.TestCase):
    def answer(self, request):
        if request["request"] == "harnesses":
            return {
                "ok": True,
                "harnesses": [{"id": "codex", "name": "Codex", "installed": True}],
            }
        if request["category"] == "gone":
            return {"ok": False, "error": "Unknown category."}
        return {"ok": True, "id": "new-agent", "updating": True}

    def test_the_phone_starts_an_agent_through_the_mac(self):
        with (
            Server(self, "{}") as server,
            FakeDesktop(server.directory, self.answer) as desktop,
        ):
            status, listed = server.request("GET", "/api/harnesses")
            self.assertEqual(status, 200)
            self.assertEqual(listed["harnesses"][0]["id"], "codex")
            status, started = server.request(
                "POST",
                "/api/agents",
                {
                    "harness": "codex",
                    "directory": "~/dev/x",
                    "category": "later",
                    "title": "x",
                },
            )
            self.assertEqual(
                (status, started), (200, {"id": "new-agent", "updating": True})
            )
            self.assertEqual(
                desktop.requests[-1],
                {
                    "version": 1,
                    "request": "createAgent",
                    "category": "later",
                    "harness": "codex",
                    "directory": "~/dev/x",
                    "title": "x",
                },
            )

            for title, expected in (
                ("", 200),
                ("x" * 80, 200),
                ("\N{FACE WITH OPEN MOUTH}" * 40, 200),
                ("\N{FACE WITH OPEN MOUTH}" * 41, 400),
                ("x" * 81, 400),
                ("\ud800", 400),
            ):
                with self.subTest(title=repr(title)):
                    status, _ = server.request(
                        "POST",
                        "/api/agents",
                        {
                            "harness": "codex",
                            "directory": "~/dev/x",
                            "category": "later",
                            "title": title,
                        },
                    )
                    self.assertEqual(status, expected)

    def test_refusals_and_malformed_requests(self):
        with (
            Server(self, "{}") as server,
            FakeDesktop(server.directory, self.answer) as desktop,
        ):
            status, refused = server.request(
                "POST",
                "/api/agents",
                {"harness": "codex", "directory": "/tmp", "category": "gone"},
            )
            self.assertEqual((status, refused), (422, {"error": "Unknown category."}))
            asked = len(desktop.requests)
            for body in (
                {"harness": "codex", "directory": "/tmp"},
                {"harness": "codex", "directory": "x" * 5000, "category": "later"},
                {
                    "harness": "codex",
                    "directory": "/tmp",
                    "category": "later",
                    "title": 1,
                },
                ["not", "an", "object"],
            ):
                status, _ = server.request("POST", "/api/agents", body)
                self.assertEqual(status, 400, body)
            status, _ = server.request(
                "POST",
                "/api/agents",
                {
                    "harness": "codex",
                    "directory": "~",
                    "category": "later",
                    "machine": "-oX",
                },
            )
            self.assertEqual(status, 400)
            self.assertEqual(len(desktop.requests), asked, "bad requests reach nothing")
            status, _ = server.request(
                "POST",
                "/api/agents",
                {
                    "harness": "codex",
                    "directory": "~/x",
                    "category": "later",
                    "machine": "devbox",
                    "model": "gpt-6-sol",
                    "mode": "full",
                },
            )
            self.assertEqual(status, 200)
            self.assertEqual(
                {k: desktop.requests[-1][k] for k in ("machine", "model", "mode")},
                {"machine": "devbox", "model": "gpt-6-sol", "mode": "full"},
            )

    def test_categories_and_closing_go_through_the_mac(self):
        def answer(request):
            if request["request"] == "createCategory":
                return {"ok": True, "id": "cat-1"}
            if request["request"] == "closeAgent" and request["id"] == "gone":
                return {"ok": False, "error": "No such agent"}
            return {"ok": True}

        with (
            Server(self, "{}") as server,
            FakeDesktop(server.directory, answer) as desktop,
        ):
            status, made = server.request(
                "POST", "/api/categories", {"name": " Later "}
            )
            self.assertEqual((status, made), (200, {"id": "cat-1"}))
            self.assertEqual(
                desktop.requests[-1],
                {"version": 1, "request": "createCategory", "name": "Later"},
            )
            asked = len(desktop.requests)
            for body in ({"name": ""}, {"name": "x" * 81}, {"name": 3}, ["x"]):
                status, _ = server.request("POST", "/api/categories", body)
                self.assertEqual(status, 400, body)
            self.assertEqual(len(desktop.requests), asked, "bad names reach nothing")
            status, closed = server.request("POST", "/api/agents/a1/close", {})
            self.assertEqual((status, closed), (200, {"ok": True}))
            self.assertEqual(
                desktop.requests[-1],
                {"version": 1, "request": "closeAgent", "id": "a1"},
            )
            status, refused = server.request("POST", "/api/agents/gone/close", {})
            self.assertEqual((status, refused), (422, {"error": "No such agent"}))
            # A name chosen on the phone, which the Mac keeps.
            status, named = server.request(
                "POST", "/api/agents/a1/rename", {"title": " Resize work "}
            )
            self.assertEqual((status, named), (200, {"ok": True}))
            self.assertEqual(
                desktop.requests[-1],
                {
                    "version": 1,
                    "request": "renameAgent",
                    "id": "a1",
                    "title": "Resize work",
                },
            )
            asked = len(desktop.requests)
            rejected = (
                {"title": ""},
                {"title": "x" * 81},
                {"title": "\N{FACE WITH OPEN MOUTH}" * 41},
                {"title": "x\n"},
                {"title": "\ud800"},
                {"title": 3},
                ["x"],
            )
            for body in rejected:
                status, _ = server.request("POST", "/api/agents/a1/rename", body)
                self.assertEqual(status, 400, body)
            self.assertEqual(len(desktop.requests), asked, "bad names reach nothing")
            for body in (
                {"title": "x" * 80},
                {"title": "\N{FACE WITH OPEN MOUTH}" * 40},
            ):
                status, _ = server.request("POST", "/api/agents/a1/rename", body)
                self.assertEqual(status, 200, body)
            self.assertEqual(len(desktop.requests), asked + 2)

    def test_the_phone_arranges_categories_and_agents_through_the_mac(self):
        def answer(request):
            if request["request"] == "removeCategory" and request["id"] == "full":
                return {"ok": False, "error": "Move the agents out first."}
            return {"ok": True}

        with (
            Server(self, "{}") as server,
            FakeDesktop(server.directory, answer) as desktop,
        ):
            for path, body, asked in (
                (
                    "/api/categories/c1/rename",
                    {"name": " Someday "},
                    {"request": "renameCategory", "id": "c1", "name": "Someday"},
                ),
                (
                    "/api/categories/c1/place",
                    {"index": 0},
                    {"request": "placeCategory", "id": "c1", "index": 0},
                ),
                (
                    "/api/categories/c1/remove",
                    None,
                    {"request": "removeCategory", "id": "c1"},
                ),
                (
                    "/api/agents/a1/place",
                    {"category": "c2", "index": 1},
                    {"request": "placeAgent", "id": "a1", "category": "c2", "index": 1},
                ),
                (
                    "/api/agents/a1/place",
                    {"category": "c2"},
                    {
                        "request": "placeAgent",
                        "id": "a1",
                        "category": "c2",
                        "index": 1 << 20,
                    },
                ),
                (
                    "/api/agents/a1/restart",
                    None,
                    {"request": "restartAgent", "id": "a1"},
                ),
            ):
                status, reply = server.request("POST", path, body)
                self.assertEqual((status, reply), (200, {"ok": True}), path)
                self.assertEqual(desktop.requests[-1], {"version": 1, **asked})
            status, refused = server.request("POST", "/api/categories/full/remove")
            self.assertEqual(
                (status, refused), (422, {"error": "Move the agents out first."})
            )
            asked = len(desktop.requests)
            for path, body in (
                ("/api/categories/c1/rename", {"name": ""}),
                ("/api/categories/c1/place", {"index": -1}),
                ("/api/categories/c1/place", {"index": True}),
                ("/api/categories/c1/place", {"index": "0"}),
                ("/api/agents/a1/place", {"index": 0}),
                ("/api/agents/a1/place", {"category": "c2", "index": 1.5}),
                ("/api/agents/a1/place", ["c2"]),
            ):
                status, _ = server.request("POST", path, body)
                self.assertEqual(status, 400, (path, body))
            self.assertEqual(len(desktop.requests), asked, "bad requests reach nothing")

    def test_the_phone_reads_and_changes_the_macs_settings(self):
        settings = {
            "keepAwake": True,
            "alertSound": True,
            "alertRepeat": 3,
            "finishSound": True,
            "notify": True,
            "showUsage": True,
        }

        def answer(request):
            settings.update(request.get("settings", {}))
            return {"ok": True, "settings": settings}

        with (
            Server(self, "{}") as server,
            FakeDesktop(server.directory, answer) as desktop,
        ):
            status, shown = server.request("GET", "/api/settings")
            self.assertEqual((status, shown), (200, {"settings": settings}))
            self.assertEqual(
                desktop.requests[-1], {"version": 1, "request": "settings"}
            )
            status, changed = server.request(
                "POST", "/api/settings", {"keepAwake": False, "alertRepeat": 5}
            )
            self.assertEqual(status, 200)
            self.assertFalse(changed["settings"]["keepAwake"])
            self.assertEqual(changed["settings"]["alertRepeat"], 5)
            self.assertEqual(
                desktop.requests[-1],
                {
                    "version": 1,
                    "request": "changeSettings",
                    "settings": {"keepAwake": False, "alertRepeat": 5},
                },
            )
            asked = len(desktop.requests)
            # The Mac's window appearance is not the phone's to change.
            for body in (
                {"theme": "amber"},
                {"keepAwake": "no"},
                {"keepAwake": 0},
                {"alertRepeat": 0},
                {"alertRepeat": 11},
                {"alertRepeat": True},
                ["keepAwake"],
            ):
                status, _ = server.request("POST", "/api/settings", body)
                self.assertEqual(status, 400, body)
            self.assertEqual(len(desktop.requests), asked, "bad settings reach nothing")

    def test_terminals_open_list_and_close_through_the_mac(self):
        def answer(request):
            if request["request"] == "openTerminal":
                if request["machine"] == "nowhere":
                    return {
                        "ok": False,
                        "error": "nowhere is not a host in your ssh config.",
                    }
                return {"ok": True, "id": "terminal-1"}
            return {"ok": True}

        with (
            Server(self, "{}") as server,
            FakeDesktop(server.directory, answer) as desktop,
        ):
            status, opened = server.request("POST", "/api/terminals", {"machine": ""})
            self.assertEqual((status, opened), (200, {"id": "terminal-1"}))
            self.assertEqual(
                desktop.requests[-1],
                {"version": 1, "request": "openTerminal", "machine": ""},
            )
            status, refused = server.request(
                "POST", "/api/terminals", {"machine": "nowhere"}
            )
            self.assertEqual(status, 422)
            asked = len(desktop.requests)
            for body in ({"machine": "-oProxyCommand=x"}, {"machine": 3}, ["x"]):
                status, _ = server.request("POST", "/api/terminals", body)
                self.assertEqual(status, 400, body)
            self.assertEqual(len(desktop.requests), asked, "bad machines reach nothing")
            # The desktop's terminals.json lists them; only its own endpoints count.
            folder = server.directory.resolve()
            (server.directory / "terminals.json").write_text(
                json.dumps(
                    {
                        "version": 1,
                        "terminals": [
                            {
                                "id": "terminal-1",
                                "machine": "",
                                "endpoint": str(folder / "terminal-1.sock"),
                                "program": "/bin/zsh",
                                "arguments": ["-l", "-i"],
                                "directory": "/tmp",
                            },
                            {
                                "id": "terminal-2",
                                "machine": "devbox",
                                "endpoint": "/elsewhere/terminal-2.sock",
                                "program": "/usr/bin/ssh",
                                "arguments": ["-t", "--", "devbox"],
                                "directory": "/tmp",
                            },
                        ],
                    }
                )
            )
            status, listed = server.request("GET", "/api/terminals")
            self.assertEqual(status, 200)
            self.assertEqual(
                listed["terminals"],
                [
                    {
                        "id": "terminal-1",
                        "machine": "",
                        "name": "This Mac",
                        "running": False,
                        "onPhone": False,
                    }
                ],
            )
            found = remote.Handler.gateway.agent("terminal-1")
            self.assertEqual((found["program"], found["mode"]), ("/bin/zsh", ""))
            self.assertIsNone(remote.Handler.gateway.agent("terminal-2"))
            status, closed = server.request("POST", "/api/agents/terminal-1/close", {})
            self.assertEqual((status, closed), (200, {"ok": True}))
            self.assertEqual(
                desktop.requests[-1],
                {"version": 1, "request": "closeTerminal", "id": "terminal-1"},
            )

    def test_without_lapis_on_the_mac(self):
        with Server(self, "{}") as server:
            status, reply = server.request(
                "POST",
                "/api/agents",
                {"harness": "codex", "directory": "/tmp", "category": "later"},
            )
            self.assertEqual(status, 503)
            self.assertIn("not running on the Mac", reply["error"])


class WindowlessHostCleanupTests(unittest.TestCase):
    def test_inventory_failure_still_reaps_the_owned_host(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            start_new_session=True,
        )
        failure = PermissionError("process inventory denied")
        fixture = SimpleNamespace(host=process, stop_services=Mock(side_effect=failure))
        try:
            with self.assertRaises(PermissionError) as raised:
                WindowlessHostTests.stop(fixture)
            self.assertIs(raised.exception, failure)
            self.assertIsNotNone(process.poll(), "owned host survived cleanup failure")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


@unittest.skipUnless(DESKTOP and SERVICE.is_file(), "desktop build required")
class WindowlessHostTests(unittest.TestCase):
    """The real windowless host (lapis_desktop --serve) behind the gateway."""

    def setUp(self):
        self.runtime = Path(tempfile.mkdtemp(prefix="lh-", dir="/tmp")).resolve()
        self.addCleanup(shutil.rmtree, self.runtime, True)
        (self.runtime / "bin").mkdir()
        (self.runtime / "project").mkdir()
        grok = self.runtime / "bin" / "grok"
        grok.write_text('#!/bin/sh\necho "grok ready in $(pwd)"\nexec sleep 600\n')
        grok.chmod(0o755)
        (self.runtime / "workspace.json").write_text(
            json.dumps(
                {
                    "version": 2,
                    "activeCategory": "general",
                    "categories": [
                        {"id": "general", "name": "General"},
                        {"id": "later", "name": "Later"},
                    ],
                    "agents": [],
                }
            )
        )
        environment = {
            **os.environ,
            "PATH": f"{self.runtime / 'bin'}:{os.environ.get('PATH', '')}",
            "LAPIS_HISTORY_ROOT": str(self.runtime / "history"),
        }
        self.host = subprocess.Popen(
            [
                str(DESKTOP),
                "--serve",
                "--registry",
                str(self.runtime / "workspace.json"),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            env=environment,
        )
        self.addCleanup(self.stop)
        control = str(self.runtime / remote.CONTROL_NAME)
        deadline = time.monotonic() + 15
        while not remote.service_answers(control):
            self.assertLess(time.monotonic(), deadline, "the host did not start")
            time.sleep(0.1)

    def stop(self):
        try:
            self.stop_services()
        finally:
            self.host.terminate()
            try:
                self.host.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.host.kill()
                self.host.wait(timeout=5)

    def stop_services(self):
        rows = subprocess.run(
            ["ps", "-axo", "pid=,command="], capture_output=True, text=True
        ).stdout.splitlines()
        for row in rows:
            if str(self.runtime) in row:
                try:
                    os.kill(int(row.split(None, 1)[0]), 9)
                except (ProcessLookupError, ValueError):
                    pass

    def test_the_host_starts_an_agent_in_its_category(self):
        with Server(
            self, (self.runtime / "workspace.json").read_text(), self.runtime
        ) as server:
            status, listed = server.request("GET", "/api/harnesses")
            self.assertEqual(status, 200)
            self.assertTrue(
                any(h["id"] == "grok" and h["installed"] for h in listed["harnesses"])
            )
            status, started = server.request(
                "POST",
                "/api/agents",
                {
                    "harness": "grok",
                    "directory": str(self.runtime / "project"),
                    "category": "later",
                },
            )
            self.assertEqual(status, 200, started)
            deadline = time.monotonic() + 15
            while True:
                status, listing = server.request("GET", "/api/agents")
                later = next(c for c in listing["categories"] if c["id"] == "later")
                agent = next(
                    (a for a in later["agents"] if a["id"] == started["id"]), None
                )
                if agent and agent["running"]:
                    break
                self.assertLess(time.monotonic(), deadline, "the new agent never ran")
                time.sleep(0.2)
            self.assertEqual(agent["title"], "project")
            status, refused = server.request(
                "POST",
                "/api/agents",
                {"harness": "grok", "directory": "/tmp", "category": "gone"},
            )
            self.assertEqual((status, refused["error"]), (422, "Unknown category."))


def mkdirs(root, *paths):
    for path in paths:
        (Path(root) / path).mkdir(parents=True, exist_ok=True)


def rollout(codex_home, name, meta):
    day = Path(codex_home) / "sessions" / "2026" / "09" / "24"
    day.mkdir(parents=True, exist_ok=True)
    (day / f"rollout-2026-09-24T12-00-00-{name}.jsonl").write_text(
        json.dumps({"type": "session_meta", "payload": meta}) + "\n{}\n"
    )


class FolderTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="lf-", dir="/tmp")).resolve()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.home = self.root / "home"
        mkdirs(
            self.home,
            "b",
            "a/node_modules/x",
            "a/App.app/Contents",
            "a/sub/deep/deeper/deepest",
            ".hidden/inner",
            "dev/lapis/.github/workflows",
        )
        (self.home / "link").symlink_to(self.home / "a")
        self.codex = self.root / "codex"
        self.claude = self.root / "claude"
        lapis = str(self.home / "dev" / "lapis")
        for index, meta in enumerate(
            [
                {"cwd": lapis, "source": "cli"},
                {"cwd": lapis, "source": "cli"},
                {"cwd": lapis, "source": {"subagent": {}}, "parent_thread_id": "x"},
                {"cwd": str(self.home / "b"), "source": "exec"},
            ]
        ):
            rollout(self.codex, f"01a0d4b2-0000-7000-8000-00000000000{index}", meta)
        project = self.claude / "projects" / "-home-b"
        project.mkdir(parents=True)
        for index in range(3):
            (project / f"0000000{index}-0000-4000-8000-000000000000.jsonl").write_text(
                json.dumps({"type": "summary"})
                + "\n"
                + json.dumps({"cwd": str(self.home / "b"), "entrypoint": "cli"})
            )
        (project / "agent-1234.jsonl").write_text(json.dumps({"cwd": "/elsewhere"}))
        # Automation (claude -p) is not an agent someone opened.
        (project / "10000000-0000-4000-8000-000000000000.jsonl").write_text(
            json.dumps({"cwd": str(self.home / "a"), "entrypoint": "sdk-cli"})
        )

    def test_what_is_listed_and_descended(self):
        folders = remote.scan_folders(str(self.home))
        for listed in (
            "a",
            "a/node_modules",
            "a/App.app",
            "a/sub/deep/deeper",
            ".hidden",
            ".hidden/inner",
            "dev/lapis/.github",
        ):
            self.assertIn(listed, folders)
        for skipped in (
            "a/node_modules/x",
            "a/App.app/Contents",
            "a/sub/deep/deeper/deepest",
            "dev/lapis/.github/workflows",
            "link",
        ):
            self.assertNotIn(skipped, folders)
        self.assertEqual(folders, sorted(folders))

    def test_folders_are_ranked_by_agents_started_there(self):
        registry = self.root / "workspace.json"
        registry.write_text(
            json.dumps(
                {
                    "agents": [
                        {
                            "id": "a1",
                            "endpoint": str(self.root / "a1.sock"),
                            "directory": str(self.home / "b"),
                            "program": "/bin/sh",
                        }
                    ]
                }
            )
        )
        counts = remote.AgentHistory(self.codex, self.claude).counts(registry)
        self.assertEqual(counts[str(self.home / "dev" / "lapis")], 2)
        self.assertEqual(
            counts[str(self.home / "b")], 4
        )  # 3 Claude sessions and 1 agent
        self.assertNotIn("/elsewhere", counts)
        self.assertNotIn(str(self.home / "a"), counts)

    def test_activity_and_conversations(self):
        # A named Codex thread and a Claude session with a typed first message.
        lapis = str(self.home / "dev" / "lapis")
        rollout(
            self.codex,
            "01a0d4b2-0000-7000-8000-0000000000aa",
            {
                "id": "01a0d4b2-0000-7000-8000-0000000000aa",
                "cwd": lapis,
                "source": "cli",
            },
        )
        (self.codex / "session_index.jsonl").write_text(
            json.dumps(
                {
                    "id": "01a0d4b2-0000-7000-8000-0000000000aa",
                    "thread_name": "Retry work",
                }
            )
            + "\n"
        )
        session = (
            self.claude
            / "projects"
            / "-home-b"
            / "20000000-0000-4000-8000-000000000000.jsonl"
        )
        session.write_text(
            json.dumps({"cwd": str(self.home / "b"), "entrypoint": "cli"})
            + "\n"
            + json.dumps({"type": "user", "message": {"content": "<command-name>/x"}})
            + "\n"
            + json.dumps({"type": "user", "message": {"content": "fix  the\nresize"}})
            + "\n"
        )
        index = remote.FolderIndex(
            self.root / "workspace.json",
            home=self.home,
            codex_home=self.codex,
            claude_home=self.claude,
        )
        with Server(self, "{}", folders=index) as server:
            status, body = server.request("GET", "/api/folders")
            self.assertEqual(status, 200)
            # Every conversation now counts about 1; b has four, dev/lapis three.
            self.assertGreater(body["activity"]["b"], body["activity"]["dev/lapis"])
            self.assertAlmostEqual(body["activity"]["dev/lapis"], 3, places=2)
            status, listed = server.request("GET", "/api/conversations")
            self.assertEqual(status, 200)
            titles = {item["id"]: item for item in listed["conversations"]}
            self.assertEqual(
                titles["01a0d4b2-0000-7000-8000-0000000000aa"]["title"], "Retry work"
            )
            self.assertEqual(
                titles["20000000-0000-4000-8000-000000000000"],
                {
                    "harness": "claude",
                    "id": "20000000-0000-4000-8000-000000000000",
                    "directory": "b",
                    "title": "fix the resize",
                    "age": titles["20000000-0000-4000-8000-000000000000"]["age"],
                },
            )
            # Rollouts without an id count for their folder but cannot resume.
            self.assertEqual(len(listed["conversations"]), 5)

    def test_history_ignores_malformed_records_and_keeps_a_good_title(self):
        codex_id = "30000000-0000-4000-8000-000000000000"
        codex = (
            self.codex
            / "sessions"
            / "2026"
            / "09"
            / "24"
            / f"rollout-2026-09-24T12-00-00-{codex_id}.jsonl"
        )
        codex.write_text(
            json.dumps(
                {
                    "type": "session_meta",
                    "payload": {
                        "id": codex_id,
                        "cwd": str(self.home / "b"),
                        "source": "cli",
                    },
                }
            )
            + "\nnull\n"
            + json.dumps(
                {
                    "type": "response_item",
                    "payload": {
                        "type": "message",
                        "role": "user",
                        "content": 42,
                    },
                }
            )
            + "\n"
            + json.dumps(
                {
                    "type": "response_item",
                    "payload": {
                        "type": "message",
                        "role": "user",
                        "content": [{"type": "text", "text": "typed rollout"}],
                    },
                }
            )
            + "\n"
        )
        project = self.claude / "projects" / "-home-b"
        project.mkdir(parents=True, exist_ok=True)
        session = project / "31000000-0000-4000-8000-000000000000.jsonl"
        session.write_text(
            json.dumps({"cwd": str(self.home / "b"), "entrypoint": "cli"})
            + "\nnull\n"
            + json.dumps({"type": "user", "message": "not an object"})
            + "\n"
            + json.dumps({"type": "ai-title", "aiTitle": "Generated title"})
            + "\n"
            + json.dumps({"type": "ai-title", "aiTitle": "   "})
            + "\n"
        )
        (self.codex / "session_index.jsonl").write_text(
            "[]\n"
            + json.dumps({"id": codex_id, "thread_name": "typed  rollout"})
            + "\n"
        )
        history = remote.AgentHistory(self.codex, self.claude)

        def titles():
            history.counts(None)
            return {item["id"]: item["title"] for item in history.conversations}

        conversations = titles()
        self.assertEqual(conversations[codex_id], "typed rollout")
        self.assertEqual(
            conversations["31000000-0000-4000-8000-000000000000"],
            "Generated title",
        )

        names_index = self.codex / "session_index.jsonl"
        names_index.write_text(
            "[]\n"
            + json.dumps({"id": codex_id, "thread_name": "# wrapper preserved"})
            + "\n"
        )
        self.assertEqual(
            titles()[codex_id],
            "# wrapper preserved",
            "a changed index is parsed and wrapper-like names are kept",
        )

        names_index.unlink()
        self.assertEqual(
            titles()[codex_id],
            "typed rollout",
            "deleting names clears the cached title",
        )

        names_index.write_text(
            "[]\n"
            + json.dumps({"id": codex_id, "thread_name": "# wrapper recreated"})
            + "\n"
        )
        self.assertEqual(titles()[codex_id], "# wrapper recreated")

        # Exercise the self-contained program actually sent over SSH, with
        # fixture homes. A compile-only check misses omitted helper definitions.
        result = subprocess.run(
            [sys.executable, "-c", remote.REMOTE_SCRIPT],
            env={
                **os.environ,
                "HOME": str(self.home),
                "CODEX_HOME": str(self.codex),
                "CLAUDE_CONFIG_DIR": str(self.claude),
            },
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
        self.assertTrue(result.stdout.startswith(remote.REMOTE_MARKER))
        report = json.loads(result.stdout[len(remote.REMOTE_MARKER) :])
        self.assertIn("b", report["activity"])

    def test_the_phone_downloads_the_index_once(self):
        index = remote.FolderIndex(
            self.root / "workspace.json",
            home=self.home,
            codex_home=self.codex,
            claude_home=self.claude,
        )
        with Server(self, "{}", folders=index) as server:
            status, body = server.request("GET", "/api/folders")
            self.assertEqual(status, 200)
            self.assertIn("dev/lapis", body["folders"])
            self.assertEqual(
                body["frequent"],
                [{"path": "b", "count": 3}, {"path": "dev/lapis", "count": 2}],
            )
            status, again = server.request(
                "GET", f"/api/folders?have={body['version']}"
            )
            self.assertEqual(again, {"version": body["version"], "unchanged": True})
            connection = server.connection()
            connection.request(
                "GET",
                "/api/folders",
                headers={"X-Lapis-Client": "ios", "Accept-Encoding": "gzip"},
            )
            response = connection.getresponse()
            self.assertEqual(response.getheader("Content-Encoding"), "gzip")
            self.assertEqual(json.loads(gzip.decompress(response.read())), body)
            # The connection stays open for the phone's next request.
            connection.request("GET", "/api/health", headers={"X-Lapis-Client": "ios"})
            self.assertEqual(connection.getresponse().status, 200)
            connection.close()


class HomeTests(unittest.TestCase):
    def test_the_gateway_serves_the_workspace_lapis_uses(self):
        root = Path(tempfile.mkdtemp(prefix="lh-", dir="/tmp")).resolve()
        self.addCleanup(shutil.rmtree, root, True)
        # A developer build keeps its workspace in the checkout.
        self.assertEqual(remote.lapis_home({}, root), remote.ROOT)
        # The downloaded app's ~/.lapis wins once it has a workspace.
        (root / ".lapis" / "runtime").mkdir(parents=True)
        self.assertEqual(remote.lapis_home({}, root), remote.ROOT)
        (root / ".lapis" / "runtime" / "workspace.json").write_text("{}")
        self.assertEqual(remote.lapis_home({}, root), root / ".lapis")
        # LAPIS_HOME, as the desktop reads it, wins over both.
        self.assertEqual(
            remote.lapis_home({"LAPIS_HOME": str(root / "x")}, root), root / "x"
        )


class MachineTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="lm-", dir="/tmp")).resolve()
        self.addCleanup(shutil.rmtree, self.root, True)

    def test_hosts_from_config_and_history(self):
        included = self.root / "extra config"
        included.write_text("Host gpubox\n  HostName 10.0.0.2\n")
        config = self.root / "config"
        config.write_text(
            f'Include "{included}"\nHost devbox devbox-lan\nHost *\nHost i-* mi-*\n# Host commented\n'
        )
        self.assertEqual(
            remote.ssh_config_hosts(config), ["gpubox", "devbox", "devbox-lan"]
        )
        history = self.root / "zsh_history"
        history.write_text(
            ": 1700000000:0;ssh devbox\n"
            ": 1700000001:0;ssh -p 22 me@gpubox 'ls'\n"
            "mosh devbox\n"
            "ssh -J jump devbox uptime\n"
            "echo ssh nobody\n" + "ssh -o ProxyCommand=x cloud\n" * 3
        )
        counts = remote.history_hosts([history])
        self.assertEqual(counts["devbox"], 3)
        self.assertEqual(counts["gpubox"], 1)
        self.assertEqual(counts["cloud"], 3)
        self.assertNotIn("nobody", counts)

    def test_machines_are_ordered_by_availability_then_use(self):
        config = self.root / "config"
        config.write_text("Host devbox gpubox oldbox\n")
        history = self.root / "history"
        history.write_text(
            "ssh oldbox\n" * 9
            + "ssh devbox\n" * 5
            + "ssh gpubox\n"
            + "ssh gone\n" * 4
            + "ssh live\n" * 3
        )
        original = remote.reachable
        remote.reachable = lambda name, timeout=1.5: (
            name in ("devbox", "gpubox", "live")
        )
        self.addCleanup(setattr, remote, "reachable", original)
        machines = remote.MachineList(self.root / "none.json", config, [history])
        machines.build()
        self.assertEqual(
            [(m["name"], m["available"]) for m in machines.machines],
            [("devbox", True), ("live", True), ("gpubox", True), ("oldbox", False)],
        )


@unittest.skipUnless(shutil.which("caffeinate"), "macOS only")
class KeepAwakeTests(unittest.TestCase):
    def test_the_mac_stays_awake_only_while_the_setting_is_on(self):
        directory = Path(tempfile.mkdtemp(prefix="lk-", dir="/tmp"))
        self.addCleanup(shutil.rmtree, directory, True)
        config = directory / "lapis.json"
        config.write_text("{}")
        keeper = remote.KeepAwake(config)
        self.addCleanup(lambda: keeper.process and keeper.process.terminate())
        keeper.apply()
        self.assertIsNotNone(keeper.process, "on by default")
        self.assertIsNone(keeper.process.poll())
        self.assertIn("-s", keeper.process.args)
        config.write_text('{"keepAwake": false}')
        keeper.apply()
        self.assertIsNone(keeper.process, "off when the config says so")


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
def live_agent(test, program, arguments):
    """A real session service running `program`, and a registry naming it."""
    test.runtime = Path(tempfile.mkdtemp(prefix="lr-", dir="/tmp"))
    test.addCleanup(shutil.rmtree, test.runtime, True)
    test.identifier = "11111111-2222-4333-8444-555555555555"
    test.endpoint = test.runtime / (test.identifier + ".sock")
    service = subprocess.Popen(
        [str(SERVICE), str(test.endpoint), "/", program, *arguments],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
        env={**os.environ, "LAPIS_HISTORY_ROOT": str(test.runtime / "history")},
    )

    def stop():
        if service.poll() is None:
            os.killpg(service.pid, 15)
            service.wait(10)

    test.addCleanup(stop)
    deadline = time.monotonic() + 10
    while not remote.service_answers(str(test.endpoint)):
        test.assertLess(time.monotonic(), deadline, "service did not start")
        time.sleep(0.05)
    test.registry = json.dumps(
        {
            "categories": [{"id": "general", "name": "General"}],
            "activeCategory": "general",
            "agents": [
                {
                    "id": test.identifier,
                    "title": "echo",
                    "category": "general",
                    "harness": "grok",
                    "endpoint": str(test.endpoint),
                    "program": program,
                    "arguments": arguments,
                    "directory": "/",
                }
            ],
        }
    )


class LiveServiceTests(unittest.TestCase):
    def setUp(self):
        script = (
            'printf "ready\\n"; while read line; do printf "got:%s\\n" "$line"; done'
        )
        live_agent(self, "/bin/sh", ["-c", script])

    def test_a_screen_is_read_without_resizing_or_taking_the_agent(self):
        with Server(self, self.registry, self.runtime) as server:
            path = f"/api/agents/{self.identifier}/screen"
            status, first = server.request("GET", path)
            self.assertEqual(status, 200)
            self.assertIn("ready", screen_text(first))
            status, second = server.request("GET", path)
            self.assertEqual(
                (first["columns"], first["rows"]), (second["columns"], second["rows"])
            )
            status, listing = server.request("GET", "/api/agents")
            self.assertFalse(listing["categories"][0]["agents"][0]["onPhone"])

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

    def desktop_sees(self, desktop, text, timeout=10):
        """Read the Mac-side client's screens until one shows the text."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            received = desktop.receive(0.5)
            if received is None:
                continue
            kind, data = received
            self.assertNotEqual(kind, remote.STATUS, remote.status_message(data))
            snapshot = desktop.accept_snapshot(kind, data)
            if snapshot is not None:
                frame = remote.render_snapshot(snapshot)
                if text in screen_text(frame):
                    return frame
        self.fail(f"the Mac never showed {text}")

    def test_the_phone_scrolls_back_through_history(self):
        with Server(self, self.registry, self.runtime) as server:
            path = f"/api/agents/{self.identifier}"
            phone = Events(server, path + "/stream?columns=40&rows=8")
            phone.until(lambda name, data: name == "frame")
            lines = "".join(f"line{index}\r" for index in range(1, 41))
            server.request("POST", path + "/input", {"text": lines})
            phone.until(
                lambda name, data: name == "frame" and "got:line40" in screen_text(data)
            )
            seen, before = [], 0
            for _ in range(200):
                status, page = server.request("GET", path + f"/history?before={before}")
                self.assertEqual(status, 200)
                if page["busy"]:
                    time.sleep(0.1)
                    continue
                if page["end"]:
                    break
                seen.insert(
                    0, "\n".join("".join(r[0] for r in line) for line in page["lines"])
                )
                before = page["page"]
            rows = [row.rstrip() for row in "\n".join(seen).splitlines()]
            self.assertEqual(rows[0], "ready")
            # Echo of the typed-ahead lines can share a row with the first reply.
            self.assertTrue(any(row.endswith("got:line1") for row in rows), rows)
            # Newer pages follow on from a loaded page, for new output.
            status, newer = server.request("GET", path + f"/history?after={before}")
            self.assertEqual(status, 200)
            self.assertTrue(newer["page"] > before or newer["end"] or newer["busy"])
            phone.close()

    def test_mac_and_phone_stay_in_sync(self):
        with Server(self, self.registry, self.runtime) as server:
            agent = remote.load_workspace(server.registry)["agents"][0]
            # The desktop attaches the way lapis does and asks for its size.
            desktop = remote.WireSession(agent, 90, 30, mode=remote.DISCOVER)
            self.addCleanup(desktop.close)
            path = f"/api/agents/{self.identifier}"
            phone = Events(server, path + "/stream?columns=40&rows=12")
            _, attached = phone.until(lambda name, data: name == "attached")
            self.assertTrue(attached["shared"])

            # Typing on the phone reaches the Mac, at the phone's size.
            server.request(
                "POST", path + "/input", {"paste": "from phone", "key": "enter"}
            )
            frame = self.desktop_sees(desktop, "got:from phone")
            self.assertEqual(frame["columns"], 40)

            # Typing on the Mac reaches the phone, at the Mac's size.
            desktop.text(b"from mac\r")
            phone.until(
                lambda name, data: (
                    name == "frame"
                    and data["columns"] == 90
                    and "got:from mac" in screen_text(data)
                )
            )

            # The desktop reattaching (a restart) leaves the phone attached.
            desktop.close()
            again = remote.WireSession(agent, 90, 30, mode=remote.DISCOVER)
            self.addCleanup(again.close)
            server.request("POST", path + "/input", {"text": "still here\r"})
            phone.until(
                lambda name, data: (
                    name == "frame" and "got:still here" in screen_text(data)
                )
            )
            self.desktop_sees(again, "got:still here")
            phone.close()


@unittest.skipUnless(SERVICE.exists(), "needs the built session service")
class WheelTests(unittest.TestCase):
    """A full-screen program that reports the mouse, as Claude Code's
    full-screen mode does, gets the phone's wheel as mouse wheel events."""

    def setUp(self):
        live_agent(self, sys.executable, [str(ROOT / "tools" / "qa" / "fake_agent.py")])

    def test_the_wheel_reaches_a_full_screen_program(self):
        with Server(self, self.registry, self.runtime) as server:
            path = f"/api/agents/{self.identifier}"
            events = Events(server, path + "/stream?columns=40&rows=12")
            _, first = events.until(
                lambda name, data: (
                    name == "frame" and "lapis fake agent" in screen_text(data)
                )
            )
            self.assertFalse(first["wheel"])
            # The primary screen pages history instead; the wheel is refused.
            status, _ = server.request("POST", path + "/input", {"wheel": [1, 0, 0]})
            self.assertEqual(status, 400)
            server.request("POST", path + "/input", {"text": "mouse\r"})
            _, shown = events.until(
                lambda name, data: (
                    name == "frame" and "mouse ready" in screen_text(data)
                )
            )
            self.assertTrue(shown["alternateScreen"] and shown["wheel"])
            for bad in ([0, 1, 1], [1, -1, 0], [True, 1, 1], [1, 1]):
                status, _ = server.request("POST", path + "/input", {"wheel": bad})
                self.assertEqual(status, 400, bad)
            status, _ = server.request("POST", path + "/input", {"wheel": [2, 4, 2]})
            self.assertEqual(status, 200)
            events.until(
                lambda name, data: (
                    name == "frame"
                    and "mouse got 2 events, first ESC[<64;5;3M" in screen_text(data)
                )
            )
            events.close()


@unittest.skipUnless(SERVICE.exists(), "needs the built session service")
class HistoryJumpTests(unittest.TestCase):
    """History keeps everything, and the phone jumps to any row of it."""

    def setUp(self):
        script = (
            "i=0; while [ $i -lt 2000 ]; do printf 'line %04d\\n' $i; i=$((i + 1));"
            " done; echo all printed; exec sleep 600"
        )
        live_agent(self, "/bin/sh", ["-c", script])

    def test_the_phone_jumps_to_the_first_row(self):
        with Server(self, self.registry, self.runtime) as server:
            path = f"/api/agents/{self.identifier}"
            events = Events(server, path + "/stream?columns=40&rows=12")
            events.until(
                lambda name, data: (
                    name == "frame" and "all printed" in screen_text(data)
                )
            )
            # A jump needs to know the service can; the newest page says so.
            status, _ = server.request("GET", path + "/history?at=0")
            self.assertEqual(status, 502)
            status, newest = server.request("GET", path + "/history?before=0")
            self.assertEqual(status, 200)
            place = newest["place"]
            self.assertTrue(place["scrubbable"])
            self.assertGreaterEqual(place["total"], 1980)
            self.assertEqual(place["offset"] + place["rows"], place["total"])
            status, start = server.request("GET", path + "/history?at=0")
            self.assertEqual(status, 200)
            self.assertEqual(start["place"]["offset"], 0)
            self.assertIn(
                "line 0000",
                "\n".join(run[0] for line in start["lines"] for run in line),
            )
            status, middle = server.request(
                "GET", path + f"/history?at={place['total'] // 2}"
            )
            self.assertEqual(status, 200)
            offset = middle["place"]["offset"]
            self.assertLessEqual(offset, place["total"] // 2)
            self.assertGreater(offset + middle["place"]["rows"], place["total"] // 2)
            events.close()


class OldServiceTests(unittest.TestCase):
    """A service started before joining existed rejects it; the phone takes over."""

    def test_the_phone_takes_over_an_old_service(self):
        runtime = Path(tempfile.mkdtemp(prefix="lr-", dir="/tmp"))
        self.addCleanup(shutil.rmtree, runtime, True)
        endpoint = runtime / "old.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(endpoint))
        listener.listen(2)
        self.addCleanup(listener.close)
        modes = []

        def serve():
            for _ in range(2):
                connection, _ = listener.accept()
                request = connection.recv(74)
                modes.append(request[5 + 4 + 32])
                if modes[-1] != remote.DISCOVER:
                    message = b"Invalid attachment request"
                    connection.sendall(
                        remote.frame(remote.STATUS, bytes([1]) + message)
                    )
                    connection.close()
                    continue
                attachment = b"S" * 16 + b"E" * 16 + struct.pack(">Q", 1)
                connection.sendall(
                    remote.frame(
                        remote.HELLO,
                        struct.pack(">I", remote.WIRE_VERSION)
                        + attachment
                        + struct.pack(">Q", 9),
                    )
                )
                screen = snapshot(2, 1, [cell("o"), cell("k")])
                envelope = attachment + struct.pack(">Q", 1) + bytes(24)
                connection.sendall(remote.frame(remote.SNAPSHOT, envelope + screen))
                connection.recv(64)  # ready
                self.addCleanup(connection.close)

        threading.Thread(target=serve, daemon=True).start()
        agent = {
            "endpoint": str(endpoint),
            "program": "/bin/sh",
            "arguments": [],
            "directory": "/",
            "mode": "",
        }
        session, shared = remote.open_session(agent)
        self.addCleanup(session.close)
        self.assertFalse(shared)
        self.assertEqual(modes, [remote.JOIN, remote.DISCOVER])
        self.assertEqual(screen_text(remote.render_snapshot(session.first)), "ok")


if __name__ == "__main__":
    unittest.main()
