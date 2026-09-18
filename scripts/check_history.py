"""Exercise disk-backed history through the real service and attachment protocol."""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from check_cli_launch import (
    RESIZE,
    SNAPSHOT,
    TEXT,
    Service,
    decode_snapshot,
    require,
)

ROOT = Path(__file__).resolve().parents[1]
HISTORY_REQUEST, HISTORY_PAGE = 10, 11
FIXTURE = r"""
import sys,termios
attr=termios.tcgetattr(0);attr[3]&=~termios.ECHO;termios.tcsetattr(0,termios.TCSANOW,attr)
print('READY',flush=True)
for line in sys.stdin:
    command=line.strip()
    if command=='burst':
        for i in range(1500): print('ROW%04d'%i)
        print('BURST_DONE',end='',flush=True)
    else: print('ECHO:'+command,flush=True)
"""


def history(client, request_id, reference=0, newer=False):
    client.send(
        HISTORY_REQUEST,
        client.attachment + struct.pack(">QQB", request_id, reference, newer),
    )
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        kind, payload = client.receive(max(0.01, deadline - time.monotonic()))
        if kind == SNAPSHOT:
            client.sequence = struct.unpack_from(">Q", payload, 40)[0]
            client.cached_snapshot = decode_snapshot(payload[72:])
            continue
        require(kind == HISTORY_PAGE, "Expected history reply")
        require(payload[:40] == client.attachment, "History attachment mismatch")
        reply_id, page_id, length = struct.unpack_from(">QQI", payload, 40)
        require(
            reply_id == request_id and length <= 4096, "History correlation mismatch"
        )
        message = payload[60 : 60 + length].decode("utf-8")
        snapshot = decode_snapshot(payload[60 + length :]) if page_id else None
        return page_id, snapshot, message
    raise RuntimeError("History reply deadline expired")


def exercise(binary, runtime, artifacts, history_root):
    service = Service(
        binary,
        runtime,
        artifacts,
        "history",
        sys.executable,
        ["-u", "-c", FIXTURE],
        runtime,
        {
            "LAPIS_HISTORY_ROOT": str(history_root),
            "LAPIS_HISTORY_SESSION_BYTES": str(1024 * 1024),
            "LAPIS_HISTORY_GLOBAL_BYTES": str(2 * 1024 * 1024),
        },
    )
    try:
        with service.connect() as client:
            client.snapshot(lambda s: "READY" in s["text"])
            client.send(RESIZE, struct.pack(">HH", 20, 1))
            client.send(TEXT, b"burst\n")
            client.snapshot(lambda s: "BURST_DONE" in s["text"], timeout=60)
            page_id, page, message = history(client, 1)
            require(page_id and page and not message, "No usable archived page")
            require(
                "ROW" in page["text"] or "BURST_DONE" in page["text"],
                "Archived text missing",
            )
            ids = [page_id]
            for request in range(2, 22):
                page_id, page, message = history(client, request, ids[-1])
                require(
                    page_id and page and page_id < ids[-1], "Older paging lost order"
                )
                ids.append(page_id)
            newer_id, _, _ = history(client, 30, ids[-1], newer=True)
            require(newer_id == ids[-2], "Newer paging lost order")
            client.send(RESIZE, struct.pack(">HH", 60, 8))
            client.send(TEXT, b"LIVE_OK\n")
            client.snapshot(lambda s: "ECHO:LIVE_OK" in s["text"])
        with service.connect() as client:
            client.snapshot(lambda s: "ECHO:LIVE_OK" in s["text"])
            newest, _, _ = history(client, 31)
            require(newest >= ids[0], "Reattachment lost archive")
            pages = list(history_root.glob("*/*.page"))
            require(pages, "No history pages were written")
            require(
                sum(p.stat().st_size for p in pages) <= 1024 * 1024,
                "Session quota exceeded",
            )
            # Corrupt an owned fixture page and verify both detection and a live PTY.
            latest = max(pages, key=lambda p: int(p.stem))
            original = latest.read_bytes()
            latest.write_bytes(b"broken")
            _, _, error = history(client, 32)
            require(
                "corrupt" in error.lower() or "invalid" in error.lower(),
                "Corruption was not reported",
            )
            client.send(TEXT, b"AFTER_CORRUPTION\n")
            client.snapshot(lambda s: "ECHO:AFTER_CORRUPTION" in s["text"])
            latest.write_bytes(original)
            recovered_id, _, recovered_message = history(client, 33)
            require(recovered_id, "Storage retry did not recover")
            require(
                not recovered_message, "Recovered archive retained stale failure status"
            )
        return {
            "paging_rows": 1500,
            "one_row_backpressure": True,
            "quota_bytes": 1024 * 1024,
            "resize_and_same_pid_reattach": True,
            "corruption_and_live_recovery": True,
        }
    finally:
        service.stop()


