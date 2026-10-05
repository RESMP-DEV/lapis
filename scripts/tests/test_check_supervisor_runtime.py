import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS = ROOT / "scripts"
_PATH = ROOT / "scripts" / "check_supervisor_runtime.py"
_path_owner = str(_SCRIPTS)
if _path_owner not in sys.path:
    sys.path.insert(0, _path_owner)
_spec = importlib.util.spec_from_file_location("check_supervisor_runtime", _PATH)
assert _spec and _spec.loader
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)


def integration(logs: Path) -> tuple[dict[str, object], dict[str, object]]:
    logs.mkdir(parents=True, exist_ok=True)
    (logs / "test.log").write_text("output")
    test_result = {"exit_code": 1, "timed_out": False}
    return checker.integration_result("output", test_result, logs), test_result


def test_failed_integration_is_not_reported_as_a_bind_skip() -> None:
    result, test_result = integration(ROOT / "build" / "test-supervisor-receipt")
    assert result["passed"] is False
    assert result["integration_skipped"] is False
    assert result["exit_code"] == test_result["exit_code"]
    assert "did not pass" in str(result["diagnostic"])
    assert "bind is unavailable" not in str(result["diagnostic"])


def test_bind_skip_reports_exit_and_skip_without_success_claim() -> None:
    logs = ROOT / "build" / "test-supervisor-receipt-skip"
    logs.mkdir(parents=True, exist_ok=True)
    logs.joinpath("test.log").write_text("QLocalServer bind unavailable\n")
    result = checker.integration_result(
        "QLocalServer bind unavailable", {"exit_code": 75, "timed_out": False}, logs
    )
    assert result["passed"] is False
    assert result["integration_skipped"] is True
    assert result["exit_code"] == 75
    assert "bind is unavailable" in str(result["diagnostic"])
    assert "exited successfully" not in str(result["diagnostic"])


def test_output_outside_checkout_is_rejected_before_receipt_write(tmp_path) -> None:
    output = tmp_path / "lapis-supervisor-receipt-outside.json"
    assert not output.resolve().is_relative_to(ROOT)
    completed = subprocess.run(
        [sys.executable, str(_PATH), "--output", str(output)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 2
    assert "must name a receipt inside" in completed.stderr
    assert not output.exists()
