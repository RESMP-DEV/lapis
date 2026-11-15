#!/usr/bin/env python3
"""Build and run the isolated native-browser W0 probe.

The probe downloads one pinned CEF standard distribution, verifies both
digests, overlays the lapis-owned host in the ignored build tree, and
exercises a hidden browser without remote DevTools. It is an engine probe, not
product web-service wiring.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CEF_VERSION = "156.0.30+g6028375+chromium-156.0.8078.96"
ARCHIVE_NAME = "cef_binary_156.0.30+g6028375+chromium-156.0.8078.96_macosarm64.tar.bz2"
ARCHIVE_URL = f"https://cef-builds.spotifycdn.com/{ARCHIVE_NAME}"
ARCHIVE_SHA1 = "269bb579ada39eefab3c13baf82b3fd46dd42581"
ARCHIVE_SHA256 = "22a510e5d348a907953278b618e1e9e442ccf78773b9fee0e09eaf154eef53b0"
ARCHIVE_SIZE = 312_211_060
ARCHIVE_CONNECT_TIMEOUT_SECONDS = 15
ARCHIVE_TOTAL_TIMEOUT_SECONDS = 3600
SOURCE_PATHS = (
    "scripts/check_web_engine.py",
    "tools/web_probe/cefsimple_mac.mm",
    "tools/web_probe/simple_app.cc",
    "tools/web_probe/simple_handler.cc",
    "tools/web_probe/simple_handler.h",
    "tools/web_probe/w0.html",
)


class CheckError(RuntimeError):
    """A W0 acceptance condition failed."""


@dataclass(frozen=True)
class ProcessSample:
    pid: int
    ppid: int
    rss_kb: int
    command: str


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    timeout: float = 300,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        tail = "\n".join(result.stderr.splitlines()[-20:])
        raise CheckError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{tail}"
        )
    return result


def digest(path: Path, algorithm: str) -> str:
    hasher = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def archive_path(cache: Path) -> Path:
    path = cache / ARCHIVE_NAME
    partial = cache / f"{ARCHIVE_NAME}.download"
    if path.exists():
        verify_archive(path)
        return path
    cache.mkdir(parents=True, exist_ok=True)
    partial.unlink(missing_ok=True)
    try:
        subprocess.run(
            [
                "curl",
                "--fail",
                "--location",
                "--progress-bar",
                "--retry",
                "3",
                "--connect-timeout",
                str(ARCHIVE_CONNECT_TIMEOUT_SECONDS),
                "--max-time",
                str(ARCHIVE_TOTAL_TIMEOUT_SECONDS),
                "--output",
                str(partial),
                ARCHIVE_URL,
            ],
            check=True,
        )
        verify_archive(partial)
        partial.replace(path)
    finally:
        partial.unlink(missing_ok=True)
    return path


def verify_archive(path: Path) -> None:
    if path.stat().st_size != ARCHIVE_SIZE:
        raise CheckError(
            f"archive size changed: {path.stat().st_size} != {ARCHIVE_SIZE}"
        )
    sha1 = digest(path, "sha1")
    sha256 = digest(path, "sha256")
    if sha1 != ARCHIVE_SHA1 or sha256 != ARCHIVE_SHA256:
        raise CheckError(f"archive digest mismatch: sha1={sha1} sha256={sha256}")


def extract_archive(cache: Path, archive: Path) -> Path:
    root = cache / f"cef-{CEF_VERSION}"
    extracted = root / ARCHIVE_NAME.removesuffix(".tar.bz2")
    marker = extracted / "README.txt"
    if not marker.exists():
        root.mkdir(parents=True, exist_ok=True)
        run(["tar", "-xjf", str(archive), "-C", str(root)], timeout=600)
    if CEF_VERSION not in extracted.name:
        raise CheckError(f"unexpected extracted CEF root: {extracted}")
    return extracted


def overlay_host(extracted: Path) -> None:
    source = ROOT / "tools/web_probe"
    target = extracted / "tests/cefsimple"
    for name in (
        "simple_app.cc",
        "simple_handler.h",
        "simple_handler.cc",
        "cefsimple_mac.mm",
    ):
        (target / name).write_bytes((source / name).read_bytes())

    cmake = target / "CMakeLists.txt"
    text = cmake.read_text()
    text = text.replace('set(CEF_TARGET "cefsimple")', 'set(CEF_TARGET "lapis_w0")')
    text = text.replace(
        'set(CEF_HELPER_TARGET "cefsimple_Helper")',
        'set(CEF_HELPER_TARGET "lapis_w0_Helper")',
    )
    text = text.replace(
        'set(CEF_HELPER_OUTPUT_NAME "cefsimple Helper")',
        'set(CEF_HELPER_OUTPUT_NAME "lapis_w0 Helper")',
    )
    text = "\n".join(
        line for line in text.splitlines() if line.strip() != "simple_handler_mac.mm"
    )
    cmake.write_text(text + "\n")


def build_host(extracted: Path) -> Path:
    build = extracted / "build/lapis-w0"
    run(
        [
            "cmake",
            "-G",
            "Ninja",
            "-S",
            str(extracted),
            "-B",
            str(build),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DPROJECT_ARCH=arm64",
            "-DUSE_SANDBOX=OFF",
        ],
        timeout=300,
    )
    run(
        [
            "cmake",
            "--build",
            str(build),
            "--target",
            "lapis_w0",
            "--parallel",
            "8",
        ],
        timeout=900,
    )
    app = build / "tests/cefsimple/Release/lapis_w0.app"
    executable = app / "Contents/MacOS/lapis_w0"
    if not executable.is_file():
        raise CheckError(f"missing W0 executable: {executable}")
    return app


def process_samples() -> list[ProcessSample]:
    result = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,rss=,comm="],
        text=True,
        capture_output=True,
        check=True,
    )
    samples: list[ProcessSample] = []
    for line in result.stdout.splitlines():
        fields = line.split(None, 3)
        if len(fields) != 4:
            continue
        try:
            samples.append(
                ProcessSample(int(fields[0]), int(fields[1]), int(fields[2]), fields[3])
            )
        except ValueError:
            continue
    return samples


def family_samples(root_pid: int) -> list[ProcessSample]:
    samples = process_samples()
    by_parent: dict[int, list[ProcessSample]] = {}
    for sample in samples:
        by_parent.setdefault(sample.ppid, []).append(sample)
    family = {root_pid}
    changed = True
    while changed:
        changed = False
        for parent, children in by_parent.items():
            if parent not in family:
                continue
            for child in children:
                if child.pid not in family:
                    family.add(child.pid)
                    changed = True
    return [sample for sample in samples if sample.pid in family]


def wait_for_descendant_exit(pids: list[int], timeout: float = 10.0) -> None:
    """Wait for already-sampled descendants without relying on PPID links."""
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        alive = {sample.pid for sample in process_samples()}
        if not any(pid in alive for pid in pids):
            return
        time.sleep(0.1)
    alive = {sample.pid for sample in process_samples()}
    survivors = sorted(pid for pid in pids if pid in alive)
    if survivors:
        raise CheckError(f"W0 descendant processes survived: {survivors}")


def signal_processes(pids: list[int], sig: signal.Signals) -> None:
    for pid in pids:
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            continue


def terminate_family(
    process: subprocess.Popen[bytes], sampled_pids: list[int] | None = None
) -> None:
    """Bound failure paths; successful runs must exit naturally."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and process.poll() is None:
        time.sleep(0.1)
    if process.poll() is None:
        process.kill()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if sampled_pids is not None:
        signal_processes(sampled_pids, signal.SIGKILL)


