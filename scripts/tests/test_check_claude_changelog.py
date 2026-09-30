"""Unit coverage for the Claude Code changelog watcher."""

import json
import io
import sys
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from unittest.mock import patch
from contextlib import redirect_stderr, redirect_stdout

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


class FetchFeedTests(unittest.TestCase):
    def test_rejects_body_over_named_limit_before_parsing(self) -> None:
        response = _FeedResponse(b"<rss>too-large</rss>")
        with (
            patch.object(watcher, "MAX_FEED_BYTES", 4),
            patch.object(watcher.urllib.request, "urlopen", return_value=response),
        ):
            with self.assertRaisesRegex(ChangelogError, "4-byte limit"):
                watcher.fetch_feed("https://example.invalid/feed", 0.1)
        self.assertEqual(response.read_size, 5)


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
            self.assertIn("temporary cleanup failed: cleanup denied", errors.getvalue())
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
