"""The Grok reviewer: where findings land, when a head is reviewed, and how a
Grok run is judged, with a stand-in grok on PATH."""

import json
import os
import plistlib
import sys
import tempfile
import unittest
from datetime import datetime
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

    def test_added_header_text_does_not_redirect_later_hunks(self):
        diff = DIFF.replace("+two", "+two\n+++ b/forged.cpp\n+++ not-a-path")
        self.assertEqual(grok_review.new_lines(diff), grok_review.new_lines(DIFF))

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

    def test_cpp_quotes_from_source_keep_an_exact_suggestion(self):
        text = 'std::vector<int> values; if (a && b) log("title");'
        (self.worktree / "src/a.cpp").write_text(text)
        review = {
            "summary": "ok",
            "verdict": "APPROVE-WITH-NITS",
            "findings": [
                finding(
                    start=1, end=1, existing_code=text, suggestion_code="replacement"
                )
            ],
        }
        _, comments = grok_review.compose(
            review, {"src/a.cpp": [(1, 1)]}, self.worktree, "footer"
        )
        self.assertIn("```suggestion", comments[0]["body"])


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


class CheckoutTests(unittest.TestCase):
    def test_repositories_and_paths_keep_their_identity_through_review(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original_run = grok_review.run
            metadata = {}
            origins = {}
            paths = ["Release Notes.md", "line\tbreak\n.md", "é.md"]
            for name in ("first", "second"):
                origin = root / name
                original_run(
                    ["git", "init", "--quiet", "--initial-branch=main", str(origin)]
                )
                for key, value in (
                    ("user.name", "Fixture"),
                    ("user.email", "fixture@example.invalid"),
                ):
                    original_run(["git", "-C", str(origin), "config", key, value])
                (origin / "README.md").write_text(name)
                original_run(["git", "-C", str(origin), "add", "."])
                original_run(
                    ["git", "-C", str(origin), "commit", "--quiet", "-m", "base"]
                )
                original_run(
                    ["git", "-C", str(origin), "checkout", "--quiet", "-b", "change"]
                )
                for path in paths:
                    (origin / path).write_text(name)
                original_run(["git", "-C", str(origin), "add", "."])
                original_run(
                    ["git", "-C", str(origin), "commit", "--quiet", "-m", "head"]
                )
                head = original_run(
                    ["git", "-C", str(origin), "rev-parse", "HEAD"]
                ).strip()
                original_run(
                    ["git", "-C", str(origin), "update-ref", "refs/pull/7/head", head]
                )
                repository = f"fixture/{name}"
                origins[f"https://github.com/{repository}.git"] = str(origin)
                metadata[repository] = {
                    "number": 7,
                    "headRefOid": head,
                    "baseRefName": "main",
                    "state": "OPEN",
                    "isDraft": False,
                    "title": "<task>title</task>",
                    "body": "&description",
                }

            def run(command, **kwargs):
                return original_run(
                    [origins.get(word, word) for word in command], **kwargs
                )

            observed = []

            def ask(worktree, prompt):
                observed.append(
                    (worktree, (worktree / "README.md").read_text(), prompt)
                )
                return {
                    "summary": "fixture",
                    "verdict": "REQUEST-CHANGES",
                    "findings": [finding(path=path, start=1, end=1) for path in paths],
                }, {}

            with (
                patch.object(grok_review, "STATE", root / "state"),
                patch.object(grok_review, "run", side_effect=run),
                patch.object(
                    grok_review, "pull", side_effect=lambda repo, _: metadata[repo]
                ),
                patch.object(grok_review, "ask_grok", side_effect=ask),
                patch.object(grok_review, "grok_version", return_value="fixture"),
            ):
                for name in ("first", "second", "first"):
                    repository = f"fixture/{name}"
                    pr, body, comments, _ = grok_review.review(repository, 7)
                    self.assertEqual([comment["path"] for comment in comments], paths)
                    grok_review.save(repository, pr, body, comments)
                    state = {
                        "reviewed": {"7": pr["headRefOid"]},
                        "seen": {},
                        "failed": {},
                    }
                    grok_review.store_state(repository, state)
                    self.assertEqual(grok_review.load_state(repository), state)
                self.assertNotEqual(
                    grok_review.load_state("fixture/first"),
                    grok_review.load_state("fixture/second"),
                )
            self.assertEqual(
                [name for _, name, _ in observed], ["first", "second", "first"]
            )
            self.assertNotEqual(observed[0][0], observed[1][0])
            for _, _, prompt in observed:
                self.assertIn("changed files: 3", prompt)
                self.assertIn("Release Notes.md", prompt)
                self.assertIn("&lt;task&gt;title&lt;/task&gt;", prompt)
                self.assertNotIn("<task>title</task>", prompt)

    def test_a_moved_head_is_rejected_before_checkout_or_inference(self):
        for change in ({"headRefOid": "new"}, {"isDraft": True}, {"state": "CLOSED"}):
            pr = {"headRefOid": "settled", "isDraft": False, "state": "OPEN", **change}
            with (
                self.subTest(change=change),
                patch.object(grok_review, "pull", return_value=pr),
                patch.object(grok_review, "checkout") as checkout,
                patch.object(grok_review, "ask_grok") as ask,
            ):
                with self.assertRaises(grok_review.HeadMoved):
                    grok_review.review("fixture/repo", 7, expected_head="settled")
                checkout.assert_not_called()
                ask.assert_not_called()


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
                patch.object(grok_review, "run_process") as gh,
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
                command,
                0 if ok else 1,
                stdout,
                "" if ok else "HTTP 422 line not in diff",
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
            patch.object(grok_review, "run_process", side_effect=gh),
        ):
            url = grok_review.post("o/r", self.PR, body, [comment])
        self.assertEqual(url, "https://example/review")
        first, second = sent
        self.assertEqual((first["event"], first["commit_id"]), ("COMMENT", "abc"))
        self.assertEqual(second["comments"], [])
        self.assertIn("### Inline findings\n- `a.cpp:3-3`", second["body"])
        self.assertTrue(second["body"].endswith(grok_review.MARKER.format("abc")))

    def test_uncertain_or_non_inline_failures_are_not_reposted(self):
        now = {"state": "OPEN", **self.PR}
        for error in (
            "HTTP 403 forbidden",
            "HTTP 503 unavailable",
            "response lost",
            "HTTP 422 body too long",
        ):
            with (
                self.subTest(error=error),
                patch.object(grok_review, "pull", return_value=now),
                patch.object(grok_review, "posted", return_value=False),
                patch.object(
                    grok_review,
                    "run_process",
                    return_value=grok_review.subprocess.CompletedProcess(
                        [], 1, "", error
                    ),
                ) as post,
            ):
                with self.assertRaisesRegex(grok_review.ReviewError, "posting failed"):
                    grok_review.post(
                        "o/r",
                        self.PR,
                        "body",
                        [{"path": "a.cpp", "line": 1, "body": "x"}],
                    )
                self.assertEqual(post.call_count, 1)


