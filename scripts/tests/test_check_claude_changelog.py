"""Unit coverage for the Claude Code changelog watcher."""

import json
import sys
import tempfile
import unittest
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


def recorded_state(path, guids):
    watcher.record_state(
        path,
        watcher.FEED_URL,
        [
            watcher.Entry(guid, f"2.1.{guid}", "https://example.invalid", None, None)
            for guid in guids
        ],
    )


class ParseFeedTests(unittest.TestCase):
    def test_parses_rss_items_with_cdata_and_entities(self):
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

    def test_summary_uses_description_when_content_encoded_is_missing(self):
        entries = parse_feed(FEED)
        self.assertEqual(entries[1].summary, "Added and removed things")

    def test_rejects_invalid_xml(self):
        with self.assertRaises(ChangelogError):
            parse_feed(b"<rss><channel>")


class NewEntriesTests(unittest.TestCase):
    def entries(self):
        return parse_feed(FEED)

    def test_without_state_reports_nothing(self):
        self.assertEqual(watcher.new_entries(self.entries(), None), [])

    def test_reports_only_unknown_guids_in_feed_order(self):
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            recorded_state(state_path, ["abc123"])
            state = watcher.load_state(state_path)
            fresh = watcher.new_entries(self.entries(), state)
        self.assertEqual([entry.guid for entry in fresh], ["abc124"])


class CollectTests(unittest.TestCase):
    def run_collect(self, directory, feed=FEED, argv=()):
        state_path = Path(directory) / "claude-code.json"
        argv = ["--state", str(state_path), *argv]
        with patch.object(watcher, "fetch_feed", return_value=feed):
            result = watcher.collect(watcher.parse_args(argv))
        return result, state_path

    def test_first_run_initializes_baseline_without_reporting(self):
        with tempfile.TemporaryDirectory() as directory:
            result, state_path = self.run_collect(directory)
            self.assertTrue(result["initialized"])
            self.assertEqual(result["new_entries"], [])
            self.assertFalse(state_path.exists())

    def test_record_persists_baseline_and_next_run_is_clean(self):
        with tempfile.TemporaryDirectory() as directory:
            _, state_path = self.run_collect(directory, argv=("--record",))
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(
                [entry["guid"] for entry in state["entries"]], ["abc124", "abc123"]
            )
            result, _ = self.run_collect(directory)
            self.assertFalse(result["initialized"])
            self.assertEqual(result["new_entries"], [])

    def test_new_release_is_reported_then_cleared_by_record(self):
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

    def test_corrupt_state_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "claude-code.json"
            state_path.write_text("not json", encoding="utf-8")
            with patch.object(watcher, "fetch_feed", return_value=FEED):
                with self.assertRaises(ChangelogError):
                    watcher.collect(watcher.parse_args(["--state", str(state_path)]))

    def test_main_reports_fetch_failure_with_exit_code_one(self):
        with tempfile.TemporaryDirectory() as directory:
            argv = ["--json", "--state", str(Path(directory) / "claude-code.json")]
            with patch.object(
                watcher, "fetch_feed", side_effect=ChangelogError("offline")
            ):
                self.assertEqual(watcher.main(argv), 1)


if __name__ == "__main__":
    unittest.main()
