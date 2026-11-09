import base64
import json
import os
import stat
import sys

K = {
    "hook_event_name",
    "notification_type",
    "prompt_id",
    "reason",
    "session_id",
    "source",
    "tool_name",
    "tool_use_id",
    "type",
    "thread-id",
    "turn-id",
}


def work(key, value):
    if not isinstance(value, list) or len(value) > 64:
        # The local relay treats a present non-array as schema drift. Keep that
        # conservative meaning across the terminal boundary; an empty list
        # would claim that no background work is running.
        return {}
    if key == "session_crons":
        # The remote consumer derives a count. Do not put cron identities,
        # schedules or other private configuration on the terminal.
        return [{} for _ in value]
    out = []
    for t in value:
        s = t.get("status") if isinstance(t, dict) else None
        out.append({"status": s} if isinstance(s, str) and len(s) <= 32 else {})
    return out


def valid_nonce(value):
    return len(value) == 32 and all(c in "0123456789abcdef" for c in value)


def relay(cli, text):
    n = os.environ.get("LAPIS_HOOK_NONCE", "")
    source = json.loads(text)
    if not valid_nonce(n) or not isinstance(source, dict):
        return
    event = {
        k: v
        for k, v in source.items()
        if k in K and isinstance(v, str) and len(v.encode()) <= 256
    }
    for k in ("background_tasks", "session_crons"):
        if cli == "claude" and k in source:
            event[k] = work(k, source[k])
    body = base64.b64encode(json.dumps(event, separators=(",", ":")).encode())
    frame = b"\x1b]7717;lapis-event;" + n.encode() + b";" + body + b"\x07"
    path = os.environ.get("LAPIS_HOOK_TTY", "")
    if not path.startswith("/dev/") or os.path.realpath(path) != path:
        return
    tty = os.open(path, os.O_WRONLY | os.O_NOCTTY | os.O_NOFOLLOW)
    try:
        if not stat.S_ISCHR(os.fstat(tty).st_mode):
            raise OSError("hook output is not a character device")
        view = memoryview(frame)
        while view:
            count = os.write(tty, view)
            if count <= 0:
                raise OSError("short write")
            view = view[count:]
    finally:
        os.close(tty)


def top_level_notify(text):
    # Without tomllib (python3 before 3.11, as macOS ships): the top-level
    # `notify = [...]` array of plain strings, which may span lines, or None.
    import re

    word = re.compile(r'\s*(?:"((?:[^"\\\n]|\\.)*)"|\'([^\'\n]*)\')\s*(,|\])')
    lines = iter(text.splitlines())
    for line in lines:
        if line.lstrip().startswith("["):
            return None  # the first table ends the top level
        found = re.match(r"\s*notify\s*=\s*\[(.*)$", line)
        if not found:
            continue
        rest, words = found.group(1), []
        while True:
            rest = re.sub(r"^(\s*(#.*)?)?$", "", rest)
            if not rest:
                rest = next(lines, None)
                if rest is None:
                    return None
                continue
            if rest.lstrip().startswith("]"):
                return words
            item = word.match(rest)
            if not item:
                return None
            basic, literal, end = item.groups()
            words.append(json.loads('"' + basic + '"') if basic is not None else literal)
            rest = rest[item.end() :]
            if end == "]":
                return words
    return None


def chain(payload):
    home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
    with open(os.path.join(home, "config.toml"), "rb") as f:
        text = f.read()
    try:
        import tomllib
    except ImportError:
        notify = top_level_notify(text.decode())
    else:
        notify = tomllib.loads(text.decode()).get("notify")
    if isinstance(notify, list) and notify and all(isinstance(w, str) for w in notify):
        os.execvp(notify[0], notify + [payload])


cli = sys.argv[1] if len(sys.argv) > 1 else ""
try:
    if cli == "claude":
        relay(cli, sys.stdin.buffer.read())
    elif cli == "codex" and len(sys.argv) > 2:
        relay(cli, sys.argv[2])
except Exception:
    pass
try:
    if cli == "codex" and len(sys.argv) > 2:
        chain(sys.argv[2])
except Exception:
    pass
