import base64
import json
import os
import re
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


TOML_ESCAPES = {
    "b": "\b",
    "t": "\t",
    "n": "\n",
    "f": "\f",
    "r": "\r",
    '"': '"',
    "\\": "\\",
}


def decode_basic_string(value):
    out = []
    index = 0
    while index < len(value):
        character = value[index]
        if character != "\\":
            out.append(character)
            index += 1
            continue
        index += 1
        if index >= len(value):
            return None
        escape = value[index]
        if escape in TOML_ESCAPES:
            out.append(TOML_ESCAPES[escape])
            index += 1
        elif escape in ("u", "U"):
            width = 4 if escape == "u" else 8
            digits = value[index + 1 : index + 1 + width]
            if len(digits) != width or any(
                digit not in "0123456789abcdefABCDEF" for digit in digits
            ):
                return None
            out.append(chr(int(digits, 16)))
            index += 1 + width
        else:
            return None
    return "".join(out)


def read_toml_string(text, index):
    length = len(text)
    if text.startswith('"""', index):
        end = text.find('"""', index + 3)
        if end < 0:
            return None, length
        value = text[index + 3 : end].removeprefix("\n")
        return decode_basic_string(re.sub(r"\\\s*\n\s*", "", value)), end + 3
    if text.startswith("'''", index):
        end = text.find("'''", index + 3)
        if end < 0:
            return None, length
        return text[index + 3 : end].removeprefix("\n"), end + 3
    quote = text[index]
    index += 1
    start = index
    while index < length:
        character = text[index]
        if quote == '"' and character == "\\":
            index += 2
            continue
        if character == quote:
            raw = text[start:index]
            return (raw if quote == "'" else decode_basic_string(raw)), index + 1
        if character == "\n":
            break
        index += 1
    return None, index


def skip_space_and_comments(text, index):
    length = len(text)
    while index < length:
        character = text[index]
        if character.isspace():
            index += 1
        elif character == "#":
            end = text.find("\n", index)
            index = length if end < 0 else end
        else:
            break
    return index


def notify_array(text, index):
    index = skip_space_and_comments(text, index)
    if index >= len(text) or text[index] != "[":
        return None
    index += 1
    words = []
    while True:
        index = skip_space_and_comments(text, index)
        if index >= len(text):
            return None
        if text[index] == "]":
            return words
        if text[index] not in "\"'":
            return None
        value, index = read_toml_string(text, index)
        if value is None:
            return None
        words.append(value)
        index = skip_space_and_comments(text, index)
        if index >= len(text):
            return None
        if text[index] == "]":
            return words
        if text[index] != ",":
            return None
        index += 1


def top_level_notify(text):
    # Without tomllib (python3 before 3.11, as macOS ships): find the top-level
    # `notify = [...]` array of plain strings, or None. Track brackets so a
    # nested array before notify is not mistaken for the first TOML table.
    length = len(text)
    index = 0
    depth = 0
    line_start = True
    key = re.compile(r'([A-Za-z0-9_-]+|"[^"\n]+"|\'[^\'\n]+\')\s*=')
    while index < length:
        character = text[index]
        if character == "#":
            end = text.find("\n", index)
            index = length if end < 0 else end
        elif depth == 0 and line_start and (character.isalpha() or character in "\"'"):
            found = key.match(text, index)
            if not found:
                index += 1
                line_start = False
                continue
            name = found.group(1)
            if name in ("notify", '"notify"', "'notify'"):
                return notify_array(text, found.end())
            index = found.end()
            line_start = False
        elif character in "\"'":
            value, end = read_toml_string(text, index)
            if value is None:
                return None
            index = end
            line_start = False
        elif character == "[":
            if depth == 0 and line_start:
                return None
            depth += 1
            index += 1
            line_start = False
        elif character == "]":
            depth = max(0, depth - 1)
            index += 1
            line_start = False
        elif character == "\n":
            index += 1
            line_start = True
        else:
            index += 1
            if not character.isspace():
                line_start = False
    return None


def notify_from_toml(text):
    try:
        import tomllib
    except ImportError:
        return top_level_notify(text)
    return tomllib.loads(text).get("notify")


def effective_notify(home):
    notify = None
    if os.path.isfile(os.path.join(home, "config.toml")):
        with open(os.path.join(home, "config.toml"), "rb") as f:
            notify = notify_from_toml(f.read().decode())
    profile = os.environ.get("LAPIS_CODEX_PROFILE")
    if not profile or not re.fullmatch(r"[A-Za-z0-9_-]+", profile):
        return notify
    path = os.path.join(home, f"{profile}.config.toml")
    if not os.path.isfile(path):
        return notify
    with open(path, "rb") as f:
        layered = notify_from_toml(f.read().decode())
    if layered is not None:
        return layered
    return notify


def chain(payload):
    home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
    notify = effective_notify(home)
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
