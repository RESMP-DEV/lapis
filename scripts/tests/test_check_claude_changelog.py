"""Unit coverage for the Claude Code changelog watcher."""

import http.client
import http.server
import io
import json
import sys
import tempfile
import threading
import unittest
from collections.abc import Sequence
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_claude_changelog as watcher
from check_claude_changelog import ChangelogError, parse_feed

FEED = b"""<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/">
  <channel>
    <title>Claude Code changelog</title>
    <item>
      <title><![CDATA[2.1.284]]></title>
      <link>https://code.claude.com/docs/en/changelog#2-1-284</link>
      <guid isPermaLink="false">abc124</guid>
      <pubDate>Sat, 26 Sep 2026 10:00:00 GMT</pubDate>
      <content:encoded><![CDATA[<ul><li>Changed <code>stream-json</code> init events &amp; headers</li></ul>]]></content:encoded>
    </item>
    <item>
      <title><![CDATA[2.1.283]]></title>
      <link>https://code.claude.com/docs/en/changelog#2-1-283</link>
      <guid isPermaLink="false">abc123</guid>
      <pubDate>Fri, 25 Sep 2026 22:00:11 GMT</pubDate>
      <description><![CDATA[<p>Added and removed things</p>]]></description>
    </item>
  </channel>
</rss>
"""


ATOM_FEED = b"""<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>Claude Code changelog</title>
  <entry>
    <title>2.1.286</title>
    <link rel="alternate" href="https://code.claude.com/docs/en/changelog#2-1-286"/>
    <id>tag:claude,2026:2.1.286</id>
    <published>2026-09-27T11:00:00Z</published>
    <content type="html">&lt;p&gt;Atom entry body&lt;/p&gt;</content>
  </entry>
</feed>
"""

NAMESPACED_FEED = b"""<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns="http://example.invalid/rss">
  <channel>
    <item>
      <title>2.1.287</title>
      <link>https://code.claude.com/docs/en/changelog#2-1-287</link>
      <guid>abc127</guid>
      <pubDate>Sun, 27 Sep 2026 12:00:00 GMT</pubDate>
    </item>
  </channel>
</rss>
"""


def items_feed(count: int) -> bytes:
    items = "".join(
        f"<item><title>2.1.{280 + index}</title><guid>g{index}</guid></item>"
        for index in range(count)
    )
    return (
        f'<?xml version="1.0"?><rss version="2.0"><channel>{items}</channel></rss>'
    ).encode()


def recorded_state(path: Path, guids: list[str]) -> None:
    watcher.record_state(
        path,
        watcher.FEED_URL,
        [
            watcher.Entry(guid, f"2.1.{guid}", "https://example.invalid", None, None)
            for guid in guids
        ],
    )


