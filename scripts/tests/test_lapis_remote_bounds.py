"""Bounded remote-machine discovery, HTTP work, and Tailnet identity admission."""

import http.client
import json
import shutil
import socket
import tempfile
import time
import subprocess
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps" / "remote"))
import lapis_remote as remote  # noqa: E402


class FakeClock:
    def __init__(self, now=1000.0):
        self.now = now

    def __call__(self):
        return self.now


class ServiceProbeBoundsTests(unittest.TestCase):
    def test_a_full_batch_probes_real_services_concurrently(self):
        folder = Path(tempfile.mkdtemp(prefix="lapis-probes-"))
        self.addCleanup(shutil.rmtree, folder, True)
        live = folder / "live.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(live))
        listener.listen()
        self.addCleanup(listener.close)
        targets = [str(live)] + [
            str(folder / f"dead-{number}.sock") for number in range(16)
        ]
        original = remote.service_answers
        release = threading.Event()
        calls = []

        def slow_service_answers(endpoint, timeout=0.3):
            calls.append(endpoint)
            if len(calls) == remote.MAX_LISTING_WORKERS:
                release.set()
            release.wait(5)
            return original(endpoint, timeout)

        with patch.object(remote, "service_answers", slow_service_answers):
            began = time.monotonic()
            answers = remote.run_bounded_probes(targets, deadline=time.monotonic() + 5)
            elapsed = time.monotonic() - began

        self.assertEqual(answers, [True] + [False] * 16)
        self.assertEqual(set(calls), set(targets))
        # Sixteen workers make this two rounds; serial calls would need 0.8s.
        self.assertLess(elapsed, 0.55)

    def test_an_oversized_batch_fails_without_starting_any_probe(self):
        targets = [f"/tmp/not-a-service-{number}.sock" for number in range(65)]
        calls = []

        def observe(endpoint, timeout=0.3):
            calls.append(endpoint)
            return remote.service_answers(endpoint, timeout)

        with patch.object(remote, "service_answers", observe):
            self.assertIsNone(
                remote.run_bounded_probes(targets, deadline=time.monotonic() + 5)
            )
        self.assertEqual(calls, [])

    def test_a_slow_service_probe_returns_transient_not_stopped(self):
        folder = Path(tempfile.mkdtemp(prefix="lapis-slow-probe-"))
        self.addCleanup(shutil.rmtree, folder, True)
        target = folder / "slow.sock"
        original = remote.service_answers

        def slow_service_answers(endpoint, timeout=0.3):
            threading.Event().wait(timeout)
            return original(endpoint, timeout)

        with patch.object(remote, "service_answers", slow_service_answers):
            self.assertIsNone(
                remote.run_bounded_probes(
                    [str(target)], deadline=time.monotonic() + 0.02
                )
            )


