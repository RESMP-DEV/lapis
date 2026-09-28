#!/usr/bin/env python3
"""Compile and run the Foundation history lifecycle probe on loopback.

The server exposes barrier-controlled counts, screen frames and history
responses. Old requests stay outstanding until their replacement attachment is
acknowledged, so absence assertions follow a real release rather than relying
only on elapsed time. No iOS application or simulator is launched.

    uv run --no-project python tools/qa/run_history_lifecycle_probe.py
"""

from __future__ import annotations

import argparse
import json
import hashlib
import tempfile
import subprocess
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]
SWIFT_SOURCES = (
    ROOT / "apps/ios/Lapis/Models.swift",
    ROOT / "apps/ios/Lapis/Gateway.swift",
    ROOT / "apps/ios/Lapis/FolderSearch.swift",
)
PROBE_SOURCE = ROOT / "tools/qa/history_lifecycle_probe.swift"


class DefaultsIsolation:
    """Remove the probe's uniquely named defaults domain after the run."""

    def __init__(self, suite: str) -> None:
        self.suite = suite

    def remove(self) -> None:
        # The unique suite protects the user; an absent domain is not a failure.
        subprocess.run(
            ["/usr/bin/defaults", "delete", self.suite],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def lines(count: int) -> list[list[list]]:
    return [[[f"page row {number}", None, None, 0, 0, 14]] for number in range(count)]


def history_response(key: str, call: int) -> dict:
    if key == "stale":
        held = {2: (77, 80), 4: (98, 80)}
        ordinary = {1: (10, 80), 3: (9, 80), 5: (8, 80)}
    elif key == "prefetch":
        held = {1: (77, 80)}
        ordinary = {2: (2, 80)}
    elif key == "paging":
        held = {}
        ordinary = {1: (30, 30), 2: (29, 30), 3: (28, 30), 4: (0, 0)}
    elif key == "retry":
        held = {}
        ordinary = {1: (0, 0), 2: (10, 80), 3: (0, 0)}
    else:  # Keep unknown fixture keys visibly bounded and inert.
        held = {}
        ordinary = {1: (0, 0)}
    page, row_count = held.get(call, ordinary.get(call, (0, 0)))
    return {
        "page": page,
        "message": "",
        "end": False,
        "busy": False,
        "columns": 80,
        "lines": lines(row_count),
    }


class LifecycleServer:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.counts: dict[str, dict[str, int]] = {}
        self.listing_counts: dict[str, int] = {}
        self.unexpected_routes: list[str] = []
        self.active_workspace = "base"
        self.events: dict[tuple[str, str, int], threading.Event] = {}
        self.stream_releases: list[threading.Event] = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.server.daemon_threads = True
        self.thread = threading.Thread(
            target=self.server.serve_forever, name="history-lifecycle-peer"
        )

    def event(self, key: str, name: str, index: int) -> threading.Event:
        with self.lock:
            identity = (key, name, index)
            return self.events.setdefault(identity, threading.Event())

    def wait(self, key: str, name: str, index: int, timeout: float = 5.0) -> None:
        if not self.event(key, name, index).wait(timeout):
            raise AssertionError(f"fixture barrier timeout: {key} {name}/{index}")

    def counts_for(self, key: str) -> dict[str, int]:
        with self.lock:
            values = self.counts.setdefault(
                key,
                {
                    "streams": 0,
                    "history": 0,
                    "frames": 0,
                    "terminals": 0,
                    "revisions": [],
                },
            ).copy()
            values["revisions"] = values.get("revisions", []).copy()
            values.setdefault("machines", 0)
            values.setdefault("folders", 0)
            return values

    def increment(self, key: str, counter: str) -> int:
        with self.lock:
            values = self.counts.setdefault(
                key,
                {"streams": 0, "history": 0, "frames": 0, "terminals": 0},
            )
            values[counter] = values.get(counter, 0) + 1
            return values[counter]

    def advance_listing(self, workspace: str) -> int:
        with self.lock:
            self.listing_counts[workspace] = self.listing_counts.get(workspace, 0) + 1
            return self.listing_counts[workspace]

    def listing_count(self, workspace: str) -> int:
        with self.lock:
            return self.listing_counts.get(workspace, 0)

    def activate_workspace(self, workspace: str) -> None:
        with self.lock:
            self.active_workspace = workspace

    def active_workspace_name(self) -> str:
        with self.lock:
            return self.active_workspace

    def record_unexpected_route(self, error: AssertionError) -> None:
        with self.lock:
            self.unexpected_routes.append(str(error))

    @staticmethod
    def listing(workspace: str) -> dict:
        if workspace != "machine-failure":
            return {"categories": [], "activeCategory": workspace}
        agents = [
            {
                "id": "machine-failure",
                "title": "screen prefetch",
                "harness": "fixture",
                "directory": "",
                "running": True,
                "onPhone": False,
                "machine": None,
                "place": "this Mac",
            }
        ]
        return {
            "categories": [{"id": workspace, "name": workspace, "agents": agents}],
            "activeCategory": workspace,
        }

    @staticmethod
    def screen(revision: int) -> dict:
        return {
            "revision": revision,
            "columns": 80,
            "rows": 24,
            "cursor": {"x": 0, "y": 0, "visible": True},
            "alternateScreen": False,
            "applicationCursor": False,
            "foreground": "#ffffff",
            "background": "#000000",
            "lines": [],
        }

    def advance_stream(self, key: str) -> tuple[int, int]:
        with self.lock:
            values = self.counts.setdefault(
                key,
                {
                    "streams": 0,
                    "history": 0,
                    "frames": 0,
                    "terminals": 0,
                    "revisions": [],
                },
            )
            values["streams"] += 1
            values["frames"] += 1
            stream = values["streams"]
            revision = values["frames"]
            values["revisions"].append(revision)
            return stream, revision

    def frame(self, revision: int) -> bytes:
        value = self.screen(revision)
        return f"event: frame\ndata: {json.dumps(value)}\n\n".encode()

    def handler(self) -> type[BaseHTTPRequestHandler]:
        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args: object) -> None:
                pass

            def answer(self, body: dict | dict[str, int]) -> None:
                data = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def read_body(self) -> bytes:
                length = int(self.headers.get("Content-Length", "0"))
                return self.rfile.read(length) if length else b""

            def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
                parts = [part for part in urlparse(self.path).path.split("/") if part]
                try:
                    if (
                        len(parts) == 4
                        and parts[:2] == ["api", "agents"]
                        and parts[3] == "input"
                    ):
                        key = parts[2]
                        call = server.increment(key, "input")
                        server.event(key, "input", call).set()
                        if (key, call) in {("input", 1), ("input", 2)}:
                            server.wait(key, "input-release", call)
                        self.read_body()
                        try:
                            self.answer({})
                        finally:
                            server.event(key, "input-done", call).set()
                        return
                    raise AssertionError(f"unexpected fixture POST path: {self.path}")
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except AssertionError as error:
                    server.record_unexpected_route(error)
                    self.send_error(500)

            def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
                parts = [part for part in urlparse(self.path).path.split("/") if part]
                try:
                    self.route(parts)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except AssertionError as error:
                    server.record_unexpected_route(error)
                    self.send_error(500)

            def handle_one_request(self) -> None:
                try:
                    super().handle_one_request()
                except (BrokenPipeError, ConnectionResetError):
                    self.close_connection = True

            def route(self, parts: list[str]) -> None:
                if len(parts) == 2 and parts[0] == "state":
                    self.answer(server.counts_for(parts[1]))
                    return
                if len(parts) == 2 and parts[0] == "state-listing":
                    self.answer({"listings": server.listing_count(parts[1])})
                    return
                if len(parts) == 2 and parts[0] == "workspace-active":
                    server.activate_workspace(parts[1])
                    self.answer({"active": parts[1]})
                    return
                if len(parts) == 3 and parts[0] in {
                    "wait-history",
                    "wait-history-done",
                    "wait-stream",
                    "wait-listing",
                    "wait-listing-done",
                    "wait-terminals",
                    "wait-terminals-done",
                    "wait-input",
                    "wait-input-done",
                }:
                    name = parts[0].removeprefix("wait-")
                    server.wait(parts[1], name, int(parts[2]))
                    self.answer(server.counts_for(parts[1]))
                    return
                if len(parts) == 3 and parts[0] in {
                    "release-history",
                    "release-stream",
                    "release-listing",
                    "release-terminals",
                    "release-input",
                }:
                    server.event(
                        parts[1],
                        parts[0].removeprefix("release-") + "-release",
                        int(parts[2]),
                    ).set()
                    self.answer(server.counts_for(parts[1]))
                    return
                if len(parts) == 3 and parts[0] == "frame":
                    server.event(parts[1], "frame-request", int(parts[2])).set()
                    server.wait(parts[1], "frame-done", int(parts[2]))
                    self.answer(server.counts_for(parts[1]))
                    return
                if len(parts) == 2 and parts[:1] == ["api"]:
                    if parts[1] == "agents":
                        workspace = server.active_workspace_name()
                        call = server.advance_listing(workspace)
                        server.event(workspace, "listing", call).set()
                        if workspace == "held":
                            server.wait(workspace, "listing-release", call)
                        try:
                            self.answer(server.listing(workspace))
                        finally:
                            server.event(workspace, "listing-done", call).set()
                        return
                    if parts[1] == "harnesses":
                        self.answer({"harnesses": [], "defaults": None})
                        return
                    if parts[1] == "machines":
                        workspace = server.active_workspace_name()
                        if workspace == "machine-failure":
                            server.increment(workspace, "machines")
                            self.send_error(500)
                            return
                        self.answer(
                            {
                                "machines": [
                                    {
                                        "name": f"{workspace}-machine",
                                        "uses": 1,
                                        "available": False,
                                    }
                                ]
                            }
                        )
                        return
                    if parts[1] == "terminals":
                        call = server.increment("terminals", "terminals")
                        server.event("terminals", "terminals", call).set()
                        if server.active_workspace_name() == "held-terminals":
                            server.wait("terminals", "terminals-release", call)
                        try:
                            self.answer({"terminals": []})
                        finally:
                            server.event("terminals", "terminals-done", call).set()
                        return
                if len(parts) == 2 and parts[:2] == ["api", "folders"]:
                    server.increment(server.active_workspace_name(), "folders")
                    self.answer(
                        {"version": server.active_workspace_name(), "unchanged": False}
                    )
                    return
                if (
                    len(parts) == 4
                    and parts[:2] == ["api", "agents"]
                    and parts[3] == "history"
                ):
                    key = parts[2]
                    call = server.increment(key, "history")
                    server.event(key, "history", call).set()
                    response = history_response(key, call)
                    if (key, call) in {
                        ("stale", 2),
                        ("stale", 3),
                        ("stale", 4),
                        ("prefetch", 1),
                    }:
                        server.wait(key, "history-release", call)
                    try:
                        self.answer(response)
                    finally:
                        server.event(key, "history-done", call).set()
                    return
                if (
                    len(parts) == 4
                    and parts[:2] == ["api", "agents"]
                    and parts[3] == "screen"
                ):
                    server.increment(parts[2], "frames")
                    self.answer(server.screen(1))
                    return
                if (
                    len(parts) == 4
                    and parts[:2] == ["api", "agents"]
                    and parts[3] == "stream"
                ):
                    key = parts[2]
                    stream, revision = server.advance_stream(key)
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    self.wfile.write(server.frame(revision))
                    self.wfile.flush()
                    server.event(key, "stream", stream).set()
                    release = server.event(key, "stream-release", stream)
                    with server.lock:
                        server.stream_releases.append(release)
                    try:
                        if server.event(key, "frame-request", stream).wait():
                            revision = server.increment(key, "frames")
                            self.wfile.write(server.frame(revision))
                            self.wfile.flush()
                            server.event(key, "frame-done", stream).set()
                        release.wait()
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    finally:
                        server.event(key, "stream-done", stream).set()
                    return
                raise AssertionError(f"unexpected fixture path: {self.path}")

        return Handler

    def release_all(self) -> None:
        with self.lock:
            pending = list(self.events.values())
        for event in pending:
            event.set()

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.release_all()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def fixture_state(self) -> dict:
        state = {
            key: self.counts_for(key)
            for key in (
                "stale",
                "prefetch",
                "paging",
                "retry",
                "input",
                "terminals",
                "machine-failure",
            )
        }
        with self.lock:
            state["workspace"] = {
                "base": self.listing_counts.get("base", 0),
                "held": self.listing_counts.get("held", 0),
            }
            state["unexpected_routes"] = self.unexpected_routes.copy()
        return state


