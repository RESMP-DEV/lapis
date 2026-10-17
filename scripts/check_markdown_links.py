"""Check repository-local Markdown links and heading fragments."""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.parse
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IGNORED_PARTS = {".git", ".sindexer", ".worktrees", "build", "runtime"}
LINK_PATTERN = re.compile(r"(?:!?)\[[^\]]+\]\(([^)]+)\)")
HEADING_PATTERN = re.compile(r"^#{1,6}\s+(.*?)\s*#*$", re.MULTILINE)


def heading_anchor(heading: str) -> str:
    """Return the portable subset of GitHub's heading anchor shape."""

    text = re.sub(r"[^\w\s-]", "", heading, flags=re.UNICODE).strip().lower()
    return re.sub(r"\s+", "-", text)


def markdown_anchors(text: str) -> set[str]:
    anchors: list[str] = [
        heading_anchor(match.group(1)) for match in HEADING_PATTERN.finditer(text)
    ]
    counts: defaultdict[str, int] = defaultdict(int)
    result: set[str] = set()
    for anchor in anchors:
        counts[anchor] += 1
        result.add(anchor if counts[anchor] == 1 else f"{anchor}-{counts[anchor] - 1}")
    return result


def safe_relative_link(root: Path, source: Path, target: str) -> Path | None:
    path_text = urllib.parse.urlparse(target).path
    if not path_text or path_text.startswith(("http://", "https://", "mailto:")):
        return None
    candidate = (source.parent / urllib.parse.unquote(path_text)).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        return None
    return candidate


def collect_failures(root: Path) -> list[str]:
    root = root.resolve()
    failures: list[str] = []
    anchors_by_file: dict[Path, set[str]] = {}
    markdown_files = [
        path.resolve()
        for path in root.rglob("*.md")
        if not any(part in IGNORED_PARTS for part in path.relative_to(root).parts)
    ]

    for path in markdown_files:
        try:
            anchors_by_file[path] = markdown_anchors(path.read_text(encoding="utf-8"))
        except OSError as error:
            failures.append(f"{path.relative_to(root)}: unreadable: {error}")

    for source in markdown_files:
        text = source.read_text(encoding="utf-8")
        for match in LINK_PATTERN.finditer(text):
            target = match.group(1).strip()
            if target.startswith(("#", "http://", "https://", "mailto:")):
                if target.startswith("#"):
                    fragment = urllib.parse.unquote(target[1:]).lower()
                    if fragment and fragment not in anchors_by_file[source]:
                        failures.append(
                            f"{source.relative_to(root)}: missing same-document fragment {target}"
                        )
                continue

            path = safe_relative_link(root, source, target)
            if path is None:
                continue
            if not path.exists():
                failures.append(f"{source.relative_to(root)}: missing target {target}")
                continue
            fragment = urllib.parse.unquote(
                urllib.parse.urlparse(target).fragment
            ).lower()
            if not fragment or path.suffix != ".md":
                continue
            if path not in anchors_by_file:
                try:
                    anchors_by_file[path] = markdown_anchors(
                        path.read_text(encoding="utf-8")
                    )
                except OSError:
                    failures.append(
                        f"{source.relative_to(root)}: unreadable fragment target {target}"
                    )
                    continue
            if fragment not in anchors_by_file[path]:
                failures.append(
                    f"{source.relative_to(root)}: missing fragment {target}"
                )

    return sorted(failures)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--json", action="store_true", help="write failures as JSON")
    args = parser.parse_args()
    failures = collect_failures(args.root)
    if args.json:
        print(json.dumps({"failures": failures, "passed": not failures}, indent=2))
    else:
        print("\n".join(failures) if failures else "Markdown links and fragments: PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