def listening_tcp_report(pids: list[int]) -> str:
    if not pids:
        return "no live processes to inspect"
    result = subprocess.run(
        [
            "lsof",
            "-a",
            "-nP",
            "-iTCP",
            "-sTCP:LISTEN",
            "-p",
            ",".join(str(pid) for pid in pids),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    return result.stdout.strip() or result.stderr.strip() or "zero listeners"


def read_processes(
    process: subprocess.Popen[bytes], run_dir: Path
) -> tuple[list[ProcessSample], str]:
    time.sleep(0.7)
    samples = family_samples(process.pid)
    if not samples:
        raise CheckError("could not sample the W0 process family")
    listeners = listening_tcp_report([sample.pid for sample in samples])
    payload = {
        "processes": [sample.__dict__ for sample in samples],
        "rss_mb": sum(sample.rss_kb for sample in samples) / 1024,
    }
    (run_dir / "process-sample.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )
    return samples, listeners


def wait_for_exit(
    process: subprocess.Popen[bytes], timeout: float
) -> tuple[int, float]:
    started = time.monotonic()
    while process.poll() is None:
        if time.monotonic() - started > timeout:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            return 124, time.monotonic() - started
        time.sleep(0.1)
    return process.returncode or 0, time.monotonic() - started


def parse_frame(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    lines = data.split(b"\n", 3)
    if len(lines) != 4 or lines[0] != b"P6" or lines[2] != b"255":
        raise CheckError(f"invalid PPM frame: {path}")
    try:
        width, height = (int(value) for value in lines[1].split())
    except ValueError as error:
        raise CheckError(f"invalid PPM dimensions: {path}") from error
    offset = len(lines[0]) + len(lines[1]) + len(lines[2]) + 3
    pixels = data[offset:]
    if (width, height) != (800, 600) or len(pixels) != width * height * 3:
        raise CheckError(
            f"invalid frame geometry: {width}x{height}, {len(pixels)} bytes"
        )
    return {
        "width": width,
        "height": height,
        "sha256": hashlib.sha256(data).hexdigest(),
        "nonwhite_pixels": sum(pixel != 255 for pixel in pixels),
    }


def validate_outputs(run_dir: Path) -> dict[str, Any]:
    if (run_dir / "load-error.txt").exists():
        raise CheckError(
            "W0 fixture load error: "
            + (run_dir / "load-error.txt").read_text(encoding="utf-8").strip()
        )
    result = json.loads((run_dir / "result.json").read_text(encoding="utf-8"))
    required = (
        "trusted_mouse_click",
        "trusted_keyboard_character",
        "accessibility_tree",
        "dom_snapshot",
        "runtime_evaluation",
    )
    if not result.get("passed"):
        raise CheckError(f"W0 host rejected its result: {result}")
    for field in required:
        if result.get(field) is not True:
            raise CheckError(f"W0 field is not true: {field}")
    if result.get("load_error") is not False:
        raise CheckError("W0 fixture reported a load error")
    if not (run_dir / "teardown-complete").is_file():
        raise CheckError("W0 teardown did not complete through CefShutdown")

    accessibility = json.loads(
        (run_dir / "accessibility.json").read_text(encoding="utf-8")
    )
    roles = {
        node.get("role", {}).get("value") for node in accessibility.get("nodes", [])
    }
    if not {"button", "textbox", "RootWebArea"}.issubset(roles):
        raise CheckError(f"semantic roles missing from accessibility tree: {roles}")

    dom = json.loads((run_dir / "dom.json").read_text(encoding="utf-8"))
    dom_text = json.dumps(dom)
    if dom.get("root", {}).get("nodeName") != "#document":
        raise CheckError("DOM snapshot has no document root")
    if "INPUT" not in dom_text or "BUTTON" not in dom_text:
        raise CheckError("DOM snapshot lacks fixture controls")

    runtime = json.loads(
        (run_dir / "runtime-evaluation.json").read_text(encoding="utf-8")
    )
    value = runtime.get("result", {}).get("value")
    if not isinstance(value, str):
        raise CheckError("Runtime evaluation did not return a string")
    evaluated = json.loads(value)
    if evaluated != {
        "lapisProbe": "injected",
        "button": True,
        "input": True,
    }:
        raise CheckError(f"unexpected runtime evaluation: {evaluated}")

    frames = {name: parse_frame(run_dir / name) for name in ("first.ppm", "latest.ppm")}
    stderr = (run_dir / "stderr.log").read_text(encoding="utf-8")
    if stderr:
        raise CheckError(f"W0 host wrote stderr: {stderr}")
    return {
        "host_result": result,
        "accessibility_node_count": len(accessibility.get("nodes", [])),
        "accessibility_roles": sorted(value for value in roles if value),
        "frames": frames,
        "runtime_value": evaluated,
    }


def source_state() -> dict[str, Any]:
    source_files = [ROOT / path for path in SOURCE_PATHS]
    source_hash = hashlib.sha256()
    for path in sorted(source_files):
        source_hash.update(path.relative_to(ROOT).as_posix().encode())
        source_hash.update(b"\0")
        source_hash.update(path.read_bytes())
        source_hash.update(b"\0")
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    status = subprocess.run(
        [
            "git",
            "status",
            "--porcelain",
            "--untracked-files=all",
            "--",
            *SOURCE_PATHS,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.splitlines()
    return {
        "revision": revision,
        "dirty": bool(status),
        "changed_paths": [line[3:] for line in status],
        "source_digest": source_hash.hexdigest(),
    }


def toolchain_versions() -> dict[str, str]:
    versions = {}
    for name in ("cmake", "ninja", "clang++"):
        result = subprocess.run(
            [name, "--version"],
            text=True,
            capture_output=True,
            check=True,
        )
        versions[name] = result.stdout.splitlines()[0]
    return versions


def write_failure(report: Path, error: Exception) -> None:
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "recorded_at": datetime.now(timezone.utc).isoformat(),
                "scope": "Native browser W0 probe startup or verification failure.",
                "passed": False,
                "error": str(error),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def execute(arguments: argparse.Namespace) -> dict[str, Any]:
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        raise CheckError("W0 is currently pinned to macOS Apple Silicon")

    cache = arguments.cache_dir.resolve()
    report = arguments.report.resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    report.unlink(missing_ok=True)
    archive = archive_path(cache)
    extracted = extract_archive(cache, archive)
    source_before_overlay = source_state()
    overlay_host(extracted)
    app = build_host(extracted)
    executable = app / "Contents/MacOS/lapis_w0"

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = cache / f"runs/{stamp}"
    run_dir.mkdir(parents=True)
    environment = dict(os.environ)
    environment["LAPIS_W0_OUT"] = str(run_dir)
    environment["LAPIS_W0_URL"] = (ROOT / "tools/web_probe/w0.html").resolve().as_uri()
    with (
        (run_dir / "stdout.log").open("wb") as stdout,
        (run_dir / "stderr.log").open("wb") as stderr,
    ):
        process = subprocess.Popen(
            [
                str(executable),
                "--disable-gpu",
            ],
            stdout=stdout,
            stderr=stderr,
            env=environment,
            start_new_session=True,
        )
        try:
            samples, listeners = read_processes(process, run_dir)
            exit_code, elapsed = wait_for_exit(process, arguments.timeout)
        except BaseException:
            terminate_family(process)
            raise

    sampled_pids = [sample.pid for sample in samples if sample.pid != process.pid]
    if exit_code == 124:
        terminate_family(process, sampled_pids)
        raise CheckError(f"W0 host timed out after {arguments.timeout} seconds")
    try:
        wait_for_descendant_exit(sampled_pids)
    except BaseException:
        terminate_family(process, sampled_pids)
        raise
    if exit_code != 0:
        if exit_code == 75 and (run_dir / "teardown-error.txt").exists():
            raise CheckError(
                "W0 browser close stalled; host aborted before CefShutdown"
            )
        raise CheckError(f"W0 host exited {exit_code}")
    if listeners != "zero listeners":
        raise CheckError(f"W0 opened a listening TCP socket: {listeners}")
    validated = validate_outputs(run_dir)
    installed_size = sum(
        path.stat().st_size for path in app.rglob("*") if path.is_file()
    )
    source_after_run = source_state()
    if source_after_run != source_before_overlay:
        raise CheckError("W0 source changed during the build or run")
    receipt = {
        "schema_version": 1,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "scope": (
            "Isolated CEF W0 probe on a local fixture; no product web service, "
            "credentials, remote debugging, native GUI, or W1 wiring."
        ),
        "source": source_before_overlay,
        "toolchain": toolchain_versions(),
        "passed": True,
        "artifact": {
            "version": CEF_VERSION,
            "url": ARCHIVE_URL,
            "size": ARCHIVE_SIZE,
            "sha1": ARCHIVE_SHA1,
            "sha256": ARCHIVE_SHA256,
            "installed_app_bytes": installed_size,
        },
        "elapsed_seconds": elapsed,
        "process_sample": {
            "count": len(samples),
            "rss_mb": sum(sample.rss_kb for sample in samples) / 1024,
            "commands": [Path(sample.command).name for sample in samples],
        },
        "listening_tcp": listeners,
        "validation": validated,
    }
    report.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=ROOT / "build/web-engine",
        help="ignored download, extraction, and run directory",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=ROOT / "build/reports/web-engine/receipt.json",
        help="ignored JSON receipt path",
    )
    parser.add_argument("--timeout", type=float, default=45.0)
    arguments = parser.parse_args()
    try:
        receipt = execute(arguments)
    except (
        CheckError,
        OSError,
        ValueError,
        TypeError,
        subprocess.SubprocessError,
    ) as error:
        write_failure(arguments.report, error)
        print(f"FAIL web-engine W0: {error}", file=sys.stderr)
        return 1
    print(
        "PASS web-engine W0: "
        f"{arguments.report} ({receipt['process_sample']['count']} processes)",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
