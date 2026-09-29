"""The Grok reviewer: where findings land, when a head is reviewed, and how a
Grok run is judged, with a stand-in grok on PATH."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import grok_review  # noqa: E402

DIFF = """diff --git a/src/a.cpp b/src/a.cpp
--- a/src/a.cpp
+++ b/src/a.cpp
@@ -10,0 +11,3 @@ int f()
+one
+two
+three
@@ -20 +23 @@ int g()
-old
+new
@@ -30,2 +33,0 @@ int h()
-gone
-gone
diff --git a/docs/b.md b/docs/b.md
new file mode 100644
--- /dev/null
+++ b/docs/b.md
@@ -0,0 +1,2 @@
+# b
+text
diff --git a/old.txt b/old.txt
deleted file mode 100644
--- a/old.txt
+++ /dev/null
@@ -1 +0,0 @@
-bye
"""


def finding(path="src/a.cpp", start=11, end=12, **extra):
    return {
        "severity": "warning",
        "category": "correctness",
        "path": path,
        "start_line": start,
        "end_line": end,
        "title": "Off by one",
        "detail": "The loop stops early.",
        **extra,
    }


class LineTests(unittest.TestCase):
    def test_changed_lines_at_the_head(self):
        self.assertEqual(
            grok_review.new_lines(DIFF),
            {"src/a.cpp": [(11, 13), (23, 23)], "docs/b.md": [(1, 2)]},
        )

    def test_findings_anchor_only_to_changed_lines(self):
        ranges = grok_review.new_lines(DIFF)
        self.assertEqual(grok_review.anchor(ranges, finding()), (11, 12))
        self.assertEqual(
            grok_review.anchor(ranges, finding(start=13, end=11)), (11, 13)
        )
        # Reaching back before the change keeps only its last, changed line.
        self.assertEqual(grok_review.anchor(ranges, finding(start=5, end=12)), (12, 12))
        self.assertEqual(
            grok_review.anchor(ranges, finding(start=23, end=23)), (23, 23)
        )
        self.assertIsNone(grok_review.anchor(ranges, finding(start=2, end=4)))
        self.assertIsNone(
            grok_review.anchor(ranges, finding(path="old.txt", start=1, end=1))
        )
        self.assertEqual(
            grok_review.anchor(ranges, finding(path="./docs/b.md", start=2, end=2)),
            (2, 2),
        )


class ComposeTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.worktree = Path(folder.name)
        (self.worktree / "src").mkdir()
        lines = [f"line {number}" for number in range(1, 40)]
        lines[10:13] = ["one", "two", "three"]
        (self.worktree / "src" / "a.cpp").write_text("\n".join(lines) + "\n")

    def test_inline_and_summary_findings(self):
        review = {
            "summary": "Mostly fine.",
            "verdict": "APPROVE-WITH-NITS",
            "findings": [
                finding(existing_code="one\ntwo", suggestion_code="uno\ndos"),
                finding(
                    start=23, end=23, existing_code="not there", suggestion_code="x"
                ),
                finding(start=2, end=3, severity="suggestion", title="Old code"),
            ],
        }
        ranges = grok_review.new_lines(DIFF)
        body, comments = grok_review.compose(review, ranges, self.worktree, "footer")
        self.assertIn(
            "APPROVE-WITH-NITS · 0 critical, 2 warning, 1 suggestion · 2 inline", body
        )
        self.assertIn("Mostly fine.", body)
        self.assertIn("### Outside the changed lines", body)
        self.assertIn("`src/a.cpp:2-3`: Old code.", body)
        self.assertTrue(body.endswith("footer"))
        first, second = comments
        self.assertEqual(
            {key: first[key] for key in ("path", "line", "side", "start_line")},
            {"path": "src/a.cpp", "line": 12, "side": "RIGHT", "start_line": 11},
        )
        # A suggestion GitHub can apply only replaces exactly the lines it quotes.
        self.assertIn("```suggestion\nuno\ndos\n```", first["body"])
        self.assertNotIn("start_line", second)
        self.assertIn("```\nx\n```", second["body"])
        self.assertNotIn("```suggestion", second["body"])

    def test_a_path_outside_the_checkout_is_not_read(self):
        self.assertIsNone(grok_review.lines_at(self.worktree, "../../etc/hosts", 1, 1))


class DueTests(unittest.TestCase):
    def test_a_head_settles_then_is_reviewed_once_and_retried_twice(self):
        state = {"reviewed": {}, "seen": {}, "failed": {}}
        self.assertFalse(grok_review.due(state, 7, "abc", 1000))
        self.assertFalse(
            grok_review.due(state, 7, "abc", 1000 + grok_review.SETTLE - 1)
        )
        self.assertTrue(grok_review.due(state, 7, "abc", 1000 + grok_review.SETTLE))
        state["failed"]["7:abc"] = {"count": 1, "at": 2000}
        self.assertFalse(grok_review.due(state, 7, "abc", 2000 + grok_review.RETRY - 1))
        self.assertTrue(grok_review.due(state, 7, "abc", 2000 + grok_review.RETRY))
        state["failed"]["7:abc"] = {"count": grok_review.ATTEMPTS, "at": 2000}
        self.assertFalse(grok_review.due(state, 7, "abc", 99999))
        state["reviewed"]["7"] = "abc"
        self.assertFalse(grok_review.due(state, 7, "abc", 99999))
        # A new head starts its own wait.
        self.assertFalse(grok_review.due(state, 7, "def", 99999))


class PostTests(unittest.TestCase):
    """A review is posted only for the head and base Grok read, on an open PR."""

    PR = {"number": 7, "headRefOid": "abc", "baseRefName": "main"}

    def test_a_moved_or_closed_pr_is_not_posted_to(self):
        for now in (
            {"state": "OPEN", "headRefOid": "def", "baseRefName": "main"},
            {"state": "OPEN", "headRefOid": "abc", "baseRefName": "release"},
            {"state": "MERGED", "headRefOid": "abc", "baseRefName": "main"},
        ):
            with (
                self.subTest(now=now),
                patch.object(grok_review, "pull", return_value=now),
                patch.object(grok_review.subprocess, "run") as gh,
            ):
                self.assertIsNone(grok_review.post("o/r", self.PR, "body", []))
                gh.assert_not_called()

    def test_an_unplaceable_inline_comment_moves_into_the_body(self):
        now = {"state": "OPEN", "headRefOid": "abc", "baseRefName": "main"}
        sent = []

        def gh(command, input=None, **_):
            sent.append(json.loads(input))
            ok = len(sent) > 1
            stdout = json.dumps({"html_url": "https://example/review"}) if ok else ""
            return grok_review.subprocess.CompletedProcess(
                command, 0 if ok else 1, stdout, "" if ok else "422 line not in diff"
            )

        comment = {
            "path": "a.cpp",
            "line": 3,
            "side": "RIGHT",
            "body": "**warning: x**",
        }
        body = "summary\n\n" + grok_review.MARKER.format("abc")
        with (
            patch.object(grok_review, "pull", return_value=now),
            patch.object(grok_review, "posted", return_value=False),
            patch.object(grok_review.subprocess, "run", side_effect=gh),
        ):
            url = grok_review.post("o/r", self.PR, body, [comment])
        self.assertEqual(url, "https://example/review")
        first, second = sent
        self.assertEqual((first["event"], first["commit_id"]), ("COMMENT", "abc"))
        self.assertEqual(second["comments"], [])
        self.assertIn("### Inline findings\n- `a.cpp:3-3`", second["body"])
        self.assertTrue(second["body"].endswith(grok_review.MARKER.format("abc")))


class GrokRunTests(unittest.TestCase):
    """A stand-in grok records its arguments and environment and answers."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        (self.root / "bin").mkdir()
        self.worktree = self.root / "work"
        self.worktree.mkdir()
        path = f"{self.root / 'bin'}{os.pathsep}{os.environ['PATH']}"
        patcher = patch.dict(os.environ, {"PATH": path, "GH_TOKEN": "secret"})
        patcher.start()
        self.addCleanup(patcher.stop)

    def grok(self, envelope, status=0):
        script = self.root / "bin" / "grok"
        script.write_text(
            "#!/bin/sh\n"
            f'printf "%s\\n" "$@" > "{self.root}/arguments"\n'
            f'env > "{self.root}/environment"\n'
            f"cat <<'EOF'\n{json.dumps(envelope)}\nEOF\n"
            f"exit {status}\n"
        )
        script.chmod(0o755)

    def test_a_finished_review_is_read_only_and_credential_free(self):
        review = {"summary": "ok", "verdict": "APPROVE", "findings": []}
        self.grok(
            {"stopReason": "end_turn", "structuredOutput": review, "num_turns": 3}
        )
        result, envelope = grok_review.ask_grok(self.worktree, "the brief")
        self.assertEqual((result, envelope["num_turns"]), (review, 3))
        arguments = (self.root / "arguments").read_text().splitlines()
        self.assertEqual(arguments[arguments.index("--model") + 1], grok_review.MODEL)
        self.assertEqual(arguments[arguments.index("--reasoning-effort") + 1], "xhigh")
        self.assertEqual(
            arguments[arguments.index("--disallowed-tools") + 1], "write,search_replace"
        )
        denied = [arguments[i + 1] for i, a in enumerate(arguments) if a == "--deny"]
        self.assertIn("Bash(git push*)", denied)
        self.assertIn("Bash(gh *)", denied)
        # The prompt file is the last option: grok takes the next word as the prompt.
        self.assertEqual(arguments[-2], "--prompt-file")
        json.loads(arguments[arguments.index("--json-schema") + 1])
        environment = (self.root / "environment").read_text()
        self.assertNotIn("GH_TOKEN=", environment)
        self.assertIn("GH_CONFIG_DIR=", environment)

    def test_failures_and_unfinished_runs_are_errors(self):
        self.grok(
            {"type": "error", "message": "API error (status 429): usage limit"}, 1
        )
        with self.assertRaises(grok_review.ReviewError) as caught:
            grok_review.ask_grok(self.worktree, "brief")
        self.assertTrue(caught.exception.quota)
        self.grok({"stopReason": "max_turns", "structuredOutput": None})
        with self.assertRaises(grok_review.ReviewError) as caught:
            grok_review.ask_grok(self.worktree, "brief")
        self.assertFalse(caught.exception.quota)
        self.assertIn("max_turns", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
