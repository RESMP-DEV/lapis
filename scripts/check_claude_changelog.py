"""Watch the Claude Code changelog RSS for releases lapis has not reviewed.

Fetches the feed, diffs entries against the high-water state in
runtime/changelog-watch/claude-code.json, and reports new releases. The
default run is read-only; --record writes the fetched entries as the new
baseline. The first run on a missing state file reports nothing as new,
and says that no baseline exists unless --record creates one. A recording
run also replaces a malformed baseline, and refuses to record a feed that
parsed to no entries. Exit codes: 0 on success (including "no new
releases"), 1 when the feed or state cannot be read or written. Feed
responses are capped at MAX_FEED_BYTES, and redirects are followed only
while they stay on the feed's own host.

    uv run --no-project python scripts/check_claude_changelog.py --json
    uv run --no-project python scripts/check_claude_changelog.py --record
"""

import argparse
import http.client
import json
import math
import os
import sys
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
from html.parser import HTMLParser
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
FEED_URL = "https://code.claude.com/docs/en/changelog/rss.xml"
STATE_PATH = ROOT / "runtime" / "changelog-watch" / "claude-code.json"
KEEP_ENTRIES = 200
MAX_FEED_BYTES = 2 * 1024 * 1024
SUMMARY_CHARS = 1600
USER_AGENT = "lapis-changelog-watcher/1.0"
ATOM = "{http://www.w3.org/2005/Atom}"
CONTENT_ENCODED = "{http://purl.org/rss/1.0/modules/content/}encoded"
# expat rejects C0 controls, but XML character references can still smuggle
# C1 controls (for example the one-byte CSI 0x9b) into element text. Strip
# every C0/C1 control except tab/LF/CR from feed-controlled text before it
# is stored or printed.
_FEED_CONTROL_CHARS = "".join(
    chr(code)
    for code in (*range(0x20), *range(0x7F, 0xA0))
    if code not in (0x09, 0x0A, 0x0D)
)
_STRIP_FEED_CONTROLS = str.maketrans("", "", _FEED_CONTROL_CHARS)
# Closing one of these tags separates words; without it html_text glues
# adjacent blocks together ("First itemSecond item").
_BLOCK_TAGS = frozenset(
    {
        "address",
        "article",
        "blockquote",
        "br",
        "caption",
        "dd",
        "details",
        "div",
        "dl",
        "dt",
        "figcaption",
        "figure",
        "form",
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "hr",
        "li",
        "main",
        "ol",
        "option",
        "p",
        "pre",
        "section",
        "summary",
        "table",
        "tbody",
        "td",
        "tfoot",
        "th",
        "thead",
        "tr",
        "ul",
    }
)
# Void elements never emit an end tag, so their boundary fires on the
# start tag instead (a plain <br> never reaches handle_endtag).
_VOID_SEPARATOR_TAGS = frozenset({"br", "hr"})


def _clean(text: str) -> str:
    return text.translate(_STRIP_FEED_CONTROLS)


class ChangelogError(Exception):
    """Raised when the feed or the state file cannot be fetched or parsed."""


@dataclass
class Entry:
    guid: str
    title: str
    link: str
    published: str | None
    summary: str | None

    def as_dict(self) -> dict[str, str | None]:
        return {
            "guid": self.guid,
            "title": self.title,
            "link": self.link,
            "published": self.published,
            "summary": self.summary,
        }


class _TextExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.chunks: list[str] = []

    def handle_data(self, data: str) -> None:
        self.chunks.append(data)

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in _VOID_SEPARATOR_TAGS:
            self.chunks.append(" ")

    def handle_endtag(self, tag: str) -> None:
        if tag in _BLOCK_TAGS:
            self.chunks.append(" ")


def html_text(markup: str) -> str:
    """Return the visible text of an HTML fragment as one collapsed line."""
    extractor = _TextExtractor()
    extractor.feed(markup)
    return " ".join("".join(extractor.chunks).split())