class GatewayHTTPBoundsTests(unittest.TestCase):
    def test_dead_and_live_services_produce_a_complete_listing(self):
        with LiveGateway() as server:
            directory = server.directory
            agents = []
            for number in range(32):
                identifier = f"agent-{number}"
                agents.append(
                    {
                        "id": identifier,
                        "title": identifier,
                        "category": "work",
                        "harness": "codex",
                        "directory": str(directory),
                        "program": "codex",
                        "arguments": [],
                        "endpoint": str(directory / f"{identifier}.sock"),
                    }
                )
            server.registry.write_text(
                json.dumps(
                    {
                        "activeCategory": "work",
                        "categories": [{"id": "work", "name": "Work"}],
                        "agents": agents,
                    }
                )
            )
            live = directory / "agent-0.sock"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(live))
            listener.listen()
            self.addCleanup(listener.close)

            began = time.monotonic()
            status, listed = server.request("GET", "/api/agents")

            self.assertEqual(status, 200)
            self.assertEqual(len(listed["categories"][0]["agents"]), 32)
            self.assertEqual(
                [item["running"] for item in listed["categories"][0]["agents"]],
                [True] + [False] * 31,
            )
            self.assertLess(time.monotonic() - began, 2)

    def test_a_late_service_probe_is_an_explicit_transient_failure(self):
        with LiveGateway() as server:
            original = remote.service_answers

            def slow_service_answers(endpoint, timeout=0.3):
                threading.Event().wait(timeout)
                return original(endpoint, timeout)

            with (
                patch.object(remote, "LISTING_DEADLINE", 0.08),
                patch.object(remote, "service_answers", slow_service_answers),
            ):
                began = time.monotonic()
                status, refused = server.request("GET", "/api/agents")
            elapsed = time.monotonic() - began

        self.assertEqual(status, 503)
        self.assertEqual(refused, {"error": remote.LISTING_TIMEOUT_MESSAGE})
        self.assertLess(elapsed, 1.0)

    def test_connection_saturation_fails_promptly_with_bounded_threads(self):
        class BlockingHandler(remote.Handler):
            admitted_handlers = None
            release = None

            def admitted(self):
                if not super().admitted():
                    return False
                self.admitted_handlers.release()
                assert self.release.wait(5), "fixture release timeout"
                return True

            def log_message(self, format, *args):
                pass

        limit = remote.BoundedHTTPServer.MAX_CONNECTIONS
        BlockingHandler.admitted_handlers = threading.Semaphore(0)
        BlockingHandler.release = threading.Event()
        replies = []

        with LiveGateway(BlockingHandler) as server:
            for _ in range(limit):
                connection = server.connection()
                connection.request(
                    "GET", "/api/health", headers={"X-Lapis-Client": "ios"}
                )

                def collect(item=connection):
                    response = item.getresponse()
                    replies.append((response.status, response.read()))
                    item.close()

                threading.Thread(target=collect, daemon=True).start()
            for _ in range(limit):
                assert BlockingHandler.admitted_handlers.acquire(timeout=10)
            began = time.monotonic()
            status, busy = server.request("GET", "/api/health")
            elapsed = time.monotonic() - began
            BlockingHandler.release.set()
            while len(replies) < limit:
                self.assertLess(time.monotonic() - began, 10)
                time.sleep(0.01)
            while server.httpd.active_connections._value < limit:
                self.assertLess(time.monotonic() - began, 10)
                time.sleep(0.01)
            connection_count = server.httpd.active_connections._value

        self.assertEqual(status, 503)
        self.assertEqual(busy, {"error": remote.HTTP_BUSY_MESSAGE})
        self.assertLess(elapsed, 1.0)
        self.assertEqual(
            [response_status for response_status, _ in replies], [200] * limit
        )
        self.assertEqual(connection_count, limit)

    def test_a_disconnected_long_poll_releases_its_connection(self):
        with LiveGateway() as server:
            _, listed = server.request("GET", "/api/agents")
            connection = server.connection(timeout=5)
            connection.request(
                "GET",
                f"/api/agents?after={listed['version']}",
                headers={"X-Lapis-Client": "ios"},
            )
            connection.close()

            began = time.monotonic()
            while server.httpd.active_connections._value < server.httpd.MAX_CONNECTIONS:
                self.assertLess(time.monotonic() - began, 2)
                time.sleep(0.01)

        self.assertLess(time.monotonic() - began, 2)


def local_tailscale():
    """Admit loopback without touching the machine's real Tailnet."""

    def runner(command):
        if command[1] == "status":
            return {
                "Self": {
                    "UserID": 7,
                    "TailscaleIPs": ["100.64.0.1"],
                    "DNSName": "gateway.test.",
                },
                "User": {"7": {"LoginName": "owner@example.invalid"}},
            }
        return {
            "UserProfile": {"LoginName": "owner@example.invalid"},
            "Node": {"Hostinfo": {"OS": "ios"}},
        }

    return runner


def workspace_registry(directory, *, agents=1):
    """A desktop registry whose agent sockets are private test endpoints."""
    entries = []
    for number in range(agents):
        identifier = f"agent-{number}"
        entries.append(
            {
                "id": identifier,
                "title": identifier,
                "category": "work",
                "harness": "codex",
                "directory": str(directory),
                "program": "codex",
                "arguments": [],
                "endpoint": str(directory / f"{identifier}.sock"),
            }
        )
    registry = directory / "workspace.json"
    registry.write_text(
        json.dumps(
            {
                "activeCategory": "work",
                "categories": [{"id": "work", "name": "Work"}],
                "agents": entries,
            }
        )
    )
    return registry


