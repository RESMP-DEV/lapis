#!/usr/bin/env python3
"""Which conversation shapes do Claude Code's saved sessions on this machine use?

Counts record types, content block types, tool names and system subtypes in
the newest interactive sessions, and marks every shape the visual view
(apps/desktop/src/transcript.cpp) has no renderer for. Those fall back to a
plain block; this list is what to teach the view next. Prints counts only,
never conversation text.

    python3 scripts/transcript_survey.py [--sessions 300] [--json]
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys
from pathlib import Path

# What transcript.cpp renders on purpose; keep in step with ClaudeReader::add
# and tool_item. Bookkeeping records are skipped by design, not unknown.
RENDERED_BLOCKS = {
    "text",
    "thinking",
    "redacted_thinking",
    "tool_use",
    "tool_result",
    "image",
    "document",
}
SHAPED_TOOLS = {
    "Bash",
    "Read",
    "Write",
    "Edit",
    "MultiEdit",
    "Grep",
    "Glob",
    "Agent",
    "Task",
    "WebFetch",
    "WebSearch",
    "TodoWrite",
    "Monitor",
    "SendMessage",
    "ScheduleWakeup",
    "Skill",
    "TaskStop",
    "ToolSearch",
    "AskUserQuestion",
}
SHOWN_SYSTEM = {"compact_boundary"}


def rendered_block(kind: str) -> bool:
    return kind in RENDERED_BLOCKS


def shaped_tool(name: object) -> bool:
    return isinstance(name, str) and (name in SHAPED_TOOLS or name.startswith("mcp__"))


def sessions(root: Path, limit: int) -> list[Path]:
    files = [p for p in root.glob("*/*.jsonl") if p.is_file()]
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return files[:limit]


def survey(paths: list[Path]) -> dict:
    records: collections.Counter = collections.Counter()
    blocks: collections.Counter = collections.Counter()
    tools: collections.Counter = collections.Counter()
    system: collections.Counter = collections.Counter()
    versions: collections.Counter = collections.Counter()
    read = 0
    for path in paths:
        try:
            handle = path.open("rb")
        except OSError:
            continue
        with handle:
            for raw in handle:
                try:
                    record = json.loads(raw)
                except ValueError:
                    continue
                if not isinstance(record, dict) or record.get("isSidechain"):
                    continue
                kind = record.get("type", "?")
                records[kind] += 1
                if record.get("version"):
                    versions[record["version"]] += 1
                if kind == "system":
                    system[record.get("subtype", "?")] += 1
                message = record.get("message")
                if not isinstance(message, dict):
                    continue
                content = message.get("content")
                if isinstance(content, list):
                    for block in content:
                        if not isinstance(block, dict):
                            continue
                        blocks[block.get("type", "?")] += 1
                        if block.get("type") == "tool_use":
                            tools[block.get("name", "?")] += 1
        read += 1
    return {
        "sessions": read,
        "records": records,
        "blocks": blocks,
        "tools": tools,
        "system": system,
        "versions": versions,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--sessions", type=int, default=300, help="newest sessions to read"
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()
    home = Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home() / ".claude"))
    result = survey(sessions(home / "projects", args.sessions))
    unknown_blocks = {
        k: v for k, v in result["blocks"].items() if not rendered_block(k)
    }
    generic_tools = {k: v for k, v in result["tools"].items() if not shaped_tool(k)}
    if args.json:
        json.dump(
            {
                **{
                    k: dict(v) if isinstance(v, collections.Counter) else v
                    for k, v in result.items()
                },
                "unrendered_blocks": unknown_blocks,
                "generic_tools": generic_tools,
            },
            sys.stdout,
            indent=1,
            sort_keys=True,
        )
        print()
        return 0
    newest = max(
        result["versions"],
        key=lambda v: tuple(int(x) for x in v.split(".") if x.isdigit()),
        default="?",
    )
    print(f"{result['sessions']} sessions, newest CLI version {newest}")
    print("\ncontent blocks (* = shown as a plain block)")
    for name, count in result["blocks"].most_common():
        print(f"  {'*' if name in unknown_blocks else ' '} {count:>9}  {name}")
    print("\ntools (* = generic card: name plus compact input)")
    for name, count in result["tools"].most_common(40):
        print(f"  {'*' if name in generic_tools else ' '} {count:>9}  {name}")
    print("\nsystem records (shown: " + ", ".join(sorted(SHOWN_SYSTEM)) + ")")
    for name, count in result["system"].most_common():
        print(f"    {count:>9}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
