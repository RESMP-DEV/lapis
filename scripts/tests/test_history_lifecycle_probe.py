"""A failed Swift compile must leave a truthful receipt and no run artifacts."""

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.qa import run_history_lifecycle_probe as probe


class ProbeCleanupTests(unittest.TestCase):
    def test_fixture_errors_reject_an_otherwise_successful_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "receipt.json"
            source = root / "fixture.swift"
            source.write_text("// isolated source receipt\n")
            with (
                patch.object(probe, "ROOT", root),
                patch.object(probe, "SWIFT_SOURCES", (source,)),
                patch.object(probe, "PROBE_SOURCE", source),
                patch.object(probe, "compile_probe"),
                patch.object(probe.DefaultsIsolation, "remove"),
                patch.object(probe, "LifecycleServer") as server,
                patch.object(
                    probe.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0, "{}", ""),
                ),
                patch.object(sys, "argv", ["probe", "--output", str(output)]),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                server.return_value.fixture_state.return_value = {
                    "unexpected_routes": ["unexpected fixture path: /unknown"]
                }
                self.assertEqual(probe.main(), 1)
            receipt = json.loads(output.read_text())
            self.assertEqual(receipt["exit_code"], 0)
            self.assertFalse(receipt["passed"])
            self.assertIn("unexpected routes", receipt["error"])

    def test_compile_failure_cleans_negative_source_and_replaces_stale_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "Models.swift"
            model.write_text(
                "let current = try await gateway.agents()\n"
                "            guard hostGeneration == generation else { return }"
            )
            source = root / "probe.swift"
            source.write_text("// fixture\n")
            output = root / "receipt.json"
            output.write_text('{"passed": true}')
            with (
                patch.object(probe, "ROOT", root),
                patch.object(probe, "SWIFT_SOURCES", (model,)),
                patch.object(probe, "PROBE_SOURCE", source),
                patch.object(
                    probe,
                    "compile_probe",
                    side_effect=subprocess.CalledProcessError(
                        1, ["swiftc"], stderr="controlled compile failure"
                    ),
                ),
                patch.object(probe.DefaultsIsolation, "remove"),
                patch.object(probe, "LifecycleServer") as server,
                patch.object(
                    sys,
                    "argv",
                    ["probe", "--negative-control", "--output", str(output)],
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(probe.main(), 1)
            receipt = json.loads(output.read_text())
            self.assertFalse(receipt["passed"])
            self.assertEqual(receipt["compiler_stderr"], "controlled compile failure")
            self.assertEqual(list((root / "build/qa/history-lifecycle").iterdir()), [])
            server.assert_not_called()
