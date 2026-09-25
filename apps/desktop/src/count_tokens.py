"""Counts Codex and Claude Code tokens on this machine for lapis's usage view.

lapis runs it on another machine over ssh as `python3 - SINCE`, with this file
on standard input; SINCE is a Unix time, and transcripts untouched since then
are skipped. It prints one JSON object:

    {"codex": {model: {hour: [input, cache_read, cache_write, output]}},
     "claude": {...}}

where hour is the UTC hour "YYYY-MM-DDTHH". The rules are the desktop's
TokenLedger (usage.cpp): Codex's running totals as differences, a file's first
report and any decrease counted as that call alone; each Claude message once
across lines and files, at the largest usage any of its lines reported.
Python 3.8 or later; the standard library only.
"""

import json
import os
import sys
import time

DECODER = json.JSONDecoder()
EMPTY = [0, 0, 0, 0]


def object_at(line, at):
    """The JSON object that opens at byte `at`, or {}."""
    try:
        found = DECODER.raw_decode(line[at:].decode("utf-8", "replace"))[0]
    except ValueError:
        return {}
    return found if isinstance(found, dict) else {}


def string_after(line, key, start=0):
    """The JSON string after `key` (ending with its opening quote), or b""."""
    at = line.find(key, start)
    if at < 0:
        return b""
    at += len(key)
    end = at
    while end < len(line):
        if line[end] == 0x5C:  # a backslash escapes the next byte
            end += 2
            continue
        if line[end] == 0x22:
            return line[at:end]
        end += 1
    return b""


def number(usage, key):
    value = usage.get(key) or 0
    return int(value) if isinstance(value, (int, float)) else 0


def hour_of(stamp, since_hour):
    """b"2026-09-24T23:32:08.070Z" -> "2026-09-24T23", when not before since."""
    if len(stamp) < 13 or stamp[10:11] != b"T":
        return None
    hour = stamp[:13].decode("ascii", "replace")
    return hour if hour >= since_hour else None


def add(totals, provider, model, hour, used):
    bucket = totals[provider].setdefault(model, {}).setdefault(hour, [0, 0, 0, 0])
    for index, value in enumerate(used):
        bucket[index] += value


def codex_count(usage):
    """Codex counts cached and cache-written input inside input_tokens."""
    cached = number(usage, "cached_input_tokens")
    written = number(usage, "cache_write_input_tokens")
    return [
        max(0, number(usage, "input_tokens") - cached - written),
        cached,
        written,
        number(usage, "output_tokens"),
    ]


def codex_file(path, totals, since_hour):
    total = None
    model = "codex"
    with open(path, "rb") as source:
        for line in source:
            if not line.endswith(b"\n"):
                break  # still being written
            head = line[:200]
            if b'"type":"turn_context"' in head:
                found = string_after(line, b'"model":"')
                if found:
                    model = found.decode("utf-8", "replace")
                continue
            if b'"payload":{"type":"token_count"' not in head:
                continue
            at = line.find(b'"total_token_usage":')
            if at < 0:
                continue
            now = codex_count(object_at(line, at + 20))
            if total is not None and all(a >= b for a, b in zip(now, total)):
                used = [a - b for a, b in zip(now, total)]
            else:
                last = line.find(b'"last_token_usage":')
                used = codex_count(object_at(line, last + 19)) if last >= 0 else EMPTY
            total = now
            hour = hour_of(string_after(line, b'"timestamp":"'), since_hour)
            if hour and any(used):
                add(totals, "codex", model, hour, used)


def claude_file(path, totals, seen, since_hour):
    with open(path, "rb") as source:
        for line in source:
            if not line.endswith(b"\n"):
                break
            message = line.find(b'"message":{')
            if (
                message < 0
                or b'"role":"assistant"' not in line[message : message + 400]
            ):
                continue
            usage = line.rfind(b'"usage":{')
            if usage < 0:
                continue
            model = string_after(line, b'"model":"', message)
            if model == b"<synthetic>":
                continue
            hour = hour_of(string_after(line, b'"timestamp":"', usage), since_hour)
            if not hour:
                continue
            found = object_at(line, usage + 8)
            used = [
                number(found, "input_tokens"),
                number(found, "cache_read_input_tokens"),
                number(found, "cache_creation_input_tokens"),
                number(found, "output_tokens"),
            ]
            identity = string_after(line, b'"id":"', message)
            if identity:
                key = identity + b":" + string_after(line, b'"requestId":"')
                counted = seen.get(key, EMPTY)
                largest = [max(a, b) for a, b in zip(counted, used)]
                used = [a - b for a, b in zip(largest, counted)]
                seen[key] = largest
            if any(used):
                add(
                    totals,
                    "claude",
                    model.decode("utf-8", "replace") or "claude",
                    hour,
                    used,
                )


def transcripts(root, prefix, since):
    for directory, _, names in os.walk(root):
        for name in names:
            if not name.startswith(prefix) or not name.endswith(".jsonl"):
                continue
            path = os.path.join(directory, name)
            try:
                if os.stat(path).st_mtime >= since:
                    yield path
            except OSError:
                continue


def count(since, codex_root, claude_root):
    since_hour = time.strftime("%Y-%m-%dT%H", time.gmtime(since))
    totals = {"codex": {}, "claude": {}}
    for path in transcripts(codex_root, "rollout-", since):
        try:
            codex_file(path, totals, since_hour)
        except OSError:
            continue
    seen = {}
    for path in transcripts(claude_root, "", since):
        try:
            claude_file(path, totals, seen, since_hour)
        except OSError:
            continue
    return totals


def main():
    since = float(sys.argv[1]) if len(sys.argv) > 1 else time.time() - 31 * 86400
    home = os.path.expanduser("~")
    codex_home = os.environ.get("CODEX_HOME") or os.path.join(home, ".codex")
    claude_home = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(home, ".claude")
    totals = count(
        since,
        os.path.join(codex_home, "sessions"),
        os.path.join(claude_home, "projects"),
    )
    json.dump(totals, sys.stdout, separators=(",", ":"))


if __name__ == "__main__":
    main()