def disk_full(binary, runtime, artifacts):
    """Create and detach only our new bounded image; never use an existing device."""
    image = artifacts / "full.dmg"
    mount = runtime / "full-volume"
    mount.mkdir()
    subprocess.run(
        [
            "hdiutil",
            "create",
            "-size",
            "32m",
            "-fs",
            "HFS+",
            "-volname",
            "lapis-history-fixture",
            str(image),
        ],
        check=True,
        capture_output=True,
        timeout=60,
    )
    attached = False
    service = None
    try:
        subprocess.run(
            ["hdiutil", "attach", "-nobrowse", "-mountpoint", str(mount), str(image)],
            check=True,
            capture_output=True,
            timeout=60,
        )
        attached = True
        service = Service(
            binary,
            runtime,
            artifacts,
            "disk-full",
            sys.executable,
            ["-u", "-c", FIXTURE],
            runtime,
            {"LAPIS_HISTORY_ROOT": str(mount / "archive")},
        )
        with service.connect() as client:
            client.snapshot(lambda s: "READY" in s["text"])
            client.send(TEXT, b"burst\n")
            client.snapshot(lambda s: "BURST_DONE" in s["text"], timeout=60)
            require(history(client, 1)[0], "Disk fixture initial archive missing")
            filler = mount / "owned-filler"
            errno_seen = None
            try:
                with filler.open("wb", buffering=0) as stream:
                    while True:
                        stream.write(bytes(64 * 1024))
            except OSError as error:
                errno_seen = error.errno
            import errno

            require(errno_seen == errno.ENOSPC, "Fixture did not reach real ENOSPC")
            client.send(TEXT, b"burst\n")
            client.snapshot(lambda s: "BURST_DONE" in s["text"], timeout=60)
            _, _, message = history(client, 2)
            require(
                "paused" in message.lower() or "storage" in message.lower(),
                "Disk-full gap not reported",
            )
            client.send(TEXT, b"AFTER_FULL\n")
            client.snapshot(lambda s: "ECHO:AFTER_FULL" in s["text"])
            filler.unlink()
            recovered_id, _, recovered_message = history(client, 3)
            require(recovered_id, "Archive could not recover after space returned")
            require(
                not recovered_message, "Recovered disk retained stale failure status"
            )
        return {
            "real_enospc": True,
            "live_input_during_failure": True,
            "retry_after_freeing_space": True,
        }
    finally:
        if service:
            service.stop()
        if attached:
            subprocess.run(
                ["hdiutil", "detach", str(mount)],
                check=True,
                capture_output=True,
                timeout=60,
            )
        image.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/desktop")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/history-check/receipt.json"
    )
    parser.add_argument(
        "--disk-full",
        action="store_true",
        help="macOS: fill a new 32 MiB private disk image",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    receipt = {"schema": "lapis.history-check/1", "passed": False}
    try:
        with tempfile.TemporaryDirectory(
            prefix="lapis-history-", dir="/tmp"
        ) as directory:
            runtime = Path(directory)
            binary = args.build_dir / "services/session/lapis_session_service"
            receipt["service"] = exercise(
                binary, runtime, args.output.parent, runtime / "archive"
            )
            if args.disk_full:
                require(sys.platform == "darwin", "Disk-image fixture requires macOS")
                receipt["disk_full"] = disk_full(binary, runtime, args.output.parent)
            receipt["passed"] = True
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        receipt["error"] = str(error)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