class LiveGateway:
    """A real loopback gateway with loopback-only admission."""

    def __init__(self, handler_class=remote.Handler):
        self.handler_class = handler_class

    def __enter__(self):
        self.directory = Path(tempfile.mkdtemp(prefix="lapis-bounds-"))
        self.registry = workspace_registry(self.directory, agents=1)
        self.auth = remote.TailnetAuth(allow_local=True, runner=local_tailscale())
        gateway = remote.Gateway(
            self.registry, self.auth, 0, folders=object(), machines=object()
        )
        self.httpd = remote.BoundedHTTPServer(("127.0.0.1", 0), self.handler_class)
        gateway.port = self.httpd.server_address[1]
        self.httpd.daemon_threads = True
        remote.Handler.gateway = gateway
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(5)
        shutil.rmtree(self.directory, True)

    @property
    def gateway(self):
        return remote.Handler.gateway

    def connection(self, timeout=5):
        return http.client.HTTPConnection(
            "127.0.0.1", self.httpd.server_address[1], timeout=timeout
        )

    def request(self, method, path, headers=None):
        connection = self.connection()
        connection.request(
            method,
            path,
            headers={"X-Lapis-Client": "ios", **(headers or {})},
        )
        response = connection.getresponse()
        body = response.read()
        connection.close()
        return response.status, json.loads(body) if body else None


def folder(machine, marker):
    return {
        "version": marker,
        "machine": machine,
        "home": f"/home/{machine}",
        "folders": [],
        "frequent": [],
    }


class RemoteFolderBoundsTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.folders = remote.RemoteFolders()
        self.folders.clock = self.clock
        self.calls = []
        self.started = {}
        self.release = {}
        self.fail_machines = {"bad-machine"}
        self.arrived = None
        patcher = patch.object(remote, "remote_folder_report", self.report)
        patcher.start()
        self.addCleanup(patcher.stop)

    def report(self, machine):
        self.calls.append(machine)
        started = self.started.setdefault(machine, threading.Event())
        release = self.release.setdefault(machine, threading.Event())
        started.set()
        if self.arrived is not None:
            self.arrived.wait(5)
        if not release.wait(5):
            raise AssertionError(f"fixture release timeout: {machine}")
        if machine in self.fail_machines:
            raise subprocess.SubprocessError(machine)
        return folder(machine, f"report-{machine}-{len(self.calls)}")

    def call_async(self, machine):
        result = {}
        self.started[machine] = threading.Event()

        def run():
            result["value"] = self.folders.current(machine, timeout=5)

        thread = threading.Thread(target=run)
        thread.start()
        return thread, result

    def test_builds_are_admitted_in_fours_and_extra_targets_fail_closed(self):
        names = ("a", "b", "c", "d")
        self.arrived = threading.Barrier(len(names) + 1)
        workers = [self.call_async(name) for name in names]
        self.arrived.wait(5)

        extra = self.call_async("extra")
        extra[0].join(5)
        self.assertFalse(extra[0].is_alive())
        self.assertEqual(
            extra[1]["value"], (None, remote.RemoteFolders.TOO_MANY_MESSAGE)
        )
        self.assertEqual(len(self.calls), 4)

        for event in self.release.values():
            event.set()
        for thread, _ in workers:
            thread.join(5)
            self.assertFalse(thread.is_alive())

    def test_duplicate_builds_coalesce_into_one_ssh_call(self):
        thread, result = self.call_async("shared")
        self.assertTrue(self.started["shared"].wait(5))
        duplicate_waiting = threading.Event()

        class WatchedEvent:
            def __init__(self, underlying, entered):
                self.underlying = underlying
                self.entered = entered

            def wait(self, timeout):
                self.entered.set()
                return self.underlying.wait(timeout)

            def set(self):
                self.underlying.set()

        self.folders.builds["shared"] = WatchedEvent(
            self.folders.builds["shared"], duplicate_waiting
        )
        duplicate = self.call_async("shared")
        self.assertTrue(duplicate_waiting.wait(5))
        self.release["shared"].set()
        thread.join(5)
        duplicate[0].join(5)
        self.assertFalse(thread.is_alive() or duplicate[0].is_alive())
        self.assertEqual(self.calls, ["shared"])
        self.assertEqual(result["value"][0]["version"], "report-shared-1")

    def test_failed_machine_retries_after_the_bounded_delay(self):
        machine = "bad-machine"
        self.release[machine] = threading.Event()
        self.release[machine].set()
        first = self.call_async(machine)
        first[0].join(5)
        self.assertEqual(first[1]["value"], (None, machine))
        self.fail_machines.clear()

        self.clock.now += remote.RemoteFolders.ERROR_RETRY - 1
        self.assertEqual(self.folders.current(machine, timeout=5), (None, machine))
        self.assertEqual(len(self.calls), 1)

        self.clock.now += 1
        retry = self.call_async(machine)
        self.release[machine].set()
        retry[0].join(5)
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(retry[1]["value"][0]["version"], f"report-{machine}-2")

    def test_thread_start_failure_releases_admission_for_retry(self):
        with patch.object(
            threading.Thread, "start", side_effect=RuntimeError("no thread")
        ):
            self.assertEqual(self.folders.current("recover"), (None, "no thread"))
        self.clock.now += remote.RemoteFolders.ERROR_RETRY
        self.release["recover"] = threading.Event()
        self.release["recover"].set()
        result = self.folders.current("recover", timeout=5)
        self.assertIsNotNone(result[0])
        self.assertEqual(self.calls, ["recover"])

    def test_a_stale_report_stays_usable_while_its_replacement_builds(self):
        machine = "stable"
        self.release[machine] = threading.Event()
        self.release[machine].set()
        initial = self.call_async(machine)
        initial[0].join(5)
        first = initial[1]["value"]
        self.assertEqual(first[0]["version"], "report-stable-1")

        self.clock.now += remote.RemoteFolders.STALE + 1
        self.release[machine] = threading.Event()
        self.arrived = threading.Barrier(2)
        replacement = self.call_async(machine)
        self.arrived.wait(5)
        self.arrived = None
        self.assertEqual(
            self.folders.current(machine, timeout=5),
            first,
            "the replacement should refresh while the old report serves",
        )
        completion = self.folders.builds[machine]
        self.release[machine].set()
        replacement[0].join(5)
        self.assertTrue(completion.wait(5))
        self.assertEqual(
            self.folders.current(machine, timeout=5)[0]["version"],
            "report-stable-2",
        )

    def test_reports_and_errors_are_retained_only_up_to_their_bounds(self):
        report_limit = remote.RemoteFolders.MAX_REPORTS
        names = [f"kept-{number}" for number in range(report_limit + 1)]
        for name in names:
            self.release[name] = threading.Event()
            self.release[name].set()
            self.folders.current(name, timeout=5)
        self.assertNotIn(names[0], self.folders.reports)
        self.assertEqual(len(self.folders.reports), report_limit)

        error_limit = remote.RemoteFolders.MAX_ERRORS
        failures = [f"bad-{number}" for number in range(error_limit + 1)]
        self.clock.now = 2000
        for name in failures:
            self.fail_machines.add(name)
            self.release[name] = threading.Event()
            self.release[name].set()
            self.folders.current(name, timeout=5)
        self.assertNotIn(failures[0], self.folders.errors)
        self.assertEqual(len(self.folders.errors), error_limit)


