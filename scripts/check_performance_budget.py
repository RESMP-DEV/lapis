#!/usr/bin/env python3
"""Check measured latency receipts against the reviewed snappiness budget.

The budget records the product-level input-to-frame gate for the reference
machine. It is not a frame-budget proof: the probe's endpoint is frame
submission after the matching snapshot revision synchronized, and the receipt
must come from the correlated probe on the machine named by the run.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1
PROBE_SCHEMA = "lapis.terminal-latency/1"
ACCEPTED_INPUTS = ("OS-injected", "synthetic Qt Return")


def fail(message: str) -> None:
    raise ValueError(message)


def load_receipt(path: Path) -> dict[str, Any]:
    try:
        receipt = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read latency receipt {path}: {error}")
    if not isinstance(receipt, dict):
        fail(f"latency receipt {path} is not a JSON object")
    return receipt


def verified_measurements(receipt: dict[str, Any]) -> dict[str, float]:
    if receipt.get("schema") != PROBE_SCHEMA:
        fail(f"unexpected latency schema: {receipt.get('schema')!r}")
    if not any(prefix in str(receipt.get("input", "")) for prefix in ACCEPTED_INPUTS):
        fail("receipt does not use a correlated input mode")
    distribution = receipt.get("input_to_frame")
    if not isinstance(distribution, dict):
        fail("receipt has no input_to_frame distribution")
    measurements: dict[str, float] = {}
    for percentile in ("p50_ms", "p95_ms", "p99_ms"):
        value = distribution.get(percentile)
        if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
            fail(f"input_to_frame.{percentile} is missing or invalid")
        measurements[percentile] = float(value)
    if not (measurements["p50_ms"] <= measurements["p95_ms"] <= measurements["p99_ms"]):
        fail("input_to_frame percentiles are not ordered")
    return measurements


def check(path: Path, budget: dict[str, Any]) -> dict[str, Any]:
    receipt = load_receipt(path)
    measured = verified_measurements(receipt)
    checks: list[dict[str, Any]] = []
    passed = True
    for percentile, maximum in budget["max_ms"].items():
        if percentile not in measured:
            fail(f"budget names unmeasured percentile {percentile}")
        value = measured[percentile]
        check_passed = value <= maximum
        passed = passed and check_passed
        checks.append(
            {
                "metric": f"input_to_frame.{percentile}",
                "measured_ms": value,
                "max_ms": maximum,
                "passed": check_passed,
            }
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "scope": (
            "Input-to-frame-submission budget check; no pixel presentation, "
            "switch latency or sustained-output claim"
        ),
        "source_receipt": str(path),
        "source_schema": receipt.get("schema"),
        "samples": receipt.get("samples"),
        "refresh_hz": receipt.get("refresh_hz"),
        "budget_name": budget["name"],
        "reference": budget["reference"],
        "checks": checks,
        "passed": passed,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path, help="probe receipt to check")
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build/reports/performance-budget/receipt.json",
        help="ignored-build receipt path",
    )
    arguments = parser.parse_args()
    # Never leave a prior PASS receipt behind an interrupted or failed run.
    arguments.output.unlink(missing_ok=True)
    budget_path = ROOT / "scripts/performance_budget.json"
    try:
        budget = json.loads(budget_path.read_text(encoding="utf-8"))
        if not isinstance(budget.get("max_ms"), dict) or not budget["max_ms"]:
            fail("performance budget has no max_ms entries")
        result = check(arguments.receipt, budget)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )
        print(
            f"{'PASS' if result['passed'] else 'FAIL'} performance budget: "
            f"{arguments.output}"
        )
        return 0 if result["passed"] else 1
    except (OSError, ValueError) as error:
        print(f"FAIL performance budget: {error}", file=sys.stderr)
        try:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(
                json.dumps(
                    {
                        "schema_version": SCHEMA_VERSION,
                        "recorded_at": datetime.now(timezone.utc).isoformat(),
                        "scope": "Performance budget gate startup or verification failure.",
                        "passed": False,
                        "error": str(error),
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
        except OSError:
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
