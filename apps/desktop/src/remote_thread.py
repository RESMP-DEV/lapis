#!/usr/bin/env python3
"""Mirrors one Claude Code conversation file from this machine to lapis.

lapis sends this script over ssh on standard input (`python3 - follow ...`),
so nothing is installed or written here: it only reads the conversation's
JSONL file and writes frames to standard output, then exits when the
connection closes. Standard library only; Python 3.6 or later.

Every frame is one type byte, a big-endian 32-bit payload length, then the
payload:

  H hello    JSON {version, found, size, inode, head}: the file now, head
             being the SHA-256 of its first 4 KiB (hex)
  T tail     8-byte big-endian offset, then bytes: the file's end, cut at a
             line start, sent first when the client has nothing (or an
             incomplete copy) and the file is large; later tail frames carry
             what the CLI appends while the rest is still on its way
  D data     8-byte big-endian offset, then bytes: the file in order from the
             client's offset, then whatever the CLI appends
  R reset    JSON {reason}: the client's copy no longer matches (the file was
             replaced, truncated or rewritten); a hello for the file as it
             is now follows, and data starts again at 0
  M missing  JSON {}: no such file yet; the helper keeps looking
  E error    JSON {reason}: the helper cannot continue
  K alive    empty: sent when idle, so a closed connection is noticed
"""

import argparse
import ctypes
import ctypes.util
import glob
import hashlib
import json
import os
import re
import select
import struct
import sys
import time

VERSION = 1
HEAD_BYTES = 4096
CHUNK = 256 * 1024
TAIL_THRESHOLD = 4 * 1024 * 1024
MAX_TAIL = 4 * 1024 * 1024
WAIT_POLL = 0.5
POLL = 0.1
ALIVE_EVERY = 10.0
UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")


def frame(kind, payload=b""):
    """One frame: type byte, payload length, payload."""
    return kind + struct.pack(">I", len(payload)) + payload


def json_frame(kind, value):
    return frame(kind, json.dumps(value, separators=(",", ":")).encode())


def chunk_frame(kind, offset, data):
    return frame(kind, struct.pack(">Q", offset) + data)


def decode_frames(buffer):
    """The complete frames in `buffer` as (type, payload), and what is left."""
    frames = []
    at = 0
    while len(buffer) - at >= 5:
        (length,) = struct.unpack(">I", buffer[at + 1 : at + 5])
        if len(buffer) - at - 5 < length:
            break
        frames.append((buffer[at : at + 1], buffer[at + 5 : at + 5 + length]))
        at += 5 + length
    return frames, buffer[at:]


def project_folder(path):
    """The folder Claude Code keeps a project's conversations in."""
    return re.sub(r"[^A-Za-z0-9]", "-", path)


def claude_home(environ=None):
    environ = os.environ if environ is None else environ
    configured = environ.get("CLAUDE_CONFIG_DIR", "")
    return configured or os.path.join(os.path.expanduser("~"), ".claude")


def find_transcript(conversation, folder, home=None):
    """The conversation's transcript: in the project folder named after
    `folder` first, then in any project folder; None when not saved yet."""
    projects = os.path.join(home or claude_home(), "projects")
    name = conversation + ".jsonl"
    if folder:
        cwd = os.path.abspath(os.path.expanduser(folder))
        for candidate in (cwd, os.path.realpath(cwd)):
            path = os.path.join(projects, project_folder(candidate), name)
            if os.path.isfile(path):
                return path
    for path in sorted(glob.glob(os.path.join(glob.escape(projects), "*", name))):
        if os.path.isfile(path):
            return path
    return None


def head_hash(handle, length):
    """SHA-256 (hex) of the file's first min(length, 4 KiB) bytes."""
    handle.seek(0)
    return hashlib.sha256(handle.read(min(length, HEAD_BYTES))).hexdigest()


def tail_start(handle, size, want):
    """Where the last `want` bytes begin, moved on to the next line start;
    `size` when no line starts in that range."""
    start = max(0, size - want)
    if start == 0:
        return 0
    handle.seek(start - 1)
    at = start - 1
    while at < size:
        block = handle.read(min(65536, size - at))
        if not block:
            break
        newline = block.find(b"\n")
        if newline >= 0:
            return at + newline + 1
        at += len(block)
    return size


def identity_mismatch(handle, inode, size, offset, known_inode, known_head):
    """Why a copy of `offset` bytes no longer matches the file, or None."""
    if offset <= 0:
        return None
    if known_inode and known_inode != inode:
        return "replaced"
    if size < offset:
        return "truncated"
    if known_head and head_hash(handle, offset) != known_head:
        return "rewritten"
    return None


class Watcher:
    """Waits for the file to change: inotify on Linux, else stat polling."""

    IN_MODIFY = 0x002
    IN_ATTRIB = 0x004
    IN_CLOSE_WRITE = 0x008
    IN_MOVE_SELF = 0x800
    IN_DELETE_SELF = 0x400
    IN_NONBLOCK = 0o4000
    IN_CLOEXEC = 0o2000000

    def __init__(self, path, use_inotify=True):
        self.fd = -1
        if use_inotify and sys.platform.startswith("linux"):
            try:
                libc = ctypes.CDLL(
                    ctypes.util.find_library("c") or "libc.so.6", use_errno=True
                )
                fd = libc.inotify_init1(self.IN_NONBLOCK | self.IN_CLOEXEC)
                if fd >= 0:
                    mask = (
                        self.IN_MODIFY
                        | self.IN_ATTRIB
                        | self.IN_CLOSE_WRITE
                        | self.IN_MOVE_SELF
                        | self.IN_DELETE_SELF
                    )
                    if libc.inotify_add_watch(fd, os.fsencode(path), mask) >= 0:
                        self.fd = fd
                    else:
                        os.close(fd)
            except (OSError, AttributeError):
                self.fd = -1

    @property
    def polling(self):
        return self.fd < 0

    def wait(self, timeout):
        if self.fd < 0:
            time.sleep(min(timeout, POLL))
            return
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if ready:
            try:
                while os.read(self.fd, 65536):
                    pass
            except (BlockingIOError, InterruptedError):
                pass

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


