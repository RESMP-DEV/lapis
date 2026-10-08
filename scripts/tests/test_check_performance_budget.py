"""Performance budget verification and receipt-shape contracts."""

import json
import tempfile
import unittest
from pathlib import Path

from scripts import check_performance_budget as checker


def receipt(**overrides):
    values = {
        "schema": "lapis.terminal-latency/1",
        "input": "synthetic Qt Return",
        "recorded_at": "2026-10-07T16:00:00Z",
        "samples": 100,
        "refresh_hz": 120,
        "input_to_frame": {
            "p50_ms": 40.0,
            "p95_ms": 50.0,
            "p99_ms": 55.0,
            "max_ms": 57.0,
        },
    }
    values.update(overrides)
    return values


class PerformanceBudgetTests(unittest.TestCase):
    def test_within_budget_passes_and_records_scope(self):
        budget = {
            "name": "m4-max-120hz-input-to-frame",
            "reference": "Apple M4 Max, 120 Hz reference display",
            "min_samples": 20,
            "max_ms": {"p50_ms": 50.0, "p95_ms": 60.0, "p99_ms": 70.0},
        }
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "latency.json"
            source.write_text(json.dumps(receipt()), encoding="utf-8")
            result = checker.check(source, budget)
        self.assertTrue(result["passed"])
        self.assertEqual(
            [item["metric"] for item in result["checks"]],
            [
                "input_to_frame.p50_ms",
                "input_to_frame.p95_ms",
                "input_to_frame.p99_ms",
            ],
        )
        self.assertIn("no pixel presentation", result["scope"])

    def test_over_budget_fails_the_exceeded_percentile(self):
        budget = {
            "name": "fixture",
            "reference": "fixture",
            "min_samples": 20,
            "max_ms": {"p50_ms": 39.0, "p95_ms": 60.0, "p99_ms": 70.0},
        }
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "latency.json"
            source.write_text(json.dumps(receipt()), encoding="utf-8")
            result = checker.check(source, budget)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"][0]["passed"])
        self.assertTrue(result["checks"][1]["passed"])

    def test_unverified_receipt_shapes_are_rejected(self):
        budget = {
            "name": "fixture",
            "reference": "fixture",
            "min_samples": 20,
            "max_ms": {"p95_ms": 60.0},
        }
        cases = (
            {"schema": "lapis.terminal-latency/0"},
            {"input": "Synthetic marker replay"},
            {"recorded_at": ""},
            {"samples": 19},
            {"input_to_frame": {"p95_ms": 0}},
            {"input_to_frame": {"p50_ms": 40.0, "p95_ms": 30.0, "p99_ms": 20.0}},
        )
        for overrides in cases:
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "latency.json"
                source.write_text(json.dumps(receipt(**overrides)), encoding="utf-8")
                with self.assertRaises(ValueError):
                    checker.check(source, budget)

    def test_checked_budget_file_names_every_measured_percentile(self):
        path = Path(__file__).resolve().parents[1] / "performance_budget.json"
        budget = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(set(budget["max_ms"]), {"p50_ms", "p95_ms", "p99_ms"})
        self.assertEqual(
            budget["max_ms"], {"p50_ms": 20.0, "p95_ms": 30.0, "p99_ms": 35.0}
        )
        self.assertGreaterEqual(budget["min_samples"], 20)
        self.assertNotIn("budget_ms", budget)
        self.assertTrue(all(value > 0 for value in budget["max_ms"].values()))
        self.assertTrue(budget["name"])
        self.assertTrue(budget["reference"])


if __name__ == "__main__":
    unittest.main()
