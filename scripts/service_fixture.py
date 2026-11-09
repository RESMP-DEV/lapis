"""Shared helpers for checks that drive a disposable session service."""

import asyncio
import json
import socket
import struct
import subprocess

if __package__:
    from .check_cli_launch import (
        ATTENTION_RETRY,
        ATTENTION_SNAPSHOT,
        SNAPSHOT,
        STATUS,
        VERSION,
        CheckError,
        FrameDeadline,
        decode_attention_snapshot,
        decode_snapshot,
        require,
    )
else:
    from check_cli_launch import (
        ATTENTION_RETRY,
        ATTENTION_SNAPSHOT,
        SNAPSHOT,
        STATUS,
        VERSION,
        CheckError,
        FrameDeadline,
        decode_attention_snapshot,
        decode_snapshot,
        require,
    )


def string(value):
    data = value.encode("utf-8")
    return struct.pack(">I", len(data)) + data


def decision(request, choice, answers=None, revision=None):
    """An attention decision payload, as an older desktop sent one."""
    identifier = request["id"]
    identity = (
        b"\0" + struct.pack(">q", identifier)
        if type(identifier) is int
        else b"\1" + string(identifier)
    )
    payload = (
        struct.pack(">IQ", VERSION, request["epoch"])
        + identity
        + struct.pack(">Q", request["revision"] if revision is None else revision)
        + string(choice)
        + string(
            json.dumps(
                answers or {}, ensure_ascii=False, sort_keys=True, separators=(",", ":")
            )
        )
    )
    require(len(payload) <= 65536, "Oversized decision fixture")
    return payload


class View:
    """Consume terminal and attention frames without blocking the caller."""

    def __init__(self, client):
        self.client = client
        self.attention = None
        self.retry = None
        self.screen = (client.cached_snapshot or {}).get("text", "")
        self.running = True
        self.task = asyncio.create_task(self.pump())

    async def pump(self):
        while self.running:
            try:
                kind, data = await asyncio.to_thread(self.client.receive, 0.2)
            except (socket.timeout, FrameDeadline):
                continue
            if kind == ATTENTION_SNAPSHOT:
                value = decode_attention_snapshot(data)
                require(
                    value["attachment"] == self.client.attachment,
                    "Attention attachment mismatch",
                )
                self.attention = value
            elif kind == SNAPSHOT:
                require(
                    data[:40] == self.client.attachment, "Terminal attachment mismatch"
                )
                self.screen = decode_snapshot(data[72:])["text"]
            elif kind == ATTENTION_RETRY:
                require(
                    data[:40] == self.client.attachment, "Rejection attachment mismatch"
                )
                self.retry = data[40:]
            elif kind == STATUS:
                raise RuntimeError(
                    "Service returned status: " + data[1:].decode("utf-8")
                )
            else:
                raise RuntimeError("Unexpected service frame")

    async def wait(self, predicate, timeout=90):
        async with asyncio.timeout(timeout):
            while not predicate():
                if self.task.done():
                    await self.task
                if self.attention and not self.attention["connected"]:
                    raise RuntimeError(self.attention["diagnostic"])
                await asyncio.sleep(0.02)

    async def close(self):
        self.running = False
        try:
            await self.task
        finally:
            self.client.close()


def process_groups(parent=None):
    rows = subprocess.check_output(
        ["ps", "-axo", "pid=,ppid=,pgid="], text=True, timeout=5
    )
    values = [tuple(map(int, row.split())) for row in rows.splitlines()]
    return {
        group
        for pid, ppid, group in values
        if parent is None or (ppid == parent and pid == group)
    }


async def wait_groups_gone(groups):
    async with asyncio.timeout(5):
        while groups & await asyncio.to_thread(process_groups):
            await asyncio.sleep(0.05)


async def cleanup_service(service, groups, receipt, original_error):
    errors = []
    try:
        await asyncio.to_thread(service.stop)
        receipt["service_reaped"] = service.process.poll() is not None
    except Exception as error:
        errors.append(f"service cleanup: {str(error) or type(error).__name__}")
    if groups:
        try:
            await wait_groups_gone(groups)
            receipt["owned_process_groups_cleaned"] = True
        except Exception as error:
            errors.append(
                f"process-group cleanup: {str(error) or type(error).__name__}"
            )
    if errors:
        receipt["cleanup_errors"] = errors
        if original_error is None:
            raise CheckError("; ".join(errors))
        for error in errors:
            original_error.add_note(error)