class Follower:
    def __init__(self, args, out, use_inotify=True, home=None):
        self.args = args
        self.out = out
        self.use_inotify = use_inotify
        self.home = home
        self.last_sent = time.monotonic()

    def send(self, data):
        self.out.write(data)
        self.out.flush()
        self.last_sent = time.monotonic()

    def alive(self):
        if time.monotonic() - self.last_sent >= ALIVE_EVERY:
            self.send(frame(b"K"))

    def find(self):
        """Waits for the transcript to exist; returns its path."""
        told = False
        while True:
            path = find_transcript(self.args.conversation, self.args.folder, self.home)
            if path:
                return path
            if not told:
                self.send(json_frame(b"M", {}))
                told = True
            self.alive()
            time.sleep(WAIT_POLL)

    def hello(self, handle, inode):
        size = os.fstat(handle.fileno()).st_size
        self.send(
            json_frame(
                b"H",
                {
                    "version": VERSION,
                    "found": True,
                    "size": size,
                    "inode": inode,
                    "head": head_hash(handle, size),
                },
            )
        )

    def reset(self, handle, inode, why):
        """The client's copy is void: a reset, then the file as it is now."""
        self.send(json_frame(b"R", {"reason": why}))
        self.hello(handle, inode)

    def send_tail(self, handle, start, end):
        while start < end:
            handle.seek(start)
            data = handle.read(min(CHUNK, end - start))
            if not data:
                break
            self.send(chunk_frame(b"T", start, data))
            start += len(data)
        return start

    def run(self, rounds=None):
        offset = self.args.offset
        known_inode = self.args.inode
        known_head = self.args.head
        while True:
            path = self.find()
            try:
                handle = open(path, "rb")
            except OSError:
                time.sleep(WAIT_POLL)
                continue
            with handle:
                info = os.fstat(handle.fileno())
                size, inode = info.st_size, info.st_ino
                self.hello(handle, inode)
                why = identity_mismatch(
                    handle, inode, size, offset, known_inode, known_head
                )
                if why:
                    self.reset(handle, inode, why)
                    offset = 0
                offset = self.stream(handle, path, inode, offset, rounds)
                if offset is None:
                    return
            # The file went away or was replaced: start over with the next one.
            known_inode = inode
            known_head = None

    def stream(self, handle, path, inode, offset, rounds):
        """Sends the file from `offset` and follows it. Returns the offset when
        the file was replaced or removed, None when `rounds` ran out."""
        watcher = Watcher(path, self.use_inotify)
        try:
            tail_end = None
            size = os.fstat(handle.fileno()).st_size
            if self.args.tail_bytes > 0 and size - offset > TAIL_THRESHOLD:
                start = tail_start(handle, size, min(self.args.tail_bytes, MAX_TAIL))
                if offset < start < size:
                    tail_end = self.send_tail(handle, start, size)
            while rounds is None or rounds > 0:
                if rounds is not None:
                    rounds -= 1
                try:
                    current = os.stat(path)
                except OSError:
                    return offset
                if current.st_ino != inode:
                    self.send(json_frame(b"R", {"reason": "replaced"}))
                    return 0
                size = os.fstat(handle.fileno()).st_size
                if size < offset:
                    self.reset(handle, inode, "truncated")
                    offset = 0
                    tail_end = None
                    continue
                if tail_end is not None and size > tail_end:
                    tail_end = self.send_tail(handle, tail_end, size)
                if offset < size:
                    handle.seek(offset)
                    data = handle.read(min(CHUNK, size - offset))
                    if data:
                        self.send(chunk_frame(b"D", offset, data))
                        offset += len(data)
                    if tail_end is not None and offset >= tail_end:
                        tail_end = None
                    continue
                self.alive()
                watcher.wait(1.0)
            return None
        finally:
            watcher.close()


def arguments(argv):
    parser = argparse.ArgumentParser(prog="remote_thread")
    commands = parser.add_subparsers(dest="command")
    follow = commands.add_parser("follow")
    follow.add_argument("--cli", required=True)
    follow.add_argument("--conversation", required=True)
    follow.add_argument("--folder", default="")
    follow.add_argument("--offset", type=int, default=0)
    follow.add_argument("--head", default="")
    follow.add_argument("--inode", type=int, default=0)
    follow.add_argument("--tail-bytes", type=int, default=0)
    return parser.parse_args(argv)


def main(argv=None):
    out = sys.stdout.buffer
    try:
        args = arguments(sys.argv[1:] if argv is None else argv)
    except SystemExit:
        out.write(json_frame(b"E", {"reason": "bad arguments"}))
        return 2
    try:
        if sys.version_info < (3, 6):
            out.write(json_frame(b"E", {"reason": "python3 is older than 3.6"}))
            return 3
        if args.command != "follow" or args.cli != "claude":
            out.write(json_frame(b"E", {"reason": "only Claude Code is mirrored"}))
            return 2
        if not UUID.match(args.conversation) or args.offset < 0:
            out.write(json_frame(b"E", {"reason": "bad conversation"}))
            return 2
        Follower(args, out).run()
    except (BrokenPipeError, KeyboardInterrupt):
        return 0
    except OSError as error:
        try:
            out.write(json_frame(b"E", {"reason": "read failed: %s" % error.strerror}))
            out.flush()
        except OSError:
            pass
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