class ParseFeedTests(unittest.TestCase):
    def test_parses_rss_items_with_cdata_and_entities(self) -> None:
        entries = parse_feed(FEED)
        self.assertEqual([entry.guid for entry in entries], ["abc124", "abc123"])
        self.assertEqual(entries[0].title, "2.1.284")
        self.assertEqual(
            entries[0].link, "https://code.claude.com/docs/en/changelog#2-1-284"
        )
        self.assertEqual(entries[0].published, "2026-09-26T10:00:00Z")
        self.assertEqual(
            entries[0].summary, "Changed stream-json init events & headers"
        )

    def test_summary_uses_description_when_content_encoded_is_missing(self) -> None:
        entries = parse_feed(FEED)
        self.assertEqual(entries[1].summary, "Added and removed things")

    def test_parses_atom_entries(self) -> None:
        entries = parse_feed(ATOM_FEED)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].guid, "tag:claude,2026:2.1.286")
        self.assertEqual(
            entries[0].link, "https://code.claude.com/docs/en/changelog#2-1-286"
        )
        self.assertEqual(entries[0].published, "2026-09-27T11:00:00Z")
        self.assertEqual(entries[0].summary, "Atom entry body")

    def test_parses_rss_that_declares_a_default_namespace(self) -> None:
        entries = parse_feed(NAMESPACED_FEED)
        self.assertEqual([entry.guid for entry in entries], ["abc127"])
        self.assertEqual(entries[0].title, "2.1.287")
        self.assertEqual(entries[0].published, "2026-09-27T12:00:00Z")

    def test_separates_adjacent_block_elements(self) -> None:
        feed = FEED.replace(
            b"<ul><li>Changed <code>stream-json</code> init events &amp; headers</li></ul>",
            b"<ul><li>First item</li><li>Second item</li></ul>",
        )
        entries = parse_feed(feed)
        self.assertEqual(entries[0].summary, "First item Second item")

    def test_separates_adjacent_table_cells(self) -> None:
        feed = FEED.replace(
            b"<ul><li>Changed <code>stream-json</code> init events &amp; headers</li></ul>",
            b"<table><tr><td>Left cell</td><td>Right cell</td></tr></table>",
        )
        entries = parse_feed(feed)
        self.assertEqual(entries[0].summary, "Left cell Right cell")

    def test_separates_on_a_plain_line_break(self) -> None:
        # A bare <br> never emits an end tag, so the separator has to fire on
        # the start tag.
        feed = FEED.replace(
            b"<ul><li>Changed <code>stream-json</code> init events &amp; headers</li></ul>",
            b"Line one<br>Line two",
        )
        entries = parse_feed(feed)
        self.assertEqual(entries[0].summary, "Line one Line two")

    def test_tolerates_unknown_children(self) -> None:
        feed = FEED.replace(
            b"<title><![CDATA[2.1.284]]></title>",
            b"<title>2.1.284</title>"
            b"<unknown-channel-child>channel sentinel</unknown-channel-child>",
        ).replace(
            b'<guid isPermaLink="false">abc124</guid>',
            b'<guid isPermaLink="false">abc124</guid>'
            b"<unknown-item-child>item sentinel"
            b"<unknown-nested-child>nested sentinel</unknown-nested-child>"
            b"</unknown-item-child>",
        )
        entries = parse_feed(feed)
        self.assertEqual([entry.guid for entry in entries], ["abc124", "abc123"])
        self.assertEqual(entries[0].title, "2.1.284")
        self.assertEqual(
            entries[0].link, "https://code.claude.com/docs/en/changelog#2-1-284"
        )

    def test_strips_c1_controls_from_feed_text(self) -> None:
        # expat admits C1 controls both as character references and as raw
        # bytes. The HTML entity path remaps &#155; to a printable glyph, so
        # a raw CSI byte is what reaches the summary.
        feed = FEED.replace(
            b"<title><![CDATA[2.1.284]]></title>",
            b"<title>2.1.284&#155;31m</title>",
        ).replace(
            b"<li>Changed <code>stream-json</code> init events &amp; headers</li>",
            b"<li>Changed \xc2\x9b31mthings</li>",
        )
        entries = parse_feed(feed)
        self.assertEqual(entries[0].title, "2.1.28431m")
        self.assertEqual(entries[0].summary, "Changed 31mthings")

    def test_marks_a_truncated_summary(self) -> None:
        with patch.object(watcher, "SUMMARY_CHARS", 10):
            entries = parse_feed(FEED)
        self.assertEqual(entries[0].summary, "Changed st…")

    def test_rejects_invalid_xml(self) -> None:
        with self.assertRaises(ChangelogError):
            parse_feed(b"<rss><channel>")

    def test_non_feed_response_cannot_replace_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            recorded_state(path, ["abc123"])
            original = path.read_bytes()
            with patch.object(
                watcher, "fetch_feed", return_value=b"<html>unavailable</html>"
            ):
                with self.assertRaisesRegex(ChangelogError, "not an RSS or Atom feed"):
                    watcher.collect(
                        watcher.parse_args(["--state", str(path), "--record"])
                    )
            self.assertEqual(path.read_bytes(), original)

    def test_timeout_rejects_nonpositive_or_nonfinite_values(self) -> None:
        for value in ["0", "-1", "nan", "inf"]:
            with self.subTest(value=value), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    watcher.parse_args(["--timeout", value])
                self.assertEqual(error.exception.code, 2)