def compile_probe(output: Path, model_source: Path | None = None) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = list(SWIFT_SOURCES)
    if model_source is not None:
        sources[0] = model_source
    command = [
        "xcrun",
        "swiftc",
        "-disable-sandbox",
        "-parse-as-library",
        "-Onone",
        "-module-cache-path",
        str(output.parent / "module-cache"),
        "-module-name",
        "HistoryLifecycleProbe",
        *sources,
        PROBE_SOURCE,
        "-o",
        output,
    ]
    subprocess.run(command, check=True, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build/qa/history-lifecycle/receipt.json",
        help="ignored-build receipt path (default: build/qa/history-lifecycle/receipt.json)",
    )
    parser.add_argument(
        "--negative-control",
        choices=("workspace", "terminals"),
        nargs="?",
        const="workspace",
        help="remove the workspace or held-terminal ownership guard (default: workspace)",
    )
    args = parser.parse_args()
    build = ROOT / "build/qa/history-lifecycle"
    build.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    defaults_suite = f"lapis-history-lifecycle-{uuid.uuid4()}"
    receipt = {
        "passed": False,
        "variant": (
            f"negative-control-{args.negative_control}"
            if args.negative_control
            else "fixed"
        ),
        "scope": "Production Foundation models with controlled HTTP/SSE barriers; no simulator or desktop input.",
        "source_sha256": {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (*SWIFT_SOURCES, PROBE_SOURCE)
        },
    }
    server = None
    try:
        with tempfile.TemporaryDirectory(prefix="run-", dir=build) as directory:
            run = Path(directory)
            negative_model = None
            if args.negative_control == "workspace":
                negative_model = run / "Models-negative-control.swift"
                model = SWIFT_SOURCES[0].read_text()
                original = "let current = try await gateway.agents()\n            guard hostGeneration == generation else { return }"
                replacement = "let current = try await gateway.agents()"
                if model.count(original) != 1:
                    raise AssertionError(
                        "negative-control refresh guard contract changed"
                    )
                negative_model.write_text(model.replace(original, replacement))
                receipt["control_source_sha256"] = hashlib.sha256(
                    negative_model.read_bytes()
                ).hexdigest()
            elif args.negative_control == "terminals":
                negative_model = run / "Models-negative-terminal-control.swift"
                model = SWIFT_SOURCES[0].read_text()
                # Remove just the terminal await's post-response ownership gate.
                original = "guard hostGeneration == generation else { return }\n            if let currentTerminals {"
                replacement = "if let currentTerminals {"
                if model.count(original) != 1:
                    raise AssertionError(
                        "negative-control terminal guard contract changed"
                    )
                negative_model.write_text(model.replace(original, replacement))
                receipt["control_source_sha256"] = hashlib.sha256(
                    negative_model.read_bytes()
                ).hexdigest()
            binary = run / "history-lifecycle-probe"
            compile_probe(binary, negative_model)
            server = LifecycleServer()
            server.start()
            command = [
                str(binary),
                f"127.0.0.1:{server.server.server_port}",
                str(run / "cache"),
                defaults_suite,
            ]
            try:
                result = subprocess.run(
                    command,
                    capture_output=True,
                    text=True,
                    cwd=ROOT,
                    timeout=20,
                    check=False,
                )
                fixture_state = server.fixture_state()
                unexpected = fixture_state.get("unexpected_routes", [])
                passed = result.returncode == 0 and not unexpected
                receipt.update(
                    {
                        "command": [
                            "history-lifecycle-probe",
                            "<loopback endpoint>",
                            "<private cache>",
                            "<private preferences>",
                        ],
                        "exit_code": result.returncode,
                        "observed": json.loads(result.stdout)
                        if result.returncode == 0
                        else {},
                        "fixture_state": fixture_state,
                        "stderr": result.stderr,
                        "passed": passed,
                    }
                )
                if unexpected:
                    receipt["error"] = "fixture received unexpected routes"
            finally:
                server.stop()
                server = None
    except (OSError, subprocess.SubprocessError, ValueError, AssertionError) as error:
        receipt["passed"] = False
        receipt["error"] = f"{type(error).__name__}: {error}"
        if isinstance(error, subprocess.CalledProcessError):
            receipt["compiler_stderr"] = error.stderr
    finally:
        if server is not None:
            server.stop()
        DefaultsIsolation(defaults_suite).remove()
        args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