class CommandTests(unittest.TestCase):
    def test_stalled_and_oversized_helpers_release_the_review_lock(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(grok_review, "STATE", Path(directory)),
        ):
            for script, message in (
                ("import time; time.sleep(30)", "timeout"),
                ("import os; os.write(1,b'x'*65536)", "stdout exceeds"),
            ):
                with self.subTest(message=message):
                    with self.assertRaisesRegex(grok_review.ReviewError, message):
                        with grok_review.exclusive():
                            grok_review.run_process(
                                [sys.executable, "-c", script],
                                timeout=0.1,
                                max_output=1024,
                            )
                    with grok_review.exclusive():
                        pass
            script = "import sys; data=sys.stdin.buffer.read(); sys.stdout.buffer.write(data); sys.stderr.write('diagnostic')"
            original_read, original_write = os.read, os.write
            original_popen = grok_review.subprocess.Popen
            owned = {"read": set(), "write": set()}
            blocked = set()

            def capture_process(*args, **kwargs):
                process = original_popen(*args, **kwargs)
                owned["read"] = {process.stdout.fileno(), process.stderr.fileno()}
                owned["write"] = {process.stdin.fileno()}
                return process

            def once_unready(name, function, fd, value):
                if (
                    fd in owned[name]
                    and not os.get_blocking(fd)
                    and name not in blocked
                ):
                    blocked.add(name)
                    raise BlockingIOError("fixture readiness changed")
                return function(fd, value)

            with (
                patch.object(
                    grok_review.subprocess, "Popen", side_effect=capture_process
                ),
                patch.object(
                    grok_review.os,
                    "read",
                    side_effect=lambda fd, size: once_unready(
                        "read", original_read, fd, size
                    ),
                ),
                patch.object(
                    grok_review.os,
                    "write",
                    side_effect=lambda fd, data: once_unready(
                        "write", original_write, fd, data
                    ),
                ),
            ):
                result = grok_review.run_process(
                    [sys.executable, "-c", script], input="payload" * 10000
                )
            self.assertEqual(result.stdout, "payload" * 10000)
            self.assertEqual(result.stderr, "diagnostic")
            self.assertEqual(blocked, {"read", "write"})