class _SameHostRedirectPolicy(urllib.request.HTTPRedirectHandler):
    """Follow redirects only while they stay on the feed's own host.

    A feed URL is operator-supplied, but the feed's content is not trusted:
    without this, a compromised or spoofed feed endpoint could redirect the
    fetch to an unrelated host.
    """

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> urllib.request.Request | None:
        target = urllib.parse.urlsplit(newurl)
        origin = urllib.parse.urlsplit(req.full_url)
        if target.hostname is None or target.hostname != origin.hostname:
            raise ChangelogError(
                f"feed redirected to another host ({newurl}); refusing to follow"
            )
        # A same-host hop can still drop TLS (https -> http) or shift ports,
        # either of which lets a network interceptor choose the baseline bytes.
        if target.scheme != origin.scheme or target.port != origin.port:
            raise ChangelogError(
                f"feed redirected off its scheme or port ({newurl}); refusing to follow"
            )
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_FEED_OPENER = urllib.request.build_opener(_SameHostRedirectPolicy)


def fetch_feed(url: str, timeout: float) -> bytes:
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "application/rss+xml, application/xml, text/xml",
        },
    )
    try:
        with _FEED_OPENER.open(request, timeout=timeout) as response:
            payload = response.read(MAX_FEED_BYTES + 1)
    except (OSError, http.client.HTTPException, ValueError) as error:
        # ValueError covers an unsupported URL scheme; HTTPException covers
        # a peer that answers with something that is not HTTP.
        raise ChangelogError(f"could not fetch {url}: {error}") from error
    if len(payload) > MAX_FEED_BYTES:
        raise ChangelogError(f"feed response exceeds the {MAX_FEED_BYTES}-byte limit")
    return payload


def _child(item: ET.Element, *names: str) -> ET.Element | None:
    for name in names:
        found = item.find(name)
        if found is not None:
            return found
    return None


def _text(item: ET.Element, *names: str) -> str:
    node = _child(item, *names)
    return _clean((node.text or "").strip()) if node is not None else ""


def _iso(date_text: str) -> str | None:
    if not date_text:
        return None
    try:
        moment = parsedate_to_datetime(date_text)
    except (TypeError, ValueError):
        return date_text
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=timezone.utc)
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _names(namespace: str, *names: str) -> list[str]:
    """Candidate child tags in lookup order, each kept once.

    Plain names also get the document's namespace prefix, so a feed that
    declares a default namespace still parses; already-qualified names are
    used as given.
    """
    candidates: list[str] = []
    for name in names:
        if namespace and not name.startswith("{"):
            candidates.append(f"{namespace}{name}")
        candidates.append(name)
    return list(dict.fromkeys(candidates))


def parse_feed(data: bytes) -> list[Entry]:
    """Parse RSS 2.0 or Atom entries, newest first as delivered."""
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise ChangelogError(f"feed is not valid XML: {error}") from error
    local_name = root.tag.rsplit("}", 1)[-1]
    namespace = root.tag[: len(root.tag) - len(local_name)]
    if local_name == "rss" and root.find(f"{namespace}channel") is not None:
        items = root.findall(f"./{namespace}channel/{namespace}item")
    elif local_name == "feed":
        items = root.findall(f"./{namespace}entry")
    else:
        raise ChangelogError("response is not an RSS or Atom feed")
    title_names = _names(namespace, "title", f"{ATOM}title")
    link_names = _names(namespace, "link", f"{ATOM}link")
    guid_names = _names(namespace, "guid", f"{ATOM}id")
    date_names = _names(namespace, "pubDate", f"{ATOM}published", f"{ATOM}updated")
    # content:encoded carries the full entry body; description is the fallback.
    body_names = list(
        dict.fromkeys(
            [CONTENT_ENCODED, *_names(namespace, "description", f"{ATOM}content")]
        )
    )
    entries = []
    for item in items:
        title = _text(item, *title_names) or "untitled"
        link_node = _child(item, *link_names)
        link = ""
        if link_node is not None:
            link = _clean((link_node.text or "").strip() or link_node.get("href", ""))
        guid = _text(item, *guid_names) or link or title
        published = _iso(_text(item, *date_names))
        body = _child(item, *body_names)
        summary = None
        if body is not None:
            visible = _clean(html_text(body.text or ""))
            summary = (
                visible
                if len(visible) <= SUMMARY_CHARS
                else visible[:SUMMARY_CHARS] + "…"
            )
        entries.append(
            Entry(
                guid=guid,
                title=title,
                link=link,
                published=published,
                summary=summary or None,
            )
        )
    return entries


