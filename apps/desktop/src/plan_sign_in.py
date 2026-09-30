"""Signs a Claude Code plan in for lapis, whichever account the browser uses.

    python3 plan_sign_in.py --token-file PATH [--claude PATH]

Runs `claude setup-token` on a terminal of its own with the browser left
closed: the sign-in link Claude Code would open is printed instead, as one JSON
line {"link": URL}, for lapis to copy and show. Whoever signs in there (any
email), Claude Code prints a long-lived token for that account; this writes it
to --token-file (owner-only, replaced whole) and prints {"signedIn": true}. On
failure it prints {"error": REASON}. The token itself is never printed.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import pty
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

TOKEN = re.compile(rb"sk-ant-oat01-[A-Za-z0-9_-]{20,}")
ESCAPES = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\x1b[()][A-Z0-9]")
LINK = re.compile(r"https://\S+/oauth/authorize\?\S+")
WAIT_SECONDS = 15 * 60


def say(**message):
    print(json.dumps(message), flush=True)


def keep(token: bytes, path: str) -> None:
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, mode=0o700, exist_ok=True)
    os.chmod(folder, 0o700)
    handle, temporary = tempfile.mkstemp(dir=folder, prefix=".signing-in-")
    try:
        with os.fdopen(handle, "wb") as out:
            out.write(token + b"\n")
        os.replace(temporary, path)
    except OSError:
        os.unlink(temporary)
        raise


def last_words(seen: bytes) -> str:
    """The end of what Claude Code printed, readable and without a token."""
    text = TOKEN.sub(b"[token]", ESCAPES.sub(b"", seen)).decode(errors="replace")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return (lines[-1] if lines else "claude setup-token stopped")[:300]


def sign_in(claude: str, token_file: str) -> int:
    with tempfile.TemporaryDirectory(prefix="lapis-sign-in-") as folder:
        # Claude Code opens its link with `open` (or $BROWSER): both write it
        # here instead, so the person chooses the browser and the account.
        links = os.path.join(folder, "links")
        opener = os.path.join(folder, "open")
        with open(opener, "w") as script:
            script.write(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> '{links}'\n")
        os.chmod(opener, 0o700)
        env = dict(
            os.environ,
            BROWSER=opener,
            PATH=folder + os.pathsep + os.environ.get("PATH", ""),
        )
        pid, fd = pty.fork()
        if pid == 0:
            try:
                # Wide enough that neither the link nor the token wraps.
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 1000, 0, 0))
                os.execvpe(claude, [claude, "setup-token"], env)
            finally:
                os._exit(127)
        seen = b""
        shown = False
        deadline = time.monotonic() + WAIT_SECONDS
        try:
            while time.monotonic() < deadline:
                if not shown:
                    try:
                        with open(links) as opened:
                            for line in opened:
                                found = LINK.search(line)
                                if found:
                                    say(link=found.group(0))
                                    shown = True
                                    break
                    except OSError:
                        pass
                ready, _, _ = select.select([fd], [], [], 0.25)
                if not ready:
                    continue
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    data = b""
                if not data:
                    break
                seen = (seen + data)[-262144:]
                if not shown:
                    found = LINK.search(ESCAPES.sub(b"", seen).decode(errors="replace"))
                    if found:
                        say(link=found.group(0))
                        shown = True
                # The token ends at the end of its line.
                token = TOKEN.search(ESCAPES.sub(b"", seen))
                if token and re.search(
                    rb"[\r\n]", ESCAPES.sub(b"", seen)[token.end() :]
                ):
                    keep(token.group(0), token_file)
                    say(signedIn=True)
                    return 0
            else:
                say(error="no sign-in within 15 minutes")
                return 1
        finally:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(pid, 0)
            os.close(fd)
    say(error=last_words(seen))
    return 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--claude", default=shutil.which("claude") or "claude")
    arguments = parser.parse_args(argv)
    # lapis ends a sign-in with SIGTERM; the finally below then ends Claude
    # Code's too.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    try:
        return sign_in(arguments.claude, arguments.token_file)
    except OSError as error:
        say(error=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
