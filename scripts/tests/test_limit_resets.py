"""When lapis spends a saved Claude Code or Codex limit reset: OMP's restore and
salvage rules, and reading the accounts' own reports."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps" / "desktop" / "src"))
import limit_resets  # noqa: E402

HOUR = 3600.0
NOW = 1_800_000_000.0
SETTINGS = {"min_blocked_minutes": 60, "keep": 0, "salvage_hours": 12}


def credit(**changes):
    value = {
        "id": "launch",
        "program": limit_resets.CEDAR,
        "title": "A reset",
        "clears": ["five_hour", "seven_day"],
        "expires": NOW + 20 * 24 * HOUR,
        "remaining": 1,
        "usable": True,
        "requires_limit": False,
    }
    value.update(changes)
    return value


def plan(windows, spend=None, attempted=(), **settings):
    return limit_resets.plan(
        "claude",
        "someone@example.com",
        windows,
        credit() if spend is None else spend,
        NOW,
        dict(SETTINGS, **settings),
        set(attempted),
    )


class PlanTests(unittest.TestCase):
    def test_a_long_block_is_restored_once(self):
        windows = {
            "five_hour": (0.4, NOW + HOUR),
            "seven_day": (1.0, NOW + 3 * 24 * HOUR),
        }
        action, reason, key = plan(windows)
        self.assertEqual((action, reason), ("restore", "blocked"))
        self.assertIn("seven_day", key)
        self.assertEqual(
            plan(windows, attempted=[key])[:2], (None, "already-attempted")
        )

    def test_a_block_that_lifts_soon_keeps_the_reset(self):
        windows = {
            "five_hour": (1.0, NOW + 30 * 60),
            "seven_day": (0.5, NOW + 2 * 24 * HOUR),
        }
        self.assertEqual(plan(windows)[:2], (None, "reset-too-soon"))
        self.assertEqual(plan(windows, min_blocked_minutes=10)[0], "restore")

    def test_the_session_reset_clears_only_a_five_hour_block(self):
        session = credit(
            id=limit_resets.JUNIPER,
            program=limit_resets.JUNIPER,
            clears=["five_hour"],
            requires_limit=True,
        )
        five_hour = {
            "five_hour": (1.0, NOW + 3 * HOUR),
            "seven_day": (0.6, NOW + 2 * 24 * HOUR),
        }
        self.assertEqual(plan(five_hour, session)[0], "restore")
        weekly = {
            "five_hour": (1.0, NOW + 3 * HOUR),
            "seven_day": (1.0, NOW + 2 * 24 * HOUR),
        }
        self.assertEqual(plan(weekly, session)[:2], (None, "incomplete-coverage"))

    def test_a_reserve_is_kept_for_blocks_but_not_for_expiring_resets(self):
        blocked = {"seven_day": (1.0, NOW + 2 * 24 * HOUR)}
        self.assertEqual(plan(blocked, keep=1)[:2], (None, "reserve"))
        expiring = credit(expires=NOW + 6 * HOUR)
        self.assertEqual(
            plan({"seven_day": (0.5, NOW + HOUR)}, expiring, keep=1)[0], "salvage"
        )

    def test_an_expiring_reset_is_salvaged_only_when_it_restores_something(self):
        expiring = credit(expires=NOW + 6 * HOUR)
        self.assertEqual(
            plan({"seven_day": (0.3, NOW + 5 * HOUR)}, expiring)[0], "salvage"
        )
        self.assertEqual(
            plan({"seven_day": (0.1, NOW + 5 * HOUR)}, expiring)[:2],
            (None, "window-mostly-free"),
        )
        self.assertEqual(
            plan({"seven_day": (0.9, NOW + HOUR)})[:2], (None, "not-needed")
        )
        needs_wall = credit(expires=NOW + 6 * HOUR, requires_limit=True)
        self.assertEqual(
            plan({"seven_day": (0.9, NOW + HOUR)}, needs_wall)[:2],
            (None, "needs-limit"),
        )

    def test_unusable_missing_or_implausible_resets_are_left_alone(self):
        blocked = {"seven_day": (1.0, NOW + 2 * 24 * HOUR)}
        self.assertEqual(
            limit_resets.plan("claude", "e", blocked, None, NOW, SETTINGS, set())[:2],
            (None, "no-credit"),
        )
        self.assertEqual(
            plan(blocked, credit(usable=False))[:2], (None, "credit-unusable")
        )
        self.assertEqual(
            plan(blocked, credit(expires=NOW - 1))[:2], (None, "credit-expired")
        )
        far = {"seven_day": (1.0, NOW + 30 * 24 * HOUR)}
        self.assertEqual(plan(far)[:2], (None, "reset-implausible"))
        self.assertEqual(plan({"seven_day": (1.0, None)})[:2], (None, "no-reset-time"))

    def test_asking_spends_at_once(self):
        action, reason, key = limit_resets.plan(
            "claude",
            "e",
            {"seven_day": (0.2, NOW + HOUR)},
            credit(),
            NOW,
            SETTINGS,
            set(),
            True,
        )
        self.assertEqual((action, reason), ("now", "asked"))
        self.assertTrue(key.startswith("now|claude|e|launch|1|"))


def sweep_arguments(**changes):
    values = dict(
        min_blocked_minutes=60,
        keep=0,
        salvage_hours=12,
        attempted=[],
        apply=False,
        now=None,
        claude_credentials_stdin=False,
        claude_token_file=None,
        codex_home=None,
    )
    values.update(changes)
    return limit_resets.argparse.Namespace(**values)


class ModelCapTests(unittest.TestCase):
    def test_an_exhausted_model_cap_must_be_cleared_too(self):
        windows = {
            "five_hour": (1.0, NOW + 3 * HOUR),
            "seven_day": (0.7, NOW + 2 * 24 * HOUR),
            "seven_day_opus": (1.0, NOW + 2 * 24 * HOUR),
        }
        self.assertEqual(plan(windows)[:2], (None, "incomplete-coverage"))
        clears_opus = credit(clears=["five_hour", "seven_day", "seven_day_opus"])
        self.assertEqual(plan(windows, clears_opus)[0], "restore")

    def test_a_busy_model_cap_counts_toward_salvage(self):
        windows = {
            "seven_day": (0.1, NOW + 2 * 24 * HOUR),
            "seven_day_sonnet": (0.8, NOW + 2 * 24 * HOUR),
        }
        expiring = credit(expires=NOW + 6 * HOUR)
        self.assertEqual(plan(windows, expiring)[:2], ("salvage", "expiring"))


class ReportTests(unittest.TestCase):
    def test_claude_reports_its_windows_and_selected_reset(self):
        usage = {
            "five_hour": {
                "utilization": 50.0,
                "resets_at": "2026-09-29T06:20:00+00:00",
            },
            "seven_day": {
                "utilization": 88.0,
                "resets_at": "2026-10-02T02:00:00+00:00",
            },
            limit_resets.CEDAR: {
                "eligible": True,
                "next_grant_id": "launch",
                "grants": [
                    {
                        "id": "launch",
                        "label": "Launch reset",
                        "resets_left": 1,
                        "ends_at": "2026-10-22T16:00:00+00:00",
                        "clears": [
                            "five_hour",
                            "seven_day",
                            "seven_day_overage_included",
                        ],
                        "usable_now": True,
                        "use_requires_limit": False,
                    }
                ],
            },
        }
        windows = limit_resets.claude_windows(usage)
        self.assertEqual(windows["seven_day"][0], 0.88)
        spend = limit_resets.claude_credit(usage, None)
        self.assertEqual(
            (
                spend["id"],
                spend["program"],
                spend["clears"],
                spend["remaining"],
                spend["usable"],
            ),
            ("launch", limit_resets.CEDAR, ["five_hour", "seven_day"], 1, True),
        )

    def test_claude_falls_back_to_the_session_reset_at_the_wall(self):
        at_wall = {
            limit_resets.JUNIPER: {
                "eligible": True,
                "arm": "reset",
                "available": True,
                "weekly_resets_at": "2026-10-02T02:00:00+00:00",
            }
        }
        spend = limit_resets.claude_credit(
            {limit_resets.CEDAR: {"eligible": False}}, at_wall
        )
        self.assertEqual(
            (spend["program"], spend["clears"], spend["remaining"]),
            (limit_resets.JUNIPER, ["five_hour"], 1),
        )
        at_wall[limit_resets.JUNIPER]["arm"] = "control"
        self.assertIsNone(limit_resets.claude_credit({}, at_wall))

    def test_given_credentials_win_and_stale_ones_are_refused(self):
        fresh = {
            "claudeAiOauth": {"accessToken": "given", "expiresAt": (NOW + HOUR) * 1000}
        }
        with (
            patch.object(limit_resets.time, "time", return_value=NOW),
            patch.dict(limit_resets.os.environ, {"CLAUDE_CONFIG_DIR": "/nonexistent"}),
        ):
            self.assertEqual(limit_resets.claude_token(fresh), "given")
            stale = {
                "claudeAiOauth": {"accessToken": "old", "expiresAt": (NOW - 1) * 1000}
            }
            with self.assertRaises(limit_resets.Unavailable):
                limit_resets.claude_token(stale)

    def test_codex_reports_its_windows_and_soonest_reset(self):
        answers = {
            "/wham/usage": {
                "email": "someone@example.com",
                "rate_limit": {
                    "primary_window": {
                        "used_percent": 100,
                        "limit_window_seconds": 5 * HOUR,
                        "reset_at": NOW + 2 * HOUR,
                    },
                    "secondary_window": {
                        "used_percent": 40,
                        "limit_window_seconds": 7 * 24 * HOUR,
                        "reset_at": NOW + 3 * 24 * HOUR,
                    },
                },
            },
            "/wham/rate-limit-reset-credits": {
                "credits": [
                    {"title": "No id", "expires_at": NOW + HOUR},
                    {"id": "used", "status": "redeemed", "expires_at": NOW + HOUR},
                    {"id": "later", "expires_at": NOW + 9 * 24 * HOUR},
                    {
                        "id": "soon",
                        "title": "Banked",
                        "expires_at": NOW + 2 * 24 * HOUR,
                    },
                ]
            },
        }

        def answer(url, headers, body=None, timeout=15):
            return 200, answers[url.removeprefix(limit_resets.CODEX_API)]

        with tempfile.TemporaryDirectory() as home:
            Path(home, "auth.json").write_text(
                json.dumps({"tokens": {"access_token": "a", "account_id": "acct"}})
            )
            with patch.object(limit_resets, "request", answer):
                account = limit_resets.Codex(home)
                windows, spend = account.read()
        self.assertEqual(account.email, "someone@example.com")
        self.assertEqual(
            windows,
            {
                "five_hour": (1.0, NOW + 2 * HOUR),
                "seven_day": (0.4, NOW + 3 * 24 * HOUR),
            },
        )
        self.assertEqual(
            (spend["id"], spend["title"], spend["remaining"]), ("soon", "Banked", 2)
        )

    def test_a_retried_spend_reuses_its_request_id(self):
        sent = []

        def answer(url, headers, body=None, timeout=15):
            sent.append(body)
            return 200, {"result": "reset"}

        claude = limit_resets.Claude.__new__(limit_resets.Claude)
        claude.headers, claude.org = {}, "org"
        with patch.object(limit_resets, "request", answer):
            claude.spend(credit(), "block|k")
            claude.spend(credit(), "block|k")
            claude.spend(credit(), "block|other")
        ids = [body["request_id"] for body in sent]
        self.assertEqual(ids[0], ids[1])
        self.assertNotEqual(ids[0], ids[2])

    def test_a_lost_answer_is_a_spend_only_if_the_account_reads_as_reset(self):
        class Account:
            cli = "claude"
            took = False

            def __init__(self, given=None, token_file=None):
                self.email = "someone@example.com"

            def read(self):
                if Account.took:
                    return {"seven_day": (0.0, NOW + 7 * 24 * HOUR)}, None
                return {"seven_day": (1.0, NOW + 2 * 24 * HOUR)}, credit()

            def spend(self, chosen, key):
                Account.took = Account.lands
                raise limit_resets.Unavailable("network: timed out")

        with (
            patch.object(limit_resets, "Claude", Account),
            patch.object(limit_resets.time, "time", return_value=NOW),
        ):
            for lands, result in (
                (False, "error: network: timed out"),
                (True, "reset"),
            ):
                Account.took, Account.lands = False, lands
                report = limit_resets.sweep(sweep_arguments(now="claude"))
                self.assertEqual(
                    [account["cli"] for account in report["accounts"]], ["claude"]
                )
                self.assertEqual(report["accounts"][0]["result"], result)

    def test_a_plan_token_file_and_an_undated_sign_in_are_used(self):
        with tempfile.TemporaryDirectory() as folder:
            token = Path(folder, "work.token")
            token.write_text("plan-token\n")
            self.assertEqual(limit_resets.claude_token(None, str(token)), "plan-token")
            with self.assertRaises(limit_resets.Unavailable):
                limit_resets.claude_token(None, str(Path(folder, "missing.token")))
        undated = {"claudeAiOauth": {"accessToken": "kept", "expiresAt": 0}}
        with patch.dict(limit_resets.os.environ, {"CLAUDE_CONFIG_DIR": "/nonexistent"}):
            self.assertEqual(limit_resets.claude_token(undated), "kept")

    def test_the_sweep_reports_without_spending_unless_asked(self):
        class Account:
            cli = "claude"
            spent = []

            def __init__(self, given=None, token_file=None):
                self.email = "someone@example.com"

            def read(self):
                if Account.spent:  # the spent reset is gone and the window cleared
                    return {"seven_day": (0.0, NOW + 7 * 24 * HOUR)}, None
                return {"seven_day": (1.0, NOW + 2 * 24 * HOUR)}, credit()

            def spend(self, chosen, key):
                Account.spent.append(chosen["id"])
                return "reset"

        class Missing:
            cli = "codex"

            def __init__(self, home=None):
                raise limit_resets.Unavailable("no Codex sign-in")

        arguments = sweep_arguments()
        with (
            patch.object(limit_resets, "Claude", Account),
            patch.object(limit_resets, "Codex", Missing),
            patch.object(limit_resets.time, "time", return_value=NOW),
        ):
            report = limit_resets.sweep(arguments)
            self.assertEqual(Account.spent, [])
            claude, codex = report["accounts"]
            self.assertEqual(
                (claude["decision"], "result" in claude), ("restore", False)
            )
            self.assertEqual(codex["error"], "no Codex sign-in")
            arguments.apply = True
            report = limit_resets.sweep(arguments)
            spent = report["accounts"][0]
            self.assertEqual(
                (Account.spent, spent["result"], spent["confirmed"]),
                (["launch"], "reset", True),
            )
        json.dumps(report)


if __name__ == "__main__":
    unittest.main()