class _FeedResponse:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.read_size: int | None = None

    def __enter__(self) -> "_FeedResponse":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def read(self, size: int) -> bytes:
        self.read_size = size
        return self.payload[:size]


class _StubOpener:
    def __init__(self, response: object = None, error: Exception | None = None) -> None:
        self.response = response
        self.error = error

    def open(self, request: object, timeout: float | None = None) -> object:
        if self.error is not None:
            raise self.error
        return self.response


def _feed_handler(redirect_to: str) -> type[http.server.BaseHTTPRequestHandler]:
    class FeedHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - http.server dispatch name
            if self.path == "/redirect":
                host, port = self.server.server_address[:2]
                self.send_response(302)
                self.send_header(
                    "Location", redirect_to or f"http://{host}:{port}/feed"
                )
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/rss+xml")
            self.send_header("Content-Length", str(len(FEED)))
            self.end_headers()
            self.wfile.write(FEED)

        def log_message(self, format: str, *args: object) -> None:
            """Keep the test output free of per-request lines."""

    return FeedHandler


class _FeedHTTPServer:
    """Serves FEED at /feed and a 302 at /redirect from a daemon thread."""

    def __init__(self, redirect_to: str | None) -> None:
        self._server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), _feed_handler(redirect_to or "")
        )
        host, port = self._server.server_address[:2]
        self.feed_url = f"http://{host}:{port}/feed"
        self.redirect_url = f"http://{host}:{port}/redirect"
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def __enter__(self) -> "_FeedHTTPServer":
        self._thread.start()
        return self

    def __exit__(self, *args: object) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=5)
        return None


class FetchFeedTests(unittest.TestCase):
    def test_rejects_body_over_named_limit_before_parsing(self) -> None:
        response = _FeedResponse(b"<rss>too-large</rss>")
        with (
            patch.object(watcher, "MAX_FEED_BYTES", 4),
            patch.object(watcher, "_FEED_OPENER", _StubOpener(response)),
        ):
            with self.assertRaisesRegex(ChangelogError, "4-byte limit"):
                watcher.fetch_feed("https://example.invalid/feed", 0.1)
        self.assertEqual(response.read_size, 5)

    def test_non_http_and_value_errors_become_changelog_errors(self) -> None:
        for error in (http.client.BadStatusLine("nonsense"), ValueError("unknown url")):
            with self.subTest(error=error):
                with patch.object(watcher, "_FEED_OPENER", _StubOpener(error=error)):
                    with self.assertRaisesRegex(ChangelogError, "could not fetch"):
                        watcher.fetch_feed("https://example.invalid/feed", 0.1)

    def test_redirect_to_another_host_is_refused(self) -> None:
        with _FeedHTTPServer(redirect_to="http://example.invalid/feed") as server:
            with self.assertRaisesRegex(ChangelogError, "another host"):
                watcher.fetch_feed(server.redirect_url, 1.0)

    def test_redirect_on_the_same_host_is_followed(self) -> None:
        with _FeedHTTPServer(redirect_to=None) as server:
            payload = watcher.fetch_feed(server.redirect_url, 1.0)
        self.assertEqual(parse_feed(payload)[0].guid, "abc124")

    def test_redirect_to_a_different_port_is_refused(self) -> None:
        # Same host and scheme, but the hop lands on another local port: an
        # interceptor who owns that port serves the baseline bytes.
        with _FeedHTTPServer(redirect_to=None) as destination:
            with _FeedHTTPServer(redirect_to=destination.feed_url) as origin:
                with self.assertRaisesRegex(ChangelogError, "scheme or port"):
                    watcher.fetch_feed(origin.redirect_url, 1.0)


