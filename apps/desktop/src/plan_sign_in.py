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
import stat
import struct
import sys
import tempfile
import termios
import time
from urllib.parse import urlsplit

TOKEN = re.compile(
    rb"(?<![A-Za-z0-9_-])sk-ant-oat01-[A-Za-z0-9_-]{20,}(?![A-Za-z0-9_-])"
)
ESCAPES = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\x1b[()][A-Z0-9]")
LINK = re.compile(r"https://\S+/oauth/authorize\?\S+")
WAIT_SECONDS = 15 * 60
_MAX_LINK_FILE_BYTES = 1024 * 1024
_MAX_LINK_LINE_BYTES = 8 * 1024
_MAX_SEEN_BYTES = 256 * 1024
_LINK_HOSTS = ("claude.com", "claude.ai", "anthropic.com")
STOPPING = False


def say(**message):
    print(json.dumps(message), flush=True)


def keep(token: bytes, path: str) -> None:
    folder = os.path.dirname(os.path.abspath(path))
    # makedirs only applies its mode to the deepest directory it creates, so
    # walk the missing chain explicitly: every directory this sign-in creates
    # is owner-only from the first moment it exists, while pre-existing
    # directories (however permissive) are left exactly as found.
    missing: list[str] = []
    unfound = folder
    while not os.path.exists(unfound):
        missing.append(unfound)
        parent = os.path.dirname(unfound)
        if parent == unfound:
            break
        unfound = parent
    for directory in reversed(missing):
        try:
            os.mkdir(directory, 0o700)
        except FileExistsError:
            pass
    info = os.stat(folder, follow_symlinks=False)
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & 0o077
    ):
        raise PermissionError(
            "credential directory must be private; existing permissions were left unchanged"
        )
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


def complete_token(cleaned: bytes, at_end_of_stream: bool) -> re.Match[bytes] | None:
    """A token followed by whitespace, or by the stream's actual end."""
    for found in TOKEN.finditer(cleaned):
        rest = cleaned[found.end() :]
        if at_end_of_stream and (not rest or rest.isspace()):
            return found
        if not at_end_of_stream and rest[:1].isspace():
            return found
    return None


def start_guard(guard_read: int, error_write: int) -> None:
    """Called inside the PTY child, before exec, to anchor that process group.

    The helper alone holds the write end. Its death wakes this guardian, which
    signals its own group, never a saved PID that might have been reused.
    """
    if os.fork() != 0:
        os.close(guard_read)
        return
    group = os.getpgrp()
    for descriptor in (0, 1, 2, error_write):
        if descriptor != guard_read:
            try:
                os.close(descriptor)
            except OSError:
                pass
    try:
        while os.read(guard_read, 512):
            pass
    except OSError:
        pass
    finally:
        os.close(guard_read)
    os.killpg(group, signal.SIGKILL)
    os._exit(0)


