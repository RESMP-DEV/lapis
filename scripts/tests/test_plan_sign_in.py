"""The plan sign-in helper: Claude Code's link is passed on rather than opened,
and the token it prints is kept owner-only, never shown."""

import json
import stat
import subprocess
import sys
import tempfile
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
        folder = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", str(folder)], check=False))
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


if __name__ == "__main__":
    unittest.main()
