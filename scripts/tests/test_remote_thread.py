"""lapis's remote thread helper (apps/desktop/src/remote_thread.py): the frames
it writes, the identity check on resume, the tail cut at a line start, and
following a file with inotify or, without it, by polling."""

import argparse
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "apps" / "desktop" / "src" / "remote_thread.py"
sys.path.insert(0, str(SCRIPT.parent))
import remote_thread  # noqa: E402

CONVERSATION = "5e550000-0000-4000-8000-000000000001"


def line(n):
    return (
        '{"type":"user","uuid":"u%d","message":{"content":"p%d"}}\n' % (n, n)
    ).encode()


def follow_args(**overrides):
    values = dict(
        command="follow",
        cli="claude",
        conversation=CONVERSATION,
        folder="",
        offset=0,
        head="",
        inode=0,
        tail_bytes=0,
    )
    values.update(overrides)
    return argparse.Namespace(**values)


class Sink(io.RawIOBase):
    """A stdout that keeps what was written, safe across threads."""

    def __init__(self):
        self.data = bytearray()
        self.lock = threading.Lock()

    def writable(self):
        return True

    def write(self, data):
        with self.lock:
            self.data += data
        return len(data)

    def frames(self):
        with self.lock:
            frames, _ = remote_thread.decode_frames(bytes(self.data))
        return frames


class Frames(unittest.TestCase):
    def test_round_trip(self):
        encoded = (
            remote_thread.json_frame(b"H", {"size": 3})
            + remote_thread.chunk_frame(b"D", 2**40, b"abc")
            + remote_thread.frame(b"K")
        )
        frames, rest = remote_thread.decode_frames(encoded + b"T\x00\x00")
        self.assertEqual(rest, b"T\x00\x00")
        self.assertEqual([kind for kind, _ in frames], [b"H", b"D", b"K"])
        self.assertEqual(json.loads(frames[0][1]), {"size": 3})
        self.assertEqual(struct.unpack(">Q", frames[1][1][:8])[0], 2**40)
        self.assertEqual(frames[1][1][8:], b"abc")
        self.assertEqual(frames[2][1], b"")

    def test_header_is_type_and_big_endian_length(self):
        self.assertEqual(remote_thread.frame(b"D", b"xy"), b"D\x00\x00\x00\x02xy")