class GrokRunTests(unittest.TestCase):
    """A stand-in grok records its arguments and environment and answers."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        state = patch.object(grok_review, "STATE", self.root / "state")
        state.start()
        self.addCleanup(state.stop)
        (self.root / "bin").mkdir()
        self.worktree = self.root / "work"
        self.worktree.mkdir()
        path = f"{self.root / 'bin'}{os.pathsep}{os.environ['PATH']}"
        patcher = patch.dict(
            os.environ,
            {
                "PATH": path,
                "GH_TOKEN": "secret",
                "AWS_SECRET_ACCESS_KEY": "cloud-secret",
                "UNRELATED_API_KEY": "provider-secret",
                "SSH_AUTH_SOCK": "/private/agent.sock",
            },
        )
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
            arguments[arguments.index("--tools") + 1], "read_file,grep,list_dir"
        )
        self.assertEqual(
            arguments[arguments.index("--disallowed-tools") + 1], "search_tool,use_tool"
        )
        self.assertEqual(arguments[arguments.index("--deny") + 1], "MCPTool")
        # The prompt file is the last option: grok takes the next word as the prompt.
        self.assertEqual(arguments[-2], "--prompt-file")
        json.loads(arguments[arguments.index("--json-schema") + 1])
        environment = (self.root / "environment").read_text()
        for secret in (
            "GH_TOKEN=",
            "AWS_SECRET_ACCESS_KEY=",
            "UNRELATED_API_KEY=",
            "SSH_AUTH_SOCK=",
        ):
            self.assertNotIn(secret, environment)
        self.assertIn("HOME=", environment)
        self.assertIn("GH_CONFIG_DIR=", environment)

    def test_failures_and_unfinished_runs_are_errors(self):
        self.grok(
            {"type": "error", "message": "API error (status 429): usage limit"}, 1
        )
        with self.assertRaises(grok_review.ReviewError) as caught:
            grok_review.ask_grok(self.worktree, "brief")
        self.assertTrue(caught.exception.quota)
        # The second failure is evaluated after the account cooldown expires.
        (grok_review.STATE / "quota.json").unlink()
        self.grok({"stopReason": "max_turns", "structuredOutput": None})
        with self.assertRaises(grok_review.ReviewError) as caught:
            grok_review.ask_grok(self.worktree, "brief")
        self.assertFalse(caught.exception.quota)
        self.assertIn("max_turns", str(caught.exception))

    def test_authentication_and_request_errors_do_not_pause_the_account(self):
        for message in (
            "refresh your credentials",
            "token limit exceeded",
            "request 14293 failed",
        ):
            with self.subTest(message=message):
                self.grok({"type": "error", "message": message}, 1)
                with self.assertRaises(grok_review.ReviewError) as caught:
                    grok_review.ask_grok(self.worktree, "brief")
                self.assertFalse(caught.exception.quota)
                self.assertEqual(grok_review.quota_deadline(), 0)

    def test_malformed_structured_results_use_normal_error_handling(self):
        good = {"summary": "ok", "verdict": "APPROVE", "findings": []}
        bad = (
            {},
            {**good, "findings": [None]},
            {**good, "verdict": 1},
            {**good, "findings": [finding(start=True)]},
        )
        for result in bad:
            with self.subTest(result=result):
                self.grok({"stopReason": "end_turn", "structuredOutput": result})
                with self.assertRaisesRegex(
                    grok_review.ReviewError, "structured output"
                ):
                    grok_review.ask_grok(self.worktree, "brief")

        for value, schema in (
            (True, {"type": "boolean"}),
            ([], {"type": "array"}),
            (None, {}),
        ):
            with (
                self.subTest(schema=schema),
                self.assertRaises(grok_review.ReviewError),
            ):
                grok_review.validate_review(value, schema)

    def test_quota_pauses_all_repositories_without_spending_pr_attempts(self):
        now = [datetime(2026, 1, 10, 12).timestamp()]
        pr = {"number": 7, "headRefOid": "abc", "isDraft": False}
        state = {
            "reviewed": {},
            "seen": {"7:abc": now[0] - grok_review.SETTLE},
            "failed": {},
        }
        for repo in ("fixture/first", "fixture/second"):
            grok_review.store_state(repo, state)
        self.grok({"type": "error", "message": "free Grok Build usage limit"}, 1)

        def review(repository, number, expected_head=None):
            self.assertEqual(expected_head, "abc")
            grok_review.ask_grok(self.worktree, "brief")
            return pr, "body", [], {}

        with (
            patch.object(grok_review.time, "time", side_effect=lambda: now[0]),
            patch.object(grok_review, "run", return_value=json.dumps([pr])) as listing,
            patch.object(grok_review, "review", side_effect=review) as reviewing,
        ):
            grok_review.watch("fixture/first", False)
            deadline = grok_review.quota_deadline()
            self.assertEqual(deadline, datetime(2026, 1, 11).timestamp())
            self.assertEqual(grok_review.load_state("fixture/first")["failed"], {})
            now[0] = deadline - 1
            for repo in ("fixture/first", "fixture/second"):
                grok_review.watch(repo, False)
            self.assertEqual(listing.call_count, 1)
            self.assertEqual(reviewing.call_count, 1)
            self.assertEqual(grok_review.quota_deadline(), deadline)
            self.grok(
                {
                    "stopReason": "end_turn",
                    "structuredOutput": {
                        "summary": "ok",
                        "verdict": "APPROVE",
                        "findings": [],
                    },
                }
            )
            now[0] = deadline
            grok_review.watch("fixture/first", False)
            self.assertEqual(
                grok_review.load_state("fixture/first")["reviewed"], {"7": "abc"}
            )
            self.assertEqual(reviewing.call_count, 2)

    def test_watch_does_not_publish_or_spend_an_attempt_when_the_head_moves(self):
        now = 10000
        pr = {"number": 7, "headRefOid": "old", "isDraft": False}
        grok_review.store_state(
            "fixture/repo",
            {
                "reviewed": {},
                "seen": {"7:old": now - grok_review.SETTLE},
                "failed": {},
            },
        )
        with (
            patch.object(grok_review.time, "time", return_value=now),
            patch.object(grok_review, "run", return_value=json.dumps([pr])),
            patch.object(
                grok_review, "review", side_effect=grok_review.HeadMoved("moved")
            ) as review,
            patch.object(grok_review, "posted", return_value=False),
            patch.object(grok_review, "post") as post,
            patch.object(grok_review, "save") as save,
        ):
            grok_review.watch("fixture/repo", True)
            review.assert_called_once_with("fixture/repo", 7, expected_head="old")
            post.assert_not_called()
            save.assert_not_called()
            self.assertEqual(grok_review.load_state("fixture/repo")["failed"], {})


class AdmissionTests(unittest.TestCase):
    def test_install_preserves_the_explicit_model_without_launching_services(self):
        with tempfile.TemporaryDirectory() as directory:
            plist = Path(directory) / "review.plist"
            with (
                patch.object(grok_review, "PLIST", plist),
                patch.object(grok_review, "MODEL", "fixture-default"),
                patch.object(
                    grok_review.shutil, "which", return_value="/usr/bin/fixture"
                ),
                patch.object(
                    grok_review,
                    "run_process",
                    side_effect=[
                        grok_review.subprocess.CompletedProcess(
                            [], 1, "", "not loaded"
                        ),
                        grok_review.subprocess.CompletedProcess([], 0, "", ""),
                        grok_review.subprocess.CompletedProcess(
                            [], 1, "", "not loaded"
                        ),
                    ],
                ) as launch,
                patch.object(
                    sys,
                    "argv",
                    [
                        "grok_review.py",
                        "--repo",
                        "fixture/repo",
                        "--model",
                        "fixture-model",
                        "install",
                    ],
                ),
            ):
                grok_review.main()
                arguments = plistlib.loads(plist.read_bytes())["ProgramArguments"]
                self.assertEqual(
                    arguments[2:],
                    ["--repo", "fixture/repo", "--model", "fixture-model", "watch"],
                )
                self.assertEqual(launch.call_count, 2)
                grok_review.uninstall()
                domain = f"gui/{os.getuid()}"
                self.assertEqual(
                    [c.args[0] for c in launch.call_args_list],
                    [
                        ["launchctl", "bootout", f"{domain}/{grok_review.LABEL}"],
                        ["launchctl", "bootstrap", domain, str(plist)],
                        ["launchctl", "bootout", f"{domain}/{grok_review.LABEL}"],
                    ],
                )
                self.assertFalse(plist.exists())
                launch.side_effect = [
                    grok_review.subprocess.CompletedProcess([], 1, "", "not loaded"),
                    grok_review.subprocess.CompletedProcess(
                        [], 1, "", "bootstrap refused"
                    ),
                ]
                with self.assertRaisesRegex(
                    grok_review.ReviewError, "bootstrap refused"
                ):
                    grok_review.install("fixture/repo", False)
                launch.side_effect = grok_review.ReviewError("helper timeout")
                with self.assertRaisesRegex(
                    grok_review.ReviewError, "running job may need stopping"
                ):
                    grok_review.uninstall()
                self.assertFalse(plist.exists())

    def test_only_repository_writers_are_admitted(self):
        for permission in ("admin", "maintain", "write", "read", "triage", None):
            with self.subTest(permission=permission):
                metadata = {"number": 7, "author": {"login": "fixture-author"}}
                with patch.object(
                    grok_review,
                    "run",
                    side_effect=[
                        json.dumps(metadata),
                        json.dumps({"permission": permission}),
                    ],
                ):
                    if permission in {"admin", "maintain", "write"}:
                        self.assertEqual(grok_review.pull("org/repo", 7), metadata)
                    else:
                        with self.assertRaises(grok_review.ReviewError):
                            grok_review.pull("org/repo", 7)

    def test_missing_or_unverifiable_author_fails_closed(self):
        for author in (None, {}, {"login": "../other"}):
            with (
                self.subTest(author=author),
                patch.object(
                    grok_review, "run", return_value=json.dumps({"author": author})
                ),
            ):
                with self.assertRaises(grok_review.ReviewError):
                    grok_review.pull("org/repo", 7)
        with patch.object(
            grok_review,
            "run",
            side_effect=[
                json.dumps({"author": {"login": "fixture-author"}}),
                grok_review.ReviewError("permission API unavailable"),
            ],
        ):
            with self.assertRaisesRegex(grok_review.ReviewError, "permission API"):
                grok_review.pull("org/repo", 7)

    def test_an_overlapping_review_does_not_queue(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(grok_review, "STATE", Path(directory)),
        ):
            with grok_review.exclusive():
                with self.assertRaisesRegex(grok_review.ReviewError, "already running"):
                    with grok_review.exclusive():
                        self.fail("A second review acquired the first review's lock")
            with grok_review.exclusive():
                pass  # the owner releases its lock when it exits


if __name__ == "__main__":
    unittest.main()
