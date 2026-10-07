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
        return []
    if key == "session_crons":
        return [t if isinstance(t, dict) else {} for t in value]
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


def chain(payload):
    import tomllib

    home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
    with open(os.path.join(home, "config.toml"), "rb") as f:
        notify = tomllib.load(f).get("notify")
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
