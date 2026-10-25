import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
CHECKER_PATH = SCRIPTS / "check_supervisor_runtime.py"
path_owner = str(SCRIPTS)
if path_owner not in sys.path:
    sys.path.insert(0, path_owner)
spec = importlib.util.spec_from_file_location("check_supervisor_runtime", CHECKER_PATH)
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class SupervisorRuntimeReceiptTests(unittest.TestCase):
    def integration(self, logs: Path) -> tuple[dict[str, object], dict[str, object]]:
        logs.mkdir(parents=True, exist_ok=True)
        (logs / "test.log").write_text("output")
        test_result = {"exit_code": 1, "timed_out": False, "elapsed_seconds": 12.5}
        return checker.integration_result("output", test_result, logs), test_result

    def test_failed_integration_is_not_reported_as_a_bind_skip(self) -> None:
        result, test_result = self.integration(
            ROOT / "build" / "test-supervisor-receipt"
        )
        self.assertFalse(result["passed"])
        self.assertFalse(result["integration_skipped"])
        self.assertEqual(result["exit_code"], test_result["exit_code"])
        self.assertEqual(result["elapsed_seconds"], test_result["elapsed_seconds"])
        self.assertIn("did not pass", str(result["diagnostic"]))
        self.assertNotIn("bind is unavailable", str(result["diagnostic"]))

    def test_bind_skip_reports_exit_and_skip_without_success_claim(self) -> None:
        logs = ROOT / "build" / "test-supervisor-receipt-skip"
        logs.mkdir(parents=True, exist_ok=True)
        logs.joinpath("test.log").write_text("QLocalServer bind unavailable\n")
        result = checker.integration_result(
            "QLocalServer bind unavailable",
            {"exit_code": 75, "timed_out": False, "elapsed_seconds": 9.5},
            logs,
        )
        self.assertFalse(result["passed"])
        self.assertTrue(result["integration_skipped"])
        self.assertEqual(result["exit_code"], 75)
        self.assertEqual(result["elapsed_seconds"], 9.5)
        self.assertIn("bind is unavailable", str(result["diagnostic"]))
        self.assertNotIn("exited successfully", str(result["diagnostic"]))

    def test_daemon_result_requires_its_own_observed_marker(self) -> None:
        logs = ROOT / "build" / "test-supervisor-daemon-receipt"
        logs.mkdir(parents=True, exist_ok=True)
        logs.joinpath("test.log").write_text("supervisor-session-service: ok\n")
        result = checker.daemon_integration_result(
            "supervisor-session-service: ok",
            {"exit_code": 1, "timed_out": False, "elapsed_seconds": 7.25},
            logs,
        )
        self.assertFalse(result["passed"])
        self.assertFalse(result["integration_skipped"])
        self.assertIn("daemon integration marker was absent", str(result["diagnostic"]))
        self.assertEqual(result["elapsed_seconds"], 7.25)

    def test_output_outside_checkout_is_rejected_before_receipt_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "lapis-supervisor-receipt-outside.json"
            self.assertFalse(output.resolve().is_relative_to(ROOT))
            completed = subprocess.run(
                [sys.executable, str(CHECKER_PATH), "--output", str(output)],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("must name a receipt inside", completed.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
