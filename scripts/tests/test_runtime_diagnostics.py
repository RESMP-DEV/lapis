"""Behavioral cases for read-only runtime diagnosis."""

import errno
import io
import json
import os
import plistlib
import stat
import sys
import tempfile
import unittest
import uuid
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from scripts import lapis, runtime_diagnostics as diagnostics


class RuntimeDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.executable = self.root / "lapis"
        self.executable.write_text("#!/usr/bin/env true\n")
        self.executable.chmod(0o755)

    def write_registry(self, identifier=None, *, corrupt=False):
        identifier = identifier or str(uuid.uuid4())
        endpoint = str(self.runtime / f"{identifier}.sock")
        data = {
            "version": 2,
            "activeCategory": "general",
            "categories": [
                {"id": "general", "name": "General", "selected": identifier}
            ],
            "agents": [
                {
                    "id": identifier,
                    "title": "agent",
                    "category": "general",
                    "endpoint": endpoint,
                    "program": "/usr/bin/true",
                    "harness": "codex",
                    "mode": "",
                    "resumeThread": "",
                    "arguments": [],
                    "directory": str(self.root),
                }
            ],
        }
        path = self.runtime / "workspace.json"
        path.write_text("broken" if corrupt else json.dumps(data))
        path.chmod(0o600)
        return identifier

    def write_descriptor(self, identifier, *, valid=True):
        path = self.runtime / f"{identifier}.sock.session"
        if valid:
            path.write_bytes(
                diagnostics.DESCRIPTOR_MAGIC
                + uuid.UUID(identifier).bytes
                + uuid.uuid4().bytes
                + b"fingerprint".ljust(32, b"\0")
            )
        else:
            path.write_bytes(b"short")
        path.chmod(0o600)

    def write_bundle(self):
        app = self.root / "Lapis.app"
        executable = app / "Contents/MacOS/lapis_desktop"
        executable.parent.mkdir(parents=True)
        executable.write_text("#!/usr/bin/env true\n")
        executable.chmod(0o755)
        plist = {
            "CFBundleExecutable": "lapis_desktop",
            "CFBundleIdentifier": "dev.lapis.desktop",
            "CFBundleShortVersionString": "0.5.0",
            "CFBundleVersion": "0.5.0",
            "LSMinimumSystemVersion": "14.0",
        }
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(plist))
        return app

    def write_plist(self, app, value):
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(value))

    def run_diagnose(self, *arguments):
        output = io.StringIO()
        try:
            with redirect_stdout(output):
                code = diagnostics.main(list(arguments))
        except SystemExit as exit_status:
            code = exit_status.code
        return code, output.getvalue()

    def test_ready_bundle_reports_bounded_evidence_without_protocol_access(self):
        app = self.write_bundle()
        identifier = self.write_registry()
        self.write_descriptor(identifier)
        terminal_data = {
            "version": 1,
            "terminals": [
                {
                    "id": "terminal-a",
                    "endpoint": str(self.runtime / "terminal-a.sock"),
                    "program": "/usr/bin/true",
                    "arguments": [],
                    "directory": str(self.root),
                },
                {
                    "id": "terminal-b",
                    "endpoint": str(self.runtime / "terminal-b.sock"),
                    "program": "/usr/bin/true",
                    "arguments": [],
                    "directory": str(self.root),
                },
            ],
        }
        terminals = self.runtime / "terminals.json"
        terminals.write_text(json.dumps(terminal_data))
        terminals.chmod(0o600)
        events = []

        class FakeSocket:
            def __init__(self, family=None, type=None):
                self.family = family
                self.type = type

            def __enter__(self):
                return self

            def __exit__(self, *_):
                self.close()

            def settimeout(self, value):
                events.append(("timeout", value))

            def connect_ex(self, endpoint):
                events.append(("connect", endpoint))
                return (
                    0
                    if str(endpoint).endswith("terminal-a.sock")
                    else errno.ECONNREFUSED
                )

            def close(self):
                events.append(("close",))

        original_path_state = diagnostics.path_state

        def socket_path_state(path):
            if path.name in {"terminal-a.sock", "terminal-b.sock"}:
                return {"state": "present", "kind": "socket"}
            return original_path_state(path)

        (self.runtime / "terminal-a.sock").write_bytes(b"")
        (self.runtime / "terminal-b.sock").write_bytes(b"")
        before = {
            path.name: (path.read_bytes() if path.is_file() else None)
            for path in self.runtime.iterdir()
            if path.is_file()
        }
        with (
            patch.object(diagnostics.socket, "socket", FakeSocket),
            patch.object(diagnostics, "path_state", socket_path_state),
        ):
            code, output = self.run_diagnose(
                "--json", "--app", str(app), "--runtime", str(self.runtime)
            )

        report = json.loads(output)
        after = {
            path.name: (path.read_bytes() if path.is_file() else None)
            for path in self.runtime.iterdir()
            if path.is_file()
        }
        self.assertEqual(after, before)
        self.assertEqual(code, 0)
        self.assertEqual(report["diagnostic_status"], "ready")
        self.assertEqual(
            report["app"]["metadata"]["short_version"],
            "0.5.0",
        )
        self.assertFalse(report["app"]["protocol"]["negotiation_attempted"])
        self.assertEqual(report["runtime"]["registry"]["observations"]["listed"], 1)
        self.assertEqual(
            report["runtime"]["registry"]["observations"]["categories_listed"],
            1,
        )
        self.assertEqual(report["runtime"]["reachability"]["connectable"], 1)
        self.assertEqual(report["runtime"]["reachability"]["not_listening"], 1)
        self.assertEqual(
            [event[0] for event in events],
            ["timeout", "connect", "close", "timeout", "connect", "close"],
        )
        self.assertFalse(
            any(event[0] in ("send", "sendall", "recv") for event in events)
        )
        self.assertEqual(report["runtime"]["reachability"]["health"], "unknown")
        self.assertFalse(report["runtime"]["reachability"]["authoritative"])
        self.assertEqual(report["health"], "unknown")
        self.assertEqual(
            report["runtime"]["descriptors"]["valid_shape"],
            1,
        )
        self.assertEqual(
            report["runtime"]["descriptors"]["identity_match"],
            "unknown without the launch fingerprint",
        )

        def keys(value):
            if isinstance(value, dict):
                for name, child in value.items():
                    yield name
                    yield from keys(child)
            elif isinstance(value, list):
                for child in value:
                    yield from keys(child)

        observed = set(keys(report))
        self.assertFalse(
            observed
            & {"account", "arguments", "directory", "program", "resumeThread", "title"}
        )

    def test_plain_executable_and_empty_runtime_are_ready(self):
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 0)
        self.assertEqual(report["app"]["kind"], "executable")
        self.assertEqual(report["runtime"]["registry"]["state"], "missing")
        self.assertEqual(report["runtime"]["state"], "ok")

    def test_bounded_samples_measure_connect_only_distributions(self):
        first = str(uuid.uuid4())
        second = str(uuid.uuid4())
        outcomes = {
            first: [0, 0],
            second: [errno.ECONNREFUSED, TimeoutError()],
        }
        events = []
        monotonic_values = iter(
            (
                1_000_000,
                3_000_000,
                4_000_000,
                8_000_000,
                10_000_000,
                11_000_000,
                12_000_000,
                15_000_000,
            )
        )

        class FakeSocket:
            def __init__(self, family=None, type=None):
                self.family = family
                self.type = type

            def __enter__(self):
                return self

            def __exit__(self, *_):
                self.close()

            def settimeout(self, value):
                events.append(("timeout", value))

            def connect_ex(self, endpoint):
                name = str(endpoint).rsplit("/", 1)[-1]
                identifier = name.removesuffix(".sock")
                events.append(("connect", name))
                outcome = outcomes[identifier].pop(0)
                if isinstance(outcome, TimeoutError):
                    raise outcome
                return outcome

            def close(self):
                events.append(("close",))

        original_path_state = diagnostics.path_state

        def socket_path_state(path):
            if path.name in {f"{first}.sock", f"{second}.sock"}:
                return {"state": "present", "kind": "socket"}
            return original_path_state(path)

        with (
            patch.object(diagnostics.socket, "socket", FakeSocket),
            patch.object(diagnostics, "path_state", socket_path_state),
            patch.object(diagnostics.time, "monotonic_ns"),
        ):
            diagnostics.time.monotonic_ns.side_effect = monotonic_values
            reachability, _ = diagnostics._probe_endpoints(
                self.runtime, [first, second], samples=2
            )

        measurement = reachability["measurement"]
        self.assertEqual(reachability["samples_requested"], 2)
        self.assertEqual(reachability["sample_count"], 4)
        self.assertEqual(reachability["connectable"], 2)
        self.assertEqual(reachability["success_count"], 2)
        self.assertEqual(reachability["not_listening"], 1)
        self.assertEqual(reachability["timeout"], 1)
        self.assertEqual(reachability["timeout_count"], 1)
        self.assertEqual(measurement["aggregate"]["sample_count"], 4)
        self.assertEqual(measurement["aggregate"]["p50_ms"], 2)
        self.assertEqual(measurement["aggregate"]["p95_ms"], 4)
        self.assertEqual(measurement["aggregate"]["p99_ms"], 4)
        self.assertEqual(measurement["aggregate"]["max_ms"], 4)
        self.assertEqual(measurement["by_classification"]["connectable"]["p50_ms"], 2)
        self.assertEqual(measurement["by_classification"]["not_listening"]["max_ms"], 1)
        self.assertEqual(measurement["by_classification"]["timeout"]["max_ms"], 3)
        self.assertEqual(measurement["by_classification"]["unknown"]["sample_count"], 0)
        self.assertEqual(
            [event[0] for event in events],
            [
                "timeout",
                "connect",
                "close",
                "timeout",
                "connect",
                "close",
                "timeout",
                "connect",
                "close",
                "timeout",
                "connect",
                "close",
            ],
        )
        self.assertFalse(
            any(event[0] in ("send", "sendall", "recv") for event in events)
        )

    def test_sample_bound_rejects_values_outside_one_to_eight(self):
        parser = diagnostics.build_parser()
        self.assertEqual(parser.parse_args([]).samples, 1)
        self.assertEqual(parser.parse_args(["--samples", "8"]).samples, 8)
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                parser.parse_args(["--samples", "0"])
        self.assertEqual(caught.exception.code, 2)

    def test_export_carries_only_aggregate_connect_measurements(self):
        app = self.write_bundle()
        report = diagnostics.diagnose(app, self.runtime, samples=3)
        report["runtime"]["reachability"]["measurement"].update(
            {
                "aggregate": {
                    "sample_count": 1,
                    "p50_ms": 2,
                    "p95_ms": 2,
                    "p99_ms": 2,
                    "max_ms": 2,
                },
                "by_endpoint": {"secret": [2]},
                "endpoint": "secret.sock",
            }
        )
        report["runtime"]["reachability"]["endpoint_timings"] = {"secret.sock": [2]}
        export = diagnostics.support_export(report)
        serialized = json.dumps(export)
        self.assertEqual(
            export["runtime"]["reachability"]["measurement"]["aggregate"]["max_ms"],
            2,
        )
        self.assertNotIn("by_endpoint", export)
        self.assertNotIn("endpoint_timings", export)
        self.assertNotIn("secret.sock", serialized)

    def test_missing_selected_inputs_are_structured_and_nonzero(self):
        code, output = self.run_diagnose(
            "--json",
            "--app",
            str(self.root / "missing"),
            "--runtime",
            str(self.runtime),
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["app"]["state"], "missing")
        self.assertEqual(report["app"]["issues"][0]["code"], "app-missing")

        code, output = self.run_diagnose(
            "--json",
            "--app",
            str(self.executable),
            "--runtime",
            str(self.root / "missing"),
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["runtime"]["state"], "missing")
        self.assertEqual(report["runtime"]["issues"][0]["code"], "runtime-missing")

    def test_private_runtime_rules_are_reused_without_repair(self):
        self.runtime.chmod(0o755)
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["runtime"]["state"], "invalid")
        self.assertIn("must have mode 0700", report["runtime"]["issues"][0]["detail"])
        self.assertEqual(stat.S_IMODE(os.lstat(self.runtime).st_mode), 0o755)

    def test_registry_permissions_are_nonzero_even_when_json_parses(self):
        self.write_registry()
        registry = self.runtime / "workspace.json"
        registry.chmod(0o644)
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["runtime"]["state"], "invalid")
        self.assertEqual(
            report["runtime"]["registry"]["issues"][0]["code"],
            "metadata-not-private",
        )

    def test_untrusted_runtime_ancestor_is_reported_without_repair(self):
        shared = self.root / "shared"
        shared.mkdir(mode=0o777)
        shared.chmod(0o777)
        isolated = shared / "runtime"
        isolated.mkdir(mode=0o700)
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(isolated)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["runtime"]["state"], "invalid")
        self.assertEqual(
            report["runtime"]["issues"][0]["code"],
            "runtime-ancestor-untrusted",
        )
        self.assertEqual(stat.S_IMODE(os.lstat(shared).st_mode), 0o777)

    def test_corrupt_registry_is_a_structured_failure(self):
        self.write_registry(corrupt=True)
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["runtime"]["registry"]["state"], "corrupt")
        self.assertEqual(report["runtime"]["state"], "corrupt")
        self.assertEqual(
            report["runtime"]["registry"]["issues"][0]["code"], "metadata-corrupt"
        )

    def test_terminals_count_only_private_records_without_publishing_launch_data(self):
        identifier = str(uuid.uuid4())
        terminal = f"terminal-{identifier}"
        terminals = {
            "version": 1,
            "terminals": [
                {
                    "id": terminal,
                    "endpoint": str(self.runtime / f"{terminal}.sock"),
                    "program": "/usr/bin/true",
                    "arguments": [],
                    "directory": str(self.root),
                },
                {
                    "id": "terminal-other",
                    "endpoint": "/outside/terminal-other.sock",
                },
            ],
        }
        path = self.runtime / "terminals.json"
        path.write_text(json.dumps(terminals))
        path.chmod(0o600)
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(report["runtime"]["terminals"]["observations"]["listed"], 2)
        self.assertEqual(
            report["runtime"]["terminals"]["observations"]["valid_private"], 1
        )
        self.assertEqual(
            report["runtime"]["terminals"]["observations"]["invalid_records"], 1
        )
        self.assertEqual(report["runtime"]["terminals"]["state"], "invalid")
        self.assertEqual(code, 1)
        self.assertNotIn("terminal-a", output)
        self.assertNotIn("/usr/bin/true", output)

    def test_portable_defaults_follow_lapis_home(self):
        with patch.dict(
            os.environ,
            {"HOME": str(self.root), "LAPIS_HOME": str(self.root)},
            clear=True,
        ):
            self.assertEqual(diagnostics.default_runtime(), self.runtime)
            if hasattr(diagnostics.os.path, "expanduser"):
                self.assertEqual(
                    diagnostics.default_app(), Path("/Applications/lapis.app")
                )

    def test_registry_and_terminal_versions_fail_closed_without_record_scans(self):
        identifier = str(uuid.uuid4())
        self.write_registry(identifier)
        registry = self.runtime / "workspace.json"
        data = json.loads(registry.read_text())
        data["version"] = 3
        registry.write_text(json.dumps(data))
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(
            report["runtime"]["registry"]["issues"][0]["code"],
            "registry-version-invalid",
        )
        self.assertEqual(report["runtime"]["registry"]["observations"]["listed"], 0)

        terminals = self.runtime / "terminals.json"
        terminals.write_text(json.dumps({"version": 2, "terminals": []}))
        terminals.chmod(0o600)
        _, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(
            report["runtime"]["terminals"]["issues"][0]["code"],
            "terminals-version-invalid",
        )
        self.assertEqual(report["runtime"]["terminals"]["observations"]["listed"], 0)

    def test_terminal_arguments_keep_their_current_launcher_bound(self):
        def terminal_record(index):
            identifier = f"terminal-{index:032x}"
            return {
                "id": identifier,
                "endpoint": str(self.runtime / f"{identifier}.sock"),
                "program": "/usr/bin/true",
                "arguments": [str(value) for value in range(256)],
                "directory": str(self.root),
            }

        terminals = self.runtime / "terminals.json"
        terminals.write_text(
            json.dumps({"version": 1, "terminals": [terminal_record(1)]})
        )
        terminals.chmod(0o600)
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 0)
        self.assertEqual(report["runtime"]["terminals"]["state"], "ok")

        terminals.write_text(
            json.dumps(
                {
                    "version": 1,
                    "terminals": [
                        {
                            **terminal_record(1),
                            "arguments": [str(value) for value in range(257)],
                        }
                    ],
                }
            )
        )
        code, output = self.run_diagnose(
            "--json", "--app", str(self.executable), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["runtime"]["terminals"]["state"], "invalid")
        self.assertIn(
            "exceed documented bounds",
            report["runtime"]["terminals"]["issues"][-1]["detail"],
        )

    def test_plain_failure_is_structured_without_a_traceback(self):
        code, output = self.run_diagnose(
            "--app", str(self.root / "missing"), "--runtime", str(self.runtime)
        )
        self.assertEqual(code, 1)
        self.assertIn("App: missing", output)
        self.assertIn("Runtime: ok", output)
        self.assertIn("Health: unknown", output)

    def test_export_is_explicit_and_redacts_selected_and_launch_data(self):
        app = self.write_bundle()
        identifier = self.write_registry()
        self.write_descriptor(identifier)
        terminals = self.runtime / "terminals.json"
        terminals.write_text(json.dumps({"version": 1, "terminals": []}))
        terminals.chmod(0o600)
        code, output = self.run_diagnose(
            "--export", "--app", str(app), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 0)
        self.assertEqual(report["export"], "support")
        self.assertEqual(report["app"]["metadata"]["short_version"], "0.5.0")
        self.assertNotIn(str(app), output)
        self.assertNotIn(str(self.runtime), output)
        self.assertNotIn(str(self.root), output)
        self.assertNotIn(identifier, output)

        def keys(value):
            if isinstance(value, dict):
                for name, child in value.items():
                    yield name
                    yield from keys(child)
            elif isinstance(value, list):
                for child in value:
                    yield from keys(child)

        self.assertNotIn("selected", set(keys(report)))
        forbidden = {"account", "arguments", "directory", "program", "title"}
        self.assertFalse(forbidden & set(keys(report)))
        code, _ = self.run_diagnose(
            "--export",
            "--json",
            "--app",
            str(app),
            "--runtime",
            str(self.runtime),
        )
        self.assertEqual(code, 2)

    def test_export_never_carries_path_shaped_package_metadata(self):
        app = self.write_bundle()
        self.write_plist(
            app,
            {
                "CFBundleExecutable": "lapis_desktop",
                "CFBundleIdentifier": "dev.lapis.desktop",
                "CFBundleShortVersionString": f"{self.root}/private-version",
                "CFBundleVersion": "0.5.0",
                "LSMinimumSystemVersion": "14.0",
            },
        )
        code, output = self.run_diagnose(
            "--export", "--app", str(app), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 0)
        self.assertIsNone(report["app"]["metadata"]["short_version"])
        self.assertNotIn(str(self.root), output)

    def test_bundle_plist_is_parsed_once_and_executable_stays_inside_bundle(self):
        app = self.write_bundle()
        self.write_plist(app, ["not-a-dictionary"])
        code, output = self.run_diagnose(
            "--json", "--app", str(app), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(code, 1)
        self.assertEqual(report["app"]["metadata"]["state"], "corrupt")
        self.assertEqual(
            report["app"]["metadata"]["error"], "metadata is not an object"
        )
        self.assertFalse(report["app"]["executable"]["present"])

        self.write_plist(app, {"CFBundleExecutable": "../escaped"})
        _, output = self.run_diagnose(
            "--json", "--app", str(app), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertFalse(report["app"]["metadata"]["executable_name_present"])
        self.assertFalse(report["app"]["executable"]["present"])
        self.assertFalse((self.root / "escaped").exists())

    def test_bundle_plist_rejects_symlink_and_oversized_inode(self):
        app = self.write_bundle()
        outside = self.root / "outside.plist"
        outside.write_bytes(plistlib.dumps({"CFBundleExecutable": "escaped"}))
        plist = app / "Contents/Info.plist"
        plist.unlink()
        plist.symlink_to(outside)
        _, output = self.run_diagnose(
            "--json", "--app", str(app), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(report["app"]["metadata"]["state"], "invalid")
        self.assertEqual(report["app"]["metadata"]["error"], "metadata-symlink")
        self.assertFalse((self.root / "escaped").exists())

        plist.unlink()
        plist.write_bytes(b" " * (diagnostics.MAX_METADATA_BYTES + 1))
        _, output = self.run_diagnose(
            "--json", "--app", str(app), "--runtime", str(self.runtime)
        )
        report = json.loads(output)
        self.assertEqual(report["app"]["metadata"]["error"], "metadata-too-large")
        self.assertEqual(report["app"]["metadata"]["state"], "invalid")

    def test_private_json_fails_closed_before_reading_untrusted_files(self):
        untrusted = self.runtime / "workspace.json"
        untrusted.write_text('{"agents": "private-but-public"}')
        untrusted.chmod(0o644)
        result, issues, parseable = diagnostics._private_json(untrusted)
        self.assertEqual(result["state"], "invalid")
        self.assertEqual(issues[0]["code"], "metadata-not-private")
        self.assertFalse(parseable)

        special = self.runtime / "terminals.json"
        os.mkfifo(special, mode=0o600)
        result, issues, _ = diagnostics._private_json(special)
        self.assertEqual(result["state"], "invalid")
        self.assertEqual(issues[0]["code"], "metadata-not-a-file")

        linked = self.runtime / "workspace.json"
        linked.unlink()
        target = self.root / "target.json"
        target.write_text("{}")
        target.chmod(0o600)
        linked.symlink_to(target)
        result, issues, _ = diagnostics._private_json(linked)
        self.assertEqual(result["state"], "invalid")
        self.assertEqual(issues[0]["code"], "metadata-symlink")

        oversized = self.runtime / "workspace.json"
        oversized.unlink()
        oversized.write_bytes(b" " * (diagnostics.MAX_METADATA_BYTES + 1))
        oversized.chmod(0o600)
        result, issues, _ = diagnostics._private_json(oversized)
        self.assertEqual(result["state"], "invalid")
        self.assertEqual(issues[0]["code"], "metadata-too-large")

    def test_private_json_rejects_malformed_root_with_bounded_result(self):
        registry = self.runtime / "workspace.json"
        registry.write_text("[]")
        registry.chmod(0o600)
        result, issues, parseable = diagnostics._private_json(registry)
        self.assertEqual(result["state"], "corrupt")
        self.assertEqual(issues[0]["code"], "metadata-corrupt")
        self.assertEqual(issues[0]["detail"], "Metadata root is not an object")
        self.assertFalse(parseable)

    def test_descriptors_reject_special_files_and_replacement_links(self):
        identifier = str(uuid.uuid4())
        self.write_descriptor(identifier)
        path = self.runtime / f"{identifier}.sock.session"
        self.assertEqual(diagnostics._descriptor_observation(path), "valid_shape")

        link = self.runtime / "linked.sock.session"
        link.symlink_to(path)
        self.assertEqual(diagnostics._descriptor_observation(link), "invalid")

        special = self.runtime / "special.sock.session"
        os.mkfifo(special, mode=0o600)
        self.assertEqual(diagnostics._descriptor_observation(special), "invalid")

        path.write_bytes(path.read_bytes() + b"tail")
        self.assertEqual(diagnostics._descriptor_observation(path), "invalid")

    def test_dispatcher_owns_the_thin_command_boundary(self):
        with (
            patch.object(sys, "argv", ["lapis.py", "diagnose", "--json"]),
            patch.object(lapis, "run_python_script", return_value=7) as runner,
        ):
            self.assertEqual(lapis.main(), 7)
        self.assertEqual(runner.call_args.args[0], "runtime_diagnostics.py")
        self.assertEqual(runner.call_args.args[1], ["--json"])
        self.assertTrue(runner.call_args.kwargs["needs_ghostty"] is False)


if __name__ == "__main__":
    unittest.main()
