"""Controlled manual keyboard/IME fixture; run inside a dedicated lapis terminal."""

import argparse
import json
import os
import sys
import termios
import time
import tty
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not sys.stdin.isatty():
        parser.error("This fixture must run inside a PTY")
    # Exclusive creation preserves earlier observations and private permissions.
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    attributes = termios.tcgetattr(0)
    intro = (
        "lapis Milestone 1 physical-input qualification\r\n"
        "Use these test inputs; received bytes are shown as hex and saved locally.\r\n"
        "1. Type lapis-native and Return.\r\n"
        "2. Press Control-C (03) and Option-B (1b62).\r\n"
        "3. Paste two lines from the clipboard; observe one bracketed paste.\r\n"
        "4. Use a native IME: compose/commit 日本, then compose/cancel another word.\r\n"
        "5. Compose, change window focus, and return: no late commit should appear.\r\n"
        "6. Control-L prints history; compose, enter Older, then return to Live.\r\n"
        "7. Resize during composition and inspect native candidate placement.\r\n"
        "8. Close/reopen this lapis window during composition; check no stale commit.\r\n"
        "Control-D finishes this fixture and restores terminal settings.\r\n"
    )
    with os.fdopen(descriptor, "w") as output:
        output.write(
            json.dumps(
                {
                    "schema": "lapis.manual-native-input/1",
                    "physical_input_verified": False,
                }
            )
            + "\n"
        )
        output.flush()
        try:
            tty.setraw(0)
            os.write(1, ("\x1b[?2004h" + intro).encode())
            while True:
                data = os.read(0, 65536)
                if not data or data == b"\x04":
                    break
                output.write(
                    json.dumps({"monotonic_ns": time.monotonic_ns(), "hex": data.hex()})
                    + "\n"
                )
                output.flush()
                if data == b"\x0c":
                    os.write(
                        1,
                        "".join(
                            f"history fixture row {i:03d}\r\n" for i in range(80)
                        ).encode(),
                    )
                os.write(1, ("\r\nRECEIVED " + data.hex() + "\r\n").encode())
        finally:
            os.write(1, b"\x1b[?2004l\r\nNative input fixture ended.\r\n")
            termios.tcsetattr(0, termios.TCSANOW, attributes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