def stop_child(pid: int, guard_write: int, fd: int | None) -> None:
    if guard_write >= 0:
        os.close(guard_write)
    if pid > 0:
        # The direct child is still unreaped here, so its PID cannot be reused.
        # This also handles failure before the child could start its guardian.
        try:
            os.killpg(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            try:
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        try:
            os.waitpid(pid, 0)
        except (ChildProcessError, ProcessLookupError):
            pass
    if fd is not None and fd >= 0:
        os.close(fd)


def sign_in_link(text: str) -> str | None:
    """The first OAuth link Anthropic printed, without surrounding prose."""
    found = LINK.search(text)
    if found is None:
        return None
    # Claude Code can print the URL inside quotes or a sentence; punctuation
    # around it is not part of the URL.
    link = found.group(0).rstrip(".,;:!?)]}\"'")
    try:
        parsed = urlsplit(link)
        port = parsed.port
    except ValueError:
        return None
    host = (parsed.hostname or "").lower()
    allowed = any(
        host == candidate or host.endswith("." + candidate) for candidate in _LINK_HOSTS
    )
    if (
        parsed.scheme != "https"
        or not allowed
        or parsed.username is not None
        or parsed.password is not None
        or port is not None
        or not parsed.query
        or not parsed.path.endswith("/oauth/authorize")
    ):
        return None
    return link


def sign_in(claude: str, token_file: str) -> int:
    with tempfile.TemporaryDirectory(prefix="lapis-sign-in-") as folder:
        # Claude Code opens its link with `open` (or $BROWSER): both write it
        # here instead, so lapis can validate and present the link itself.
        links = os.path.join(folder, "links")
        opener = os.path.join(folder, "open")
        with open(opener, "w") as script:
            script.write(
                "#!/usr/bin/env python3\n"
                "import fcntl, os, sys\n"
                f"with open({links!r}, 'a+b') as out:\n"
                "    fcntl.flock(out, fcntl.LOCK_EX)\n"
                "    os.fchmod(out.fileno(), 0o600)\n"
                "    out.seek(0, 2)\n"
                f"    remaining = max(0, {_MAX_LINK_FILE_BYTES} - out.tell())\n"
                "    out.write((' '.join(sys.argv[1:]) + '\\n').encode()[:remaining])\n"
            )
        os.chmod(opener, 0o700)
        env = dict(
            os.environ,
            BROWSER=opener,
            PATH=folder + os.pathsep + os.environ.get("PATH", ""),
        )
        # The write end is non-inheritable: EOF means `exec` succeeded, while
        # a short message carries the exact failure from the forked child.
        error_read, error_write = os.pipe()
        pid = -1
        fd: int | None = None
        guard_read = guard_write = -1
        seen = b""
        try:
            guard_read, guard_write = os.pipe()
            try:
                pid, fd = pty.fork()
            except OSError:
                os.close(error_read)
                os.close(error_write)
                error_read = error_write = -1
                raise
            if pid == 0:
                os.close(error_read)
                os.close(guard_write)
                try:
                    start_guard(guard_read, error_write)
                    # Wide enough that neither the link nor the token wraps.
                    fcntl.ioctl(
                        0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 1000, 0, 0)
                    )
                    os.execvpe(claude, [claude, "setup-token"], env)
                except OSError as error:
                    try:
                        os.write(error_write, str(error).encode()[:4096])
                    finally:
                        os._exit(127)
                finally:
                    os._exit(127)
            os.close(guard_read)
            guard_read = -1
            os.close(error_write)
            error_write = -1
            try:
                launch_error = os.read(error_read, 4096)
            finally:
                os.close(error_read)
                error_read = -1
            if launch_error:
                say(
                    error="could not start claude setup-token: "
                    + launch_error.decode(errors="replace")
                )
                return 1

            shown = False
            links_handle = None
            links_buffer = b""
            links_read = 0
            deadline = time.monotonic() + WAIT_SECONDS
            try:
                while time.monotonic() < deadline:
                    if STOPPING:
                        break
                    if not shown:
                        if links_handle is None and links_read < _MAX_LINK_FILE_BYTES:
                            try:
                                links_handle = open(links, "rb")
                            except OSError:
                                pass
                        if links_handle is not None:
                            chunk = links_handle.read(8192)
                            links_read += len(chunk)
                            links_buffer = (links_buffer + chunk)[
                                -_MAX_LINK_LINE_BYTES:
                            ]
                            for raw in links_buffer.splitlines():
                                link = sign_in_link(raw.decode(errors="replace"))
                                if link is not None:
                                    say(link=link)
                                    shown = True
                                    break
                            if b"\n" in links_buffer:
                                links_buffer = links_buffer.rsplit(b"\n", 1)[-1]
                            if links_read >= _MAX_LINK_FILE_BYTES:
                                links_handle.close()
                                links_handle = None
                    ready, _, _ = select.select([fd], [], [], 0.25)
                    if not ready:
                        continue
                    assert fd is not None
                    try:
                        data = os.read(fd, 65536)
                    except OSError:
                        data = b""
                    if not data:
                        cleaned = ESCAPES.sub(b"", seen)
                        found = complete_token(cleaned, at_end_of_stream=True)
                        if found is not None:
                            keep(found.group(0), token_file)
                            say(signedIn=True)
                            return 0
                        break
                    seen = (seen + data)[-_MAX_SEEN_BYTES:]
                    cleaned = ESCAPES.sub(b"", seen)
                    if not shown:
                        link = sign_in_link(cleaned.decode(errors="replace"))
                        if link is not None:
                            say(link=link)
                            shown = True
                    found = complete_token(cleaned, at_end_of_stream=False)
                    if found is not None:
                        keep(found.group(0), token_file)
                        say(signedIn=True)
                        return 0
                else:
                    say(error=f"no sign-in within {WAIT_SECONDS // 60} minutes")
                    return 1
            finally:
                if links_handle is not None:
                    links_handle.close()
        finally:
            for descriptor in (error_read, error_write, guard_read):
                if descriptor >= 0:
                    os.close(descriptor)
            stop_child(pid, guard_write, fd)
    say(error=last_words(seen))
    return 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--claude", default=shutil.which("claude") or "claude")
    arguments = parser.parse_args(argv)

    # lapis ends a sign-in with SIGTERM; the finally below then ends Claude
    # Code's too.
    def request_stop(*_: object) -> None:
        global STOPPING
        STOPPING = True

    signal.signal(signal.SIGTERM, request_stop)
    try:
        return sign_in(arguments.claude, arguments.token_file)
    except OSError as error:
        say(error=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