class FakeTailscale:
    def __init__(
        self, owner="owner@example.invalid", own=("100.64.0.1",), system="Mac"
    ):
        self.owner = owner
        self.system = system
        self.own = own
        self.calls = []
        self.started = {}
        self.release = {}

    def __call__(self, command):
        if command[1] == "status":
            return {
                "Self": {
                    "UserID": 7,
                    "TailscaleIPs": list(self.own),
                    "DNSName": "mac.",
                },
                "User": {"7": {"LoginName": self.owner}},
            }
        address = command[-1]
        self.calls.append(address)
        started = self.started.setdefault(address, threading.Event())
        release = self.release.setdefault(address, threading.Event())
        started.set()
        if not release.wait(5):
            raise AssertionError(f"fixture release timeout: {address}")
        if address.startswith("fail"):
            raise subprocess.SubprocessError(address)
        return {
            "UserProfile": {"LoginName": self.owner},
            "Node": {"Hostinfo": {"OS": self.system}},
        }


class TailnetAdmissionBoundsTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()

    def auth(self, tailscale):
        result = remote.TailnetAuth(allow_local=True, runner=tailscale)
        result.clock = self.clock
        return result

    def test_allowed_verdict_stays_allowed_until_expiry(self):
        tailscale = FakeTailscale(system="iOS")
        auth = self.auth(tailscale)
        address = "100.64.0.2"
        tailscale.release[address] = threading.Event()
        tailscale.release[address].set()
        self.assertTrue(auth.allowed(address))
        tailscale.system = "Mac"
        self.clock.now += remote.TailnetAuth.ALLOWED_TTL - 1
        self.assertTrue(auth.allowed(address))
        self.assertEqual(tailscale.calls, [address])
        self.clock.now += 1
        self.assertFalse(auth.allowed(address))
        self.assertEqual(tailscale.calls, [address, address])

    def test_rejected_and_failed_lookups_briefly_suppress_retry(self):
        tailscale = FakeTailscale()
        auth = self.auth(tailscale)
        tailscale.release["100.64.0.2"] = threading.Event()
        tailscale.release["100.64.0.2"].set()
        self.assertFalse(auth.allowed("100.64.0.2"))
        self.clock.now += remote.TailnetAuth.REJECTED_TTL - 1
        self.assertFalse(auth.allowed("100.64.0.2"))
        self.assertEqual(tailscale.calls, ["100.64.0.2"])

        self.clock.now += 1
        self.assertFalse(auth.allowed("100.64.0.2"))
        self.assertEqual(tailscale.calls, ["100.64.0.2", "100.64.0.2"])

        failing = FakeTailscale()
        auth = self.auth(failing)
        failing.release["fail-1"] = threading.Event()
        failing.release["fail-1"].set()
        failing.release["fail-1"].set()
        self.assertFalse(auth.allowed("fail-1"))
        self.clock.now += remote.TailnetAuth.REJECTED_TTL - 1
        self.assertFalse(auth.allowed("fail-1"))
        self.assertEqual(len(failing.calls), 1)

    def test_simultaneous_whois_work_is_bounded_and_released(self):
        tailscale = FakeTailscale()
        auth = self.auth(tailscale)
        addresses = [f"100.64.0.{number}" for number in range(2, 6)]
        workers = []
        for address in addresses[: remote.TailnetAuth.MAX_WHOIS]:
            worker = threading.Thread(target=auth.allowed, args=(address,))
            worker.start()
            workers.append(worker)
            self.assertTrue(tailscale.started[address].wait(5))

        self.assertFalse(auth.allowed("100.64.9.9"))
        self.assertNotIn("100.64.9.9", tailscale.calls)

        for address in addresses[: remote.TailnetAuth.MAX_WHOIS]:
            tailscale.release[address].set()
        for worker in workers:
            worker.join(5)
            self.assertFalse(worker.is_alive())

        tailscale.release["100.64.9.9"] = threading.Event()
        tailscale.release["100.64.9.9"].set()
        self.assertFalse(auth.allowed("100.64.9.9"))
        self.assertIn("100.64.9.9", tailscale.calls)

    def test_duplicate_lookups_coalesce_and_admission_always_releases(self):
        tailscale = FakeTailscale()
        auth = self.auth(tailscale)
        address = "100.64.1.2"
        results = []
        first = threading.Thread(target=lambda: results.append(auth.allowed(address)))
        first.start()
        self.assertTrue(tailscale.started[address].wait(5))
        pending_event = auth.pending[address]
        original_wait = pending_event.wait
        duplicate_waiting = threading.Event()

        def observe_wait(timeout):
            duplicate_waiting.set()
            return original_wait(timeout)

        pending_event.wait = observe_wait
        second = threading.Thread(target=lambda: results.append(auth.allowed(address)))
        second.start()
        self.assertTrue(duplicate_waiting.wait(5))
        tailscale.release[address].set()
        first.join(5)
        second.join(5)
        self.assertFalse(first.is_alive() or second.is_alive())
        self.assertEqual(tailscale.calls, [address])
        self.assertEqual(results, [False, False])

    def test_identity_cache_is_bounded_and_evicts_oldest(self):
        tailscale = FakeTailscale()
        auth = self.auth(tailscale)
        base = 10
        addresses = [
            f"100.64.{number // 256}.{number % 256}"
            for number in range(base, base + remote.TailnetAuth.MAX_CACHE + 1)
        ]
        for address in addresses:
            tailscale.release[address] = threading.Event()
            tailscale.release[address].set()
            self.assertFalse(auth.allowed(address))
        self.assertEqual(len(tailscale.calls), remote.TailnetAuth.MAX_CACHE + 1)
        self.assertNotIn(addresses[0], auth.cache)
        self.assertEqual(len(auth.cache), remote.TailnetAuth.MAX_CACHE)

        # A retained lookup remains cached and the bound remains stable.
        self.assertFalse(auth.allowed(addresses[-1]))
        self.assertEqual(len(tailscale.calls), remote.TailnetAuth.MAX_CACHE + 1)
        self.assertEqual(auth.cache[addresses[-1]][0], False)
