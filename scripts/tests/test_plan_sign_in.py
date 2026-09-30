"""The plan sign-in helper: Claude Code's link is passed on rather than opened,
and the token it prints is kept owner-only, never shown."""

import json
import os
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "apps" / "desktop" / "src" / "plan_sign_in.py"
LINK = "https://claude.com/cai/oauth/authorize?code=true&client_id=x&state=y"
TOKEN = "sk-ant-oat01-" + "A" * 40


def stand_in(folder: Path, body: str) -> Path:
    path = folder / "claude"
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o700)
    return path


class SignInTests(unittest.TestCase):
    def run_helper(self, body):
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        claude = stand_in(folder, body)
        token = folder / "accounts" / "claude" / ".signing-in.token"
        result = subprocess.run(
            [
                sys.executable,
                str(HELPER),
                "--token-file",
                str(token),
                "--claude",
                str(claude),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        lines = [
            json.loads(line) for line in result.stdout.splitlines() if line.strip()
        ]
        return result, lines, token

    def start_helper(self, token: Path, claude: Path) -> subprocess.Popen[str]:
        process = subprocess.Popen(
            [
                sys.executable,
                str(HELPER),
                "--token-file",
                str(token),
                "--claude",
                str(claude),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        def cleanup() -> None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for stream in (process.stdout, process.stderr):
                if stream is not None and not stream.closed:
                    stream.close()

        self.addCleanup(cleanup)
        return process

    def first_message(self, process: subprocess.Popen[str]) -> dict[str, object]:
        assert process.stdout is not None
        deadline = time.monotonic() + 10
        line = bytearray()
        while len(line) < 65536:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, "helper did not print its first message")
            ready, _, _ = select.select([process.stdout], [], [], remaining)
            self.assertTrue(ready, "helper did not print its first message")
            chunk = os.read(process.stdout.fileno(), 1)
            self.assertTrue(chunk, "helper closed stdout before its first message")
            line += chunk
            if chunk == b"\n":
                if line.strip():
                    return json.loads(line)
                line.clear()
        self.fail("helper message exceeded its bound")

    def test_the_link_is_passed_on_and_the_token_kept(self):
        result, lines, token = self.run_helper(
            f'test "$1" = setup-token || exit 3\nopen "{LINK}"\nsleep 1\n'
            f"printf 'Your OAuth token:\\r\\n\\r\\n{TOKEN}\\r\\n'\nsleep 30\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines, [{"link": LINK}, {"signedIn": True}])
        self.assertEqual(token.read_text(), TOKEN + "\n")
        self.assertEqual(stat.S_IMODE(token.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(token.parent.stat().st_mode), 0o700)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)

    def test_a_sign_in_that_stops_says_why_without_a_token(self):
        result, lines, token = self.run_helper(
            f"open \"{LINK}\"\nprintf 'OAuth error: denied\\r\\n'\nexit 1\n"
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(lines[0], {"link": LINK})
        self.assertEqual(lines[-1], {"error": "OAuth error: denied"})
        self.assertFalse(token.exists())

    def test_anthropic_link_is_cleaned_and_other_hosts_are_refused(self):
        quoted = f"'\"{LINK}'."
        result, lines, token = self.run_helper(
            f"open {quoted}\nprintf 'OAuth error: denied\\r\\n'\nexit 1\n"
        )
        self.assertEqual(lines[0], {"link": LINK}, result.stderr)
        self.assertEqual(lines[-1], {"error": "OAuth error: denied"})
        self.assertFalse(token.exists())

        foreign = "https://evil.example/oauth/authorize?code=true"
        result, lines, token = self.run_helper(
            f'open "{foreign}"\nprintf "OAuth error: denied\\r\\n"\nexit 1\n'
        )
        self.assertEqual(lines[0].get("link"), None)
        self.assertEqual(lines[-1], {"error": "OAuth error: denied"})
        self.assertFalse(token.exists())

    def test_a_complete_token_is_kept_at_end_of_stream(self):
        result, lines, token = self.run_helper(
            f'open "{LINK}"\nprintf "Token: {TOKEN}"\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines, [{"link": LINK}, {"signedIn": True}])
        self.assertEqual(token.read_text(), TOKEN + "\n")

    def test_a_split_token_is_not_kept_during_a_long_pause(self):
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        first = TOKEN[:-1]
        claude = stand_in(
            folder,
            f'open "{LINK}"\nprintf "{first}"\nsleep 2.5\nprintf "{TOKEN[-1]}\\n"\n',
        )
        token = folder / "accounts" / "claude" / ".signing-in.token"
        process = self.start_helper(token, claude)
        self.assertEqual(self.first_message(process), {"link": LINK})
        time.sleep(2.1)
        self.assertFalse(token.exists(), "an unterminated token prefix was kept")
        self.assertIsNone(process.poll(), "silence ended the sign-in early")
        process.wait(timeout=10)
        assert process.stdout is not None and process.stderr is not None
        process.stdout.close()
        process.stderr.close()
        self.assertEqual(process.returncode, 0)
        self.assertEqual(token.read_text(), TOKEN + "\n")

    def test_a_missing_executable_retains_the_system_reason(self):
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        claude = folder / "claude"
        claude.write_text("#!/missing-lapis-interpreter\n")
        claude.chmod(0o700)
        token = folder / "accounts" / "claude" / ".signing-in.token"
        result = subprocess.run(
            [
                sys.executable,
                str(HELPER),
                "--token-file",
                str(token),
                "--claude",
                str(claude),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        lines = [
            json.loads(line) for line in result.stdout.splitlines() if line.strip()
        ]
        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            lines[0].get("error", "").startswith("could not start claude setup-token:"),
            True,
        )
        self.assertIn("No such file or directory", lines[0]["error"])
        self.assertFalse(token.exists())

    def test_termination_cleans_up_without_keeping_a_token(self):
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        claude_pid_file = folder / "claude.pid"
        claude = stand_in(
            folder, f'echo $$ > "{claude_pid_file}"\nopen "{LINK}"\nsleep 30\n'
        )
        token = folder / "accounts" / "claude" / ".signing-in.token"
        process = self.start_helper(token, claude)
        self.assertEqual(self.first_message(process), {"link": LINK})
        claude_pid = int(claude_pid_file.read_text())
        process.terminate()
        process.wait(timeout=5)
        # The handler stops the wait loop; its finally ends and reaps Claude.
        self.assertEqual(process.returncode, 1)
        self.assertFalse(token.exists())
        deadline = time.monotonic() + 5
        while True:
            try:
                os.kill(claude_pid, 0)
            except OSError:
                break
            self.assertLess(time.monotonic(), deadline, "cancelled Claude survived")
            time.sleep(0.05)
        assert process.stdout is not None and process.stderr is not None
        process.stdout.close()
        process.stderr.close()

    def test_force_killed_helper_kills_the_claude_process_group(self):
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        claude_pid_file = folder / "claude.pid"
        claude = stand_in(
            folder, f'echo $$ > "{claude_pid_file}"\nopen "{LINK}"\nsleep 30\n'
        )
        token = folder / "accounts" / "claude" / ".signing-in.token"
        helper = self.start_helper(token, claude)
        self.assertEqual(self.first_message(helper), {"link": LINK})
        claude_pid = int(claude_pid_file.read_text())
        os.kill(helper.pid, signal.SIGKILL)
        helper.wait(timeout=5)
        assert helper.stdout is not None and helper.stderr is not None
        helper.stdout.close()
        helper.stderr.close()
        deadline = time.monotonic() + 5
        while True:
            try:
                os.kill(claude_pid, 0)
            except OSError:
                break
            self.assertLess(time.monotonic(), deadline, "orphaned Claude survived")
            time.sleep(0.05)
        self.assertFalse(token.exists())


if __name__ == "__main__":
    unittest.main()