class Files(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_project_folder_and_fallback_search(self):
        home = self.root / "claude"
        folder = self.root / "work" / "my.proj"
        named = home / "projects" / remote_thread.project_folder(str(folder))
        named.mkdir(parents=True)
        (named / (CONVERSATION + ".jsonl")).write_bytes(line(1))
        found = remote_thread.find_transcript(CONVERSATION, str(folder), str(home))
        self.assertEqual(found, str(named / (CONVERSATION + ".jsonl")))
        # Elsewhere (another folder's project), the search still finds it.
        other = home / "projects" / "-elsewhere"
        other.mkdir()
        os.rename(found, other / (CONVERSATION + ".jsonl"))
        found = remote_thread.find_transcript(CONVERSATION, str(folder), str(home))
        self.assertEqual(found, str(other / (CONVERSATION + ".jsonl")))
        self.assertIsNone(
            remote_thread.find_transcript("0" * 8, str(folder), str(home))
        )

    def test_config_dir_is_honoured(self):
        self.assertEqual(remote_thread.claude_home({"CLAUDE_CONFIG_DIR": "/x"}), "/x")
        self.assertTrue(remote_thread.claude_home({}).endswith(".claude"))

    def test_tail_starts_at_a_line(self):
        path = self.root / "t.jsonl"
        data = b"".join(line(n) for n in range(1, 200))
        path.write_bytes(data)
        with open(path, "rb") as handle:
            start = remote_thread.tail_start(handle, len(data), 500)
            self.assertGreater(start, len(data) - 500)
            self.assertEqual(data[start - 1 : start], b"\n")
            self.assertEqual(
                remote_thread.tail_start(handle, len(data), len(data) + 1), 0
            )
        # One line longer than the tail asked: no line starts in it.
        long_line = b"x" * 1000 + b"\n"
        path.write_bytes(long_line)
        with open(path, "rb") as handle:
            self.assertEqual(
                remote_thread.tail_start(handle, len(long_line), 100), 1001
            )

    def test_identity_check(self):
        path = self.root / "t.jsonl"
        data = line(1) * 500
        path.write_bytes(data)
        head = hashlib.sha256(data[:4096]).hexdigest()
        with open(path, "rb") as handle:
            inode = os.fstat(handle.fileno()).st_ino
            check = remote_thread.identity_mismatch
            self.assertIsNone(check(handle, inode, len(data), 0, 0, ""))
            self.assertIsNone(check(handle, inode, len(data), 5000, inode, head))
            self.assertEqual(
                check(handle, inode, len(data), 5000, inode + 1, head), "replaced"
            )
            self.assertEqual(
                check(handle, inode, len(data), len(data) + 1, inode, head), "truncated"
            )
            self.assertEqual(
                check(handle, inode, len(data), 5000, inode, "0" * 64), "rewritten"
            )
            # A copy shorter than 4 KiB is checked against as many bytes.
            short = hashlib.sha256(data[:100]).hexdigest()
            self.assertIsNone(check(handle, inode, len(data), 100, 0, short))

    def test_polling_when_inotify_is_off(self):
        path = self.root / "t.jsonl"
        path.write_bytes(b"")
        watcher = remote_thread.Watcher(str(path), use_inotify=False)
        self.assertTrue(watcher.polling)
        started = time.monotonic()
        watcher.wait(5.0)
        self.assertLess(time.monotonic() - started, 1.0, "polling waits about 100 ms")
        watcher.close()

    @unittest.skipUnless(sys.platform.startswith("linux"), "inotify is Linux's")
    def test_inotify_wakes_on_append(self):
        path = self.root / "t.jsonl"
        path.write_bytes(b"")
        watcher = remote_thread.Watcher(str(path))
        self.assertFalse(watcher.polling)
        threading.Timer(0.2, lambda: path.open("ab").write(line(1))).start()
        started = time.monotonic()
        watcher.wait(5.0)
        self.assertLess(time.monotonic() - started, 2.0)
        watcher.close()


class Follow(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.home = Path(self.temp.name) / "claude"
        self.folder = self.home / "projects" / "-p"
        self.folder.mkdir(parents=True)
        self.path = self.folder / (CONVERSATION + ".jsonl")

    def tearDown(self):
        self.temp.cleanup()

    def run_follower(self, args, rounds):
        sink = Sink()
        follower = remote_thread.Follower(
            args, sink, use_inotify=False, home=str(self.home)
        )
        follower.run(rounds=rounds)
        return sink.frames()

    def data(self, frames, kind=b"D"):
        out = b""
        for got, payload in frames:
            if got == kind:
                (offset,) = struct.unpack(">Q", payload[:8])
                self.assertEqual(offset, len(out), "frames arrive in order")
                out += payload[8:]
        return out

    def test_sends_hello_then_the_file(self):
        content = b"".join(line(n) for n in range(1, 50))
        self.path.write_bytes(content)
        frames = self.run_follower(follow_args(), rounds=3)
        self.assertEqual(frames[0][0], b"H")
        hello = json.loads(frames[0][1])
        self.assertEqual(hello["size"], len(content))
        self.assertEqual(hello["head"], hashlib.sha256(content[:4096]).hexdigest())
        self.assertEqual(self.data(frames), content)

    def test_resumes_at_the_offset(self):
        content = b"".join(line(n) for n in range(1, 50))
        self.path.write_bytes(content)
        inode = os.stat(self.path).st_ino
        head = hashlib.sha256(content[:1000][:4096]).hexdigest()
        frames = self.run_follower(
            follow_args(offset=1000, head=head, inode=inode), rounds=3
        )
        self.assertNotIn(b"R", [kind for kind, _ in frames])
        sent = [p for kind, p in frames if kind == b"D"]
        self.assertEqual(struct.unpack(">Q", sent[0][:8])[0], 1000)
        self.assertEqual(b"".join(p[8:] for p in sent), content[1000:])

    def test_a_changed_file_resets(self):
        content = b"".join(line(n) for n in range(1, 50))
        self.path.write_bytes(content)
        frames = self.run_follower(follow_args(offset=1000, head="0" * 64), rounds=3)
        kinds = [kind for kind, _ in frames]
        self.assertEqual(kinds[:3], [b"H", b"R", b"H"])
        self.assertEqual(json.loads(frames[1][1])["reason"], "rewritten")
        self.assertEqual(self.data(frames), content)

    def test_large_file_sends_its_tail_first(self):
        content = b"".join(line(n) for n in range(1, 100000))
        self.assertGreater(len(content), remote_thread.TAIL_THRESHOLD)
        self.path.write_bytes(content)
        # One round a chunk, and one more to see nothing is left.
        rounds = len(content) // remote_thread.CHUNK + 2
        frames = self.run_follower(follow_args(tail_bytes=1 << 20), rounds=rounds)
        kinds = [kind for kind, _ in frames]
        self.assertEqual(kinds[0], b"H")
        self.assertEqual(kinds[1], b"T", "the end comes before the rest")
        tail = b""
        start = None
        for kind, payload in frames:
            if kind == b"T":
                (offset,) = struct.unpack(">Q", payload[:8])
                start = offset if start is None else start
                self.assertEqual(offset, start + len(tail))
                tail += payload[8:]
        self.assertEqual(content[start - 1 : start], b"\n", "cut at a line start")
        self.assertEqual(content[start:], tail)
        self.assertEqual(self.data(frames), content, "then the whole file in order")

    def test_follows_appends_and_truncation(self):
        self.path.write_bytes(line(1))
        sink = Sink()
        follower = remote_thread.Follower(
            follow_args(), sink, use_inotify=False, home=str(self.home)
        )
        thread = threading.Thread(
            target=follower.run, kwargs={"rounds": 25}, daemon=True
        )
        thread.start()
        time.sleep(0.3)
        with open(self.path, "ab") as handle:
            handle.write(line(2))
        time.sleep(0.5)
        with open(self.path, "wb") as handle:
            handle.write(b"{}\n")
        thread.join(15)
        frames = sink.frames()
        kinds = [kind for kind, _ in frames]
        self.assertIn(b"R", kinds)
        before = frames[: kinds.index(b"R")]
        self.assertEqual(self.data(before), line(1) + line(2))
        after = frames[kinds.index(b"R") :]
        self.assertEqual(json.loads(after[0][1])["reason"], "truncated")
        self.assertEqual(after[1][0], b"H")
        self.assertEqual(self.data(after), b"{}\n")

    def test_missing_file_is_waited_for(self):
        sink = Sink()
        follower = remote_thread.Follower(
            follow_args(), sink, use_inotify=False, home=str(self.home)
        )
        thread = threading.Thread(
            target=follower.run, kwargs={"rounds": 5}, daemon=True
        )
        thread.start()
        time.sleep(0.3)
        self.assertEqual([kind for kind, _ in sink.frames()], [b"M"])
        self.path.write_bytes(line(1))
        thread.join(10)
        self.assertEqual(self.data(sink.frames()), line(1))


class Script(unittest.TestCase):
    """The script as lapis runs it: on standard input, nothing written."""

    def test_bad_conversation_is_an_error_frame(self):
        result = subprocess.run(
            [
                sys.executable,
                "-",
                "follow",
                "--cli",
                "claude",
                "--conversation",
                "../x",
            ],
            input=SCRIPT.read_bytes(),
            capture_output=True,
            timeout=30,
        )
        frames, _ = remote_thread.decode_frames(result.stdout)
        self.assertEqual(frames[0][0], b"E")
        self.assertEqual(json.loads(frames[0][1])["reason"], "bad conversation")
        self.assertNotEqual(result.returncode, 0)

    def test_follow_from_stdin(self):
        with tempfile.TemporaryDirectory() as home:
            folder = Path(home) / "projects" / "-x"
            folder.mkdir(parents=True)
            (folder / (CONVERSATION + ".jsonl")).write_bytes(line(1))
            before = sorted(str(p) for p in Path(home).rglob("*"))
            process = subprocess.Popen(
                [
                    sys.executable,
                    "-",
                    "follow",
                    "--cli",
                    "claude",
                    "--conversation",
                    CONVERSATION,
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                env=dict(os.environ, CLAUDE_CONFIG_DIR=home, HOME=home),
            )
            process.stdin.write(SCRIPT.read_bytes())
            process.stdin.close()
            data = b""
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                data += os.read(process.stdout.fileno(), 65536)
                frames, _ = remote_thread.decode_frames(data)
                if any(kind == b"D" for kind, _ in frames):
                    break
            process.kill()
            process.wait(10)
            process.stdout.close()
            kinds = [kind for kind, _ in frames]
            self.assertEqual(kinds[:2], [b"H", b"D"])
            self.assertEqual(before, sorted(str(p) for p in Path(home).rglob("*")))


if __name__ == "__main__":
    unittest.main()
