"""Check that the real service publishes only whole frames of a program that
brackets its repaints in synchronized updates (DEC mode 2026), as Claude Code's
full-screen renderer does, and that an update never ended cannot freeze the
screen."""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from check_cli_launch import TEXT, CheckError, Service, require

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r"""
import sys,termios,time
attr=termios.tcgetattr(0);attr[3]&=~termios.ECHO;termios.tcsetattr(0,termios.TCSANOW,attr)
out=sys.stdout
print('READY',flush=True)
for line in sys.stdin:
    command=line.strip()
    if command=='frame':
        # One repaint, flushed in two halves well over a frame apart.
        out.write('\x1b[?2026h\x1b[2J\x1b[HHALF\r\n');out.flush()
        time.sleep(0.15)
        out.write('WHOLE\r\n\x1b[?2026l');out.flush()
    elif command=='burst':
        # One write, no quiet gap: several complete DEC 2026 frames queued
        # together exercise end/begin detection at the parser boundary.
        for number in range(5):
            out.write(f'\x1b[?2026h\x1b[2J\x1b[HFRAME{number}\r\n\x1b[?2026l')
        out.flush()
    elif command=='stuck':
        out.write('\x1b[?2026h\x1b[2J\x1b[HSTUCK\r\n');out.flush()
"""


def exercise(binary, runtime, artifacts):
    service = Service(
        binary,
        runtime,
        artifacts,
        "sync",
        sys.executable,
        ["-u", "-c", FIXTURE],
        runtime,
    )
    seen = []
    try:
        with service.connect() as client:
            client.snapshot(lambda s: "READY" in s["text"])
            for _ in range(5):
                client.send(TEXT, b"frame\n")
                start = time.monotonic()
                client.snapshot(
                    lambda s: seen.append(s["text"]) or "WHOLE" in s["text"], timeout=5
                )
                require(
                    time.monotonic() - start < 1, "A whole frame took over a second"
                )
                client.send(TEXT, b"\n")  # a quiet moment between repaints
                time.sleep(0.05)
            half = [t for t in seen if "HALF" in t and "WHOLE" not in t]
            require(not half, f"{len(half)} half-drawn screens were published")
            # One write, no quiet gap: several finished DEC 2026 frames
            # are already queued back to back.
            client.send(TEXT, b"burst\n")
            client.snapshot(
                lambda s: seen.append(s["text"]) or "FRAME4" in s["text"],
                timeout=5,
            )
            half = [
                text
                for text in seen
                if ("HALF" in text and "WHOLE" not in text) or "PARTIAL" in text
            ]
            require(not half, f"{len(half)} half-drawn screens were published")
            require(
                all("FRAME3" not in text for text in seen),
                "An intermediate no-gap frame was published",
            )
            client.send(TEXT, b"stuck\n")
            start = time.monotonic()
            client.snapshot(lambda s: "STUCK" in s["text"], timeout=3)
            stuck_ms = round((time.monotonic() - start) * 1000)
            require(
                250 <= stuck_ms < 1500,
                f"The stuck update left the screen at {stuck_ms} ms",
            )
        return {"frames": 5, "screens_seen": len(seen), "stuck_shown_ms": stuck_ms}
    finally:
        service.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/sync-output-check/receipt.json"
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    receipt = {"schema": "lapis.sync-output-check/1", "passed": False}
    try:
        with tempfile.TemporaryDirectory(prefix="lapis-sync-", dir="/tmp") as directory:
            runtime = Path(directory)
            binary = args.build_dir / "services/session/lapis_session_service"
            receipt["service"] = exercise(binary, runtime, runtime)
            receipt["passed"] = True
    except (
        CheckError,
        OSError,
        ValueError,
        EOFError,
        struct.error,
        subprocess.SubprocessError,
    ) as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
    finally:
        args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Receipt: {args.output}")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