class NewEntriesTests(unittest.TestCase):
    def entries(self) -> list[watcher.Entry]:
        return parse_feed(FEED)

    def test_without_state_reports_nothing(self) -> None:
        self.assertEqual(watcher.new_entries(self.entries(), None), [])

    def test_reports_only_unknown_guids_in_feed_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            recorded_state(state_path, ["abc123"])
            state = watcher.load_state(state_path)
            fresh = watcher.new_entries(self.entries(), state)
        self.assertEqual([entry.guid for entry in fresh], ["abc124"])


class CollectTests(unittest.TestCase):
    def run_collect(
        self,
        directory: str,
        feed: bytes = FEED,
        argv: Sequence[str] = (),
    ) -> tuple[dict[str, object], Path]:
        state_path = Path(directory) / "claude-code.json"
        argv = ["--state", str(state_path), *argv]
        with patch.object(watcher, "fetch_feed", return_value=feed):
            result = watcher.collect(watcher.parse_args(argv))
        return result, state_path

    def test_first_run_without_record_does_not_initialize(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result, state_path = self.run_collect(directory)
            self.assertTrue(result["first_run"])
            self.assertFalse(result["initialized"])
            self.assertFalse(result["recorded"])
            self.assertEqual(result["new_entries"], [])
            self.assertFalse(state_path.exists())

    def test_read_only_first_run_reports_missing_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            argv = ["--state", str(state_path)]
            output = io.StringIO()
            with patch.object(watcher, "fetch_feed", return_value=FEED):
                with redirect_stdout(output):
                    self.assertEqual(watcher.main(argv), 0)
        self.assertIn("No baseline found", output.getvalue())
        self.assertIn("--record", output.getvalue())
        self.assertFalse(state_path.exists())

    def test_record_persists_baseline_and_next_run_is_clean(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, state_path = self.run_collect(directory, argv=("--record",))
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(state["feed"], watcher.FEED_URL)
            self.assertEqual(
                [entry["guid"] for entry in state["entries"]], ["abc124", "abc123"]
            )
            self.assertEqual(list(state_path.parent.glob(".*.tmp")), [])
            result, _ = self.run_collect(directory)
            self.assertFalse(result["initialized"])
            self.assertFalse(result["first_run"])
            self.assertEqual(result["new_entries"], [])

    def test_keeps_every_guid_beyond_the_detailed_window(self) -> None:
        feed = items_feed(4)
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(watcher, "KEEP_ENTRIES", 3):
                _, state_path = self.run_collect(
                    directory, feed=feed, argv=("--record",)
                )
                state = json.loads(state_path.read_text(encoding="utf-8"))
                self.assertEqual(
                    [entry["guid"] for entry in state["entries"]],
                    ["g0", "g1", "g2", "g3"],
                )
                self.assertEqual(state["entries"][3], {"guid": "g3"})
                result, _ = self.run_collect(directory, feed=feed)
            self.assertEqual(result["new_entries"], [])
            self.assertFalse(result["first_run"])

    def test_first_run_does_not_claim_the_baseline_is_current(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            output = io.StringIO()
            with patch.object(watcher, "fetch_feed", return_value=FEED):
                with redirect_stdout(output):
                    self.assertEqual(watcher.main(["--state", str(state_path)]), 0)
            self.assertIn("No baseline found", output.getvalue())
            self.assertNotIn("No new releases since", output.getvalue())

    def test_record_recovers_from_a_malformed_state_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            state_path.write_text('{"entries": [{"guid": 123}]}', encoding="utf-8")
            output = io.StringIO()
            with patch.object(watcher, "fetch_feed", return_value=FEED):
                with redirect_stdout(output):
                    self.assertEqual(
                        watcher.main(["--state", str(state_path), "--record"]), 0
                    )
            self.assertIn("Initialized baseline", output.getvalue())
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(
                [entry["guid"] for entry in state["entries"]], ["abc124", "abc123"]
            )

    def test_record_refuses_an_empty_feed(self) -> None:
        empty = b'<rss version="2.0"><channel><title>empty</title></channel></rss>'
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            errors = io.StringIO()
            with patch.object(watcher, "fetch_feed", return_value=empty):
                with redirect_stderr(errors):
                    self.assertEqual(
                        watcher.main(["--state", str(state_path), "--record"]), 1
                    )
            self.assertIn("no entries", errors.getvalue())
            self.assertFalse(state_path.exists())

    def test_directory_sync_tolerates_an_unopenable_directory(self) -> None:
        watcher._fsync_directory(Path("/nonexistent-lapis-directory"))

    def test_new_release_is_reported_then_cleared_by_record(self) -> None:
        newer = FEED.replace(
            b"<item>",
            b"<item>\n      <title>2.1.285</title>\n      <guid>abc125</guid>\n    </item>\n    <item>",
            1,
        )
        with tempfile.TemporaryDirectory() as directory:
            self.run_collect(directory, argv=("--record",))
            result, state_path = self.run_collect(directory, feed=newer)
            self.assertEqual(
                [entry["guid"] for entry in result["new_entries"]], ["abc125"]
            )
            self.run_collect(directory, feed=newer, argv=("--record",))
            result, _ = self.run_collect(directory, feed=newer)
            self.assertEqual(result["new_entries"], [])

    def test_corrupt_state_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            state_path.write_text("not json", encoding="utf-8")
            with patch.object(
                watcher, "fetch_feed", side_effect=AssertionError("must not fetch")
            ):
                with self.assertRaises(ChangelogError):
                    watcher.collect(watcher.parse_args(["--state", str(state_path)]))

    def test_malformed_baseline_entries_are_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            state_path.write_text('{"entries": [{"guid": 123}]}', encoding="utf-8")
            with self.assertRaises(ChangelogError):
                watcher.load_state(state_path)

    def test_failed_record_preserves_baseline_and_cleans_temporary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            recorded_state(state_path, ["abc123"])
            argv = ["--json", "--record", "--state", str(state_path)]
            errors = io.StringIO()
            with patch.object(watcher, "fetch_feed", return_value=FEED):
                with patch.object(
                    watcher.os,
                    "replace",
                    side_effect=OSError("simulated disk-full"),
                ):
                    with redirect_stderr(errors):
                        self.assertEqual(watcher.main(argv), 1)
            self.assertIn("error:", errors.getvalue())
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual([entry["guid"] for entry in state["entries"]], ["abc123"])
            self.assertIn("simulated disk-full", errors.getvalue())
            self.assertEqual(list(state_path.parent.glob(".*.tmp")), [])

    def test_failed_cleanup_preserves_original_record_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            recorded_state(state_path, ["abc123"])
            argv = ["--json", "--record", "--state", str(state_path)]
            errors = io.StringIO()
            with patch.object(watcher, "fetch_feed", return_value=FEED):
                with (
                    patch.object(
                        watcher.os,
                        "replace",
                        side_effect=OSError("simulated disk-full"),
                    ),
                    patch.object(
                        watcher.Path,
                        "unlink",
                        side_effect=OSError("cleanup denied"),
                    ),
                ):
                    with redirect_stderr(errors):
                        self.assertEqual(watcher.main(argv), 1)
            self.assertIn("simulated disk-full", errors.getvalue())
            self.assertIn("left behind: cleanup denied", errors.getvalue())
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual([entry["guid"] for entry in state["entries"]], ["abc123"])

    def test_main_reports_fetch_failure_with_exit_code_one(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            argv = ["--json", "--state", str(Path(directory) / "claude-code.json")]
            with patch.object(
                watcher, "fetch_feed", side_effect=ChangelogError("offline")
            ):
                self.assertEqual(watcher.main(argv), 1)


if __name__ == "__main__":
    unittest.main()