def load_state(path: Path) -> dict[str, Any] | None:
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ChangelogError(f"unreadable state file {path}: {error}") from error
    if not isinstance(state, dict) or not isinstance(state.get("entries"), list):
        raise ChangelogError(f"state file {path} is not a changelog state object")
    if not all(
        isinstance(entry, dict) and isinstance(entry.get("guid"), str)
        for entry in state["entries"]
    ):
        raise ChangelogError(
            f"state file {path} has a malformed entry; remove it or record a new baseline"
        )
    return state


def known_guids(state: dict[str, Any]) -> set[str]:
    return {entry["guid"] for entry in state["entries"]}


def new_entries(entries: list[Entry], state: dict[str, Any] | None) -> list[Entry]:
    """Return feed entries absent from the recorded baseline.

    A missing state means "no baseline yet": report nothing so the first
    run initializes instead of replaying the whole feed as new.
    """
    if state is None:
        return []
    seen = known_guids(state)
    return [entry for entry in entries if entry.guid not in seen]


def _fsync_directory(directory: Path) -> None:
    """Best-effort directory sync so a replace survives a crash."""
    try:
        descriptor = os.open(directory, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def record_state(path: Path, feed_url: str, entries: list[Entry]) -> dict[str, Any]:
    # Every guid is kept, so an entry that falls out of the detailed window
    # is never re-reported as new; only the newest KEEP_ENTRIES keep their
    # full record (title, link, date, summary).
    payload = {
        "feed": feed_url,
        "updated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "entries": [
            entry.as_dict() if index < KEEP_ENTRIES else {"guid": entry.guid}
            for index, entry in enumerate(entries)
        ],
    }
    body = json.dumps(payload, indent=2) + "\n"
    temporary: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(body)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    except OSError as error:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError as cleanup_error:
                # A second attempt covers a transient failure; if it also
                # fails, name the orphaned file so it can be removed by hand.
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    raise ChangelogError(
                        f"could not record state file {path}: {error}; "
                        f"temporary {temporary} left behind: {cleanup_error}"
                    ) from error
        raise ChangelogError(f"could not record state file {path}: {error}") from error
    return payload


def collect(args: argparse.Namespace) -> dict[str, Any]:
    try:
        state = load_state(args.state)
    except ChangelogError:
        # --record replaces the baseline, so an unreadable one does not have
        # to be repaired by hand first.
        if not args.record:
            raise
        state = None
    entries = parse_feed(fetch_feed(args.feed, args.timeout))
    first_run = state is None
    fresh = new_entries(entries, state)
    result = {
        "feed": args.feed,
        "state": str(args.state),
        "checked_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "first_run": first_run,
        "initialized": first_run and args.record,
        "latest": entries[0].as_dict() if entries else None,
        "new_entries": [entry.as_dict() for entry in fresh],
        "recorded": False,
    }
    if args.record:
        if not entries:
            raise ChangelogError(
                "feed parsed but held no entries; refusing to record an empty baseline"
            )
        record_state(args.state, args.feed, entries)
        result["recorded"] = True
    return result


def report(result: dict[str, Any]) -> None:
    latest = result["latest"]
    if result["first_run"] and not result.get("recorded"):
        print(
            "No baseline found; older releases were not reported. "
            "Use --record to initialize the baseline."
        )
    elif result["initialized"]:
        print(
            "Initialized baseline at "
            f"{latest['title'] if latest else 'an empty feed'}; nothing reported as new."
        )
    for entry in result["new_entries"]:
        when = f" ({entry['published']})" if entry["published"] else ""
        print(f"NEW {entry['title']}{when} {entry['link']}")
    # A first run has nothing to compare against, so it must not claim the
    # baseline is up to date.
    if not result["first_run"] and not result["new_entries"] and latest:
        print(f"No new releases since {latest['title']}.")
    if result.get("recorded"):
        print(f"Recorded baseline in {result['state']}.")


def positive_timeout(value: str) -> float:
    try:
        seconds = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "timeout must be a positive finite number"
        ) from error
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("timeout must be a positive finite number")
    return seconds


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--feed", default=FEED_URL, help="RSS/Atom feed URL")
    parser.add_argument(
        "--state", type=Path, default=STATE_PATH, help="state file path"
    )
    parser.add_argument(
        "--timeout", type=positive_timeout, default=20.0, help="fetch timeout seconds"
    )
    parser.add_argument(
        "--record",
        action="store_true",
        help="write the fetched entries as the new baseline",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = collect(args)
    except ChangelogError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        report(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
