"""Spends a saved Claude Code or Codex limit reset when it helps, on this machine.

    python3 limit_resets.py --prepare [--now CLI] ...
    python3 limit_resets.py --now CLI --operation UUID --pending-credit ID ...
    python3 limit_resets.py --reconcile-only --operation UUID --pending-credit ID ...

lapis runs this for one actual plan and CLI on one machine, locally or by ssh
with the script on stdin (`python3 - ARGS`). `--prepare` reads that account and
follows OMP's spending rules, but never spends. The main machine persists the
chosen `credit_id`, then passes it with an `--operation` UUID for the consume.

- restore: a usage window is exhausted and would stay so for at least
  --min-blocked-minutes, and the account's selected reset clears every
  exhausted window (Claude's weekly session reset only a five-hour block),
  keeping --keep resets in reserve;
- salvage: a reset expires within --salvage-hours while the weekly window is at
  least a quarter used, so it is not lost unspent.

The consume re-reads only the persisted credit. `--reconcile-only` never
replays a consume: an available credit remains `outcome_unknown`; an absent,
consumed or expired credit is `settled` without claiming a confirmed reset.
After a successful consume, it reads the account again and reports whether the
reset took (`confirmed`).

--now CLI spends that CLI's already persisted reset at once, as claude.ai's
button does. A key in --attempted (printed back as `attempt`) is not tried
again.
Credentials come from the CLI's own files, or for Claude Code on the Mac from
its keychain item, which lapis reads and passes as one JSON line on stdin with
--claude-credentials-stdin. They are never printed.
"""

from __future__ import annotations

import argparse
import math
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import uuid

CLAUDE_API = "https://api.anthropic.com"
CODEX_API = "https://chatgpt.com/backend-api"
CLAUDE_HEADERS = {
    "anthropic-beta": "oauth-2025-04-20",
    "user-agent": "claude-cli/2.1.283 (external, cli)",
    "accept": "application/json",
    "content-type": "application/json",
}
CEDAR = "cedar_ember"  # a saved reset, as claude.ai's "Reset for free"
JUNIPER = "juniper_tide"  # the weekly session reset (Claude Code's /limit-reset)
SALVAGE_MIN_USED = 0.25
EXHAUSTED = 0.999
HOUR = 3600.0
MAX_REMAINING = {
    "five_hour": 6 * HOUR,
    "seven_day": 8 * 24 * HOUR,
    "seven_day_opus": 8 * 24 * HOUR,
    "seven_day_sonnet": 8 * 24 * HOUR,
}
PLAN_NAME = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class Unavailable(Exception):
    """An account/operation could not be observed; a consume may be uncertain."""


def request(url, headers, body=None, timeout=15):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        url, data=data, headers=headers, method="POST" if body else "GET"
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        try:
            payload = json.load(error)
        except ValueError:
            payload = None
        return error.code, payload
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise Unavailable(f"network: {error}") from None


def parse_time(value):
    """Seconds since the epoch from an ISO 8601 string or a number; None if absent."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return number(value)
    try:
        from datetime import datetime

        return datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


# Claude Code ---------------------------------------------------------------


def claude_token(given=None):
    """Claude Code's current access token: what lapis read from the Mac's
    keychain and passed on stdin, else Claude Code's credentials file."""
    home = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude")
    candidates = [given] if given is not None else []
    if given is None:
        try:
            with open(os.path.join(home, ".credentials.json")) as file:
                candidates.append(json.load(file))
        except (OSError, ValueError):
            pass
    for stored in candidates:
        oauth = stored.get("claudeAiOauth") if isinstance(stored, dict) else None
        if not isinstance(oauth, dict) or not oauth.get("accessToken"):
            continue
        expires_at = oauth.get("expiresAt")
        if (
            expires_at is None
            or expires_at == 0
            or (
                isinstance(expires_at, (int, float))
                and expires_at / 1000 > time.time() + 60
            )
        ):
            return oauth["accessToken"]
    raise Unavailable(
        "no current Claude Code sign-in (start a Claude session to refresh it)"
    )


def claude_plan_token(name: str) -> str:
    """A visiting Claude plan's own setup token, never its host's sign-in."""
    if (
        not isinstance(name, str)
        or not PLAN_NAME.fullmatch(name)
        or name in (".", "..")
    ):
        raise Unavailable("invalid Claude plan name")
    path = os.path.expanduser(f"~/.lapis/accounts/claude/{name}.token")
    try:
        with open(path, encoding="utf-8") as file:
            token = file.read(8192).strip()
    except OSError:
        raise Unavailable(
            f"Claude plan {name} has no usable token on this machine"
        ) from None
    if not token or "\n" in token or "\0" in token:
        raise Unavailable(f"Claude plan {name} has no usable token on this machine")
    return token


def number(value):
    """A finite number, or None; booleans are provider field errors."""
    if (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
    ):
        return float(value)
    return None


def claude_windows(usage):
    """{window: (used fraction, resets at)} for Claude's plan windows."""
    windows = {}
    for key in ("five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"):
        window = usage.get(key)
        used = number(window.get("utilization")) if isinstance(window, dict) else None
        if used is not None:
            windows[key] = (
                used / 100.0,
                parse_time(window.get("resets_at")),
            )
    return windows


def claude_credits(usage, at_wall):
    """Every provider credit with a usable identity, in provider order."""
    credits = []
    cedar = usage.get(CEDAR)
    if isinstance(cedar, dict) and cedar.get("eligible"):
        grants = cedar.get("grants")
        for grant in grants if isinstance(grants, list) else []:
            if not isinstance(grant, dict) or not isinstance(grant.get("id"), str):
                continue
            grant_id = grant["id"]
            if not grant_id:
                continue
            cooldown = parse_time(cedar.get("cooldown_until"))
            remaining = number(grant.get("resets_left"))
            credits.append(
                {
                    "id": grant_id,
                    "program": CEDAR,
                    "title": str(grant.get("label") or "Claude limit reset"),
                    "clears": [
                        name
                        for name in (
                            grant.get("clears")
                            if isinstance(grant.get("clears"), list)
                            else []
                        )
                        if isinstance(name, str) and name in MAX_REMAINING
                    ],
                    "expires": parse_time(grant.get("ends_at")),
                    "remaining": int(remaining or 0),
                    "usable": bool(grant.get("usable_now"))
                    and not grant.get("paused")
                    and not (cooldown and cooldown > time.time()),
                    "requires_limit": bool(grant.get("use_requires_limit", True)),
                    "available": grant_id == cedar.get("next_grant_id")
                    and bool(grant.get("usable_now"))
                    and not grant.get("paused"),
                }
            )
    juniper = (at_wall or {}).get(JUNIPER)
    if (
        isinstance(juniper, dict)
        and juniper.get("eligible")
        and juniper.get("arm") == "reset"
    ):
        credits.append(
            {
                "id": JUNIPER,
                "program": JUNIPER,
                "title": "Claude session limit reset",
                "clears": ["five_hour"],
                "expires": parse_time(juniper.get("weekly_resets_at")),
                "remaining": 1 if juniper.get("available") else 0,
                "usable": bool(juniper.get("available")),
                "requires_limit": True,
                "available": bool(juniper.get("available")),
            }
        )
    return credits


def claude_credit(usage, at_wall):
    """The reset the account would spend next, or None."""
    for candidate in claude_credits(usage, at_wall):
        if candidate["available"] and candidate["usable"]:
            return candidate
    return None


class Claude:
    cli = "claude"

    def __init__(self, given=None, plan="", plan_home="local", machine="local"):
        if plan:
            if plan_home == machine:
                self.token = claude_token(given if machine == "local" else None)
            else:
                self.token = claude_plan_token(plan)
        else:
            self.token = claude_token(given)
        self.headers = dict(CLAUDE_HEADERS, authorization="Bearer " + self.token)
        status, profile = request(f"{CLAUDE_API}/api/oauth/profile", self.headers)
        if status != 200 or not isinstance(profile, dict):
            raise Unavailable(f"profile: HTTP {status}")
        account = (
            profile.get("account") if isinstance(profile.get("account"), dict) else {}
        )
        organization = (
            profile.get("organization")
            if isinstance(profile.get("organization"), dict)
            else {}
        )
        self.email = str(account.get("email") or "")
        self.org = str(organization.get("uuid") or "")

    def read(self, credit_id=None):
        status, usage = request(
            f"{CLAUDE_API}/api/oauth/usage?cedar_ember=1&skip_spend=1", self.headers
        )
        if status != 200 or not isinstance(usage, dict):
            raise Unavailable(f"usage: HTTP {status}")
        status, at_wall = request(
            f"{CLAUDE_API}/api/oauth/usage?at_wall=1&skip_spend=1", self.headers
        )
        at_wall = at_wall if status == 200 and isinstance(at_wall, dict) else None
        credits = claude_credits(usage, at_wall)
        if credit_id is not None:
            cedar = usage.get(CEDAR)
            grants = cedar.get("grants") if isinstance(cedar, dict) else None
            malformed = (
                isinstance(cedar, dict)
                and cedar.get("eligible")
                and (
                    not isinstance(grants, list)
                    or any(
                        not isinstance(grant, dict)
                        or not isinstance(grant.get("id"), str)
                        or not grant.get("id")
                        for grant in grants
                    )
                )
            )
            if malformed:
                raise Unavailable("malformed Claude credit listing")
            exact = next((item for item in credits if item["id"] == credit_id), None)
            if exact is not None:
                exact["expired"] = (
                    exact["expires"] is not None and exact["expires"] <= time.time()
                )
            return claude_windows(usage), exact
        return claude_windows(usage), claude_credit(usage, at_wall)

    @staticmethod
    def consume_id(operation, credit):
        return uuid.uuid5(
            uuid.NAMESPACE_URL, f"lapis-limit-reset/{operation}/{credit['id']}"
        ).hex

    def spend(self, credit, operation=None):
        body = {"program": credit["program"]}
        if credit["program"] == CEDAR:
            body.update(
                grant_id=credit["id"],
                request_id=self.consume_id(operation or str(uuid.uuid4()), credit),
            )
        status, payload = request(
            f"{CLAUDE_API}/api/organizations/{self.org}/reset_rate_limits",
            self.headers,
            body,
            25,
        )
        result = payload.get("result") if isinstance(payload, dict) else None
        return result or f"http_{status}"


# Codex ---------------------------------------------------------------------


def codex_credits(listing):
    """Every credit with a usable identity; availability stays explicit."""
    raw = listing.get("credits") if isinstance(listing, dict) else None
    credits = []
    for item in raw if isinstance(raw, list) else []:
        if not isinstance(item, dict) or not isinstance(item.get("id"), str):
            continue
        credit_id = item["id"]
        if not credit_id:
            continue
        status = str(item.get("status") or "available")
        expires = parse_time(item.get("expires_at"))
        credits.append(
            {
                "id": credit_id,
                "program": "codex",
                "title": str(item.get("title") or "Codex rate limit reset"),
                # A saved reset clears the account's chat limits generally.
                "clears": list(MAX_REMAINING),
                "expires": expires,
                "remaining": 1 if status == "available" else 0,
                "usable": status == "available",
                "requires_limit": False,
                "available": status == "available",
                "status": status,
            }
        )
    credits.sort(
        key=lambda item: (not item["available"], item["expires"] or float("inf"))
    )
    return credits


class Codex:
    cli = "codex"

    def __init__(self, given=None, plan="", plan_home="local", machine="local"):
        del given
        if plan:
            if (
                not isinstance(plan, str)
                or not PLAN_NAME.fullmatch(plan)
                or plan in (".", "..")
            ):
                raise Unavailable("invalid Codex plan name")
            if plan_home != machine:
                home = os.path.expanduser(f"~/.lapis/accounts/codex/{plan}")
            else:
                home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
        else:
            home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
        try:
            with open(os.path.join(home, "auth.json")) as file:
                tokens = json.load(file).get("tokens") or {}
        except (OSError, ValueError, AttributeError):
            raise Unavailable(
                f"Codex plan {plan} has no usable login on this machine"
            ) from None
        if not tokens.get("access_token"):
            raise Unavailable("no Codex sign-in")
        self.headers = {
            "Authorization": "Bearer " + tokens["access_token"],
            "User-Agent": "codex_cli_rs",
            "Accept": "application/json",
            "Content-Type": "application/json",
        }
        self.account = tokens.get("account_id") or ""
        if self.account:
            self.headers["ChatGPT-Account-Id"] = self.account
        self.email = ""

    def read(self, credit_id=None):
        status, usage = request(f"{CODEX_API}/wham/usage", self.headers)
        if status != 200 or not isinstance(usage, dict):
            raise Unavailable(f"usage: HTTP {status}")
        self.email = usage.get("email") or ""
        windows = {}
        limits = usage.get("rate_limit") or {}
        for key in ("primary_window", "secondary_window"):
            window = limits.get(key)
            if not isinstance(window, dict) or not isinstance(
                window.get("used_percent"), (int, float)
            ):
                continue
            seconds = number(window.get("limit_window_seconds")) or 0
            name = "seven_day" if seconds >= 24 * HOUR else "five_hour"
            windows[name] = (
                number(window["used_percent"]) / 100.0,
                parse_time(window.get("reset_at")),
            )
        status, listing = request(
            f"{CODEX_API}/wham/rate-limit-reset-credits", self.headers
        )
        credits = codex_credits(listing)
        if credit_id is not None:
            raw = listing.get("credits") if isinstance(listing, dict) else None
            malformed = not isinstance(raw, list) or any(
                not isinstance(item, dict)
                or not isinstance(item.get("id"), str)
                or not item.get("id")
                for item in raw
            )
            if malformed:
                raise Unavailable("malformed Codex credit listing")
            exact = next((item for item in credits if item["id"] == credit_id), None)
            if exact is not None:
                exact["expired"] = (
                    exact["expires"] is not None and exact["expires"] <= time.time()
                )
            return windows, exact
        return windows, next((item for item in credits if item["available"]), None)

    @staticmethod
    def consume_id(operation, credit):
        return str(
            uuid.uuid5(
                uuid.NAMESPACE_URL, f"lapis-limit-reset/{operation}/{credit['id']}"
            )
        )

    def spend(self, credit, operation=None):
        body = {
            "credit_id": credit["id"],
            "redeem_request_id": self.consume_id(
                operation or str(uuid.uuid4()), credit
            ),
        }
        if self.account:
            body["account_id"] = self.account
        status, payload = request(
            f"{CODEX_API}/wham/rate-limit-reset-credits/consume", self.headers, body, 25
        )
        code = payload.get("code") if isinstance(payload, dict) else None
        return code or ("reset" if status == 200 else f"http_{status}")


# Policy ----------------------------------------------------------------------


def plan(cli, email, windows, credit, now, settings, attempted, force=False):
    """(action or None, reason, attempt key). Pure: the rules only."""
    if credit is None:
        return None, "no-credit", None
    if not credit["usable"] or credit["remaining"] < 1:
        return None, "credit-unusable", None
    if credit["expires"] is not None and credit["expires"] <= now:
        return None, "credit-expired", None
    identity = f"{cli}|{email}|{credit['id']}|{credit['remaining']}"
    if force:
        return "now", "asked", f"now|{identity}|{int(now // 60)}"
    blockers = sorted(name for name, (used, _) in windows.items() if used >= EXHAUSTED)
    if blockers:
        if any(name not in credit["clears"] for name in blockers):
            return None, "incomplete-coverage", None
        if credit["program"] == JUNIPER and blockers != ["five_hour"]:
            return None, "incomplete-coverage", None
        until = [windows[name][1] for name in blockers]
        if any(at is None for at in until):
            return None, "no-reset-time", None
        unblock = max(until)
        if any(
            windows[name][1] - now > MAX_REMAINING.get(name, 8 * 24 * HOUR)
            for name in blockers
        ):
            return None, "reset-implausible", None
        if unblock - now < settings["min_blocked_minutes"] * 60:
            return None, "reset-too-soon", None
        if credit["remaining"] - max(0, settings["keep"]) < 1:
            return None, "reserve", None
        key = f"block|{identity}|{','.join(blockers)}|{int(unblock // 60)}"
        return (
            (None, "already-attempted", key)
            if key in attempted
            else ("restore", "blocked", key)
        )
    weekly_values = [
        used for name, (used, _) in windows.items() if name.startswith("seven_day")
    ]
    weekly_used = max(weekly_values, default=0.0)
    horizon = settings["salvage_hours"] * HOUR
    if horizon <= 0 or credit["expires"] is None or credit["expires"] - now > horizon:
        return None, "not-needed", None
    if credit["requires_limit"]:
        return None, "needs-limit", None
    if weekly_used < SALVAGE_MIN_USED:
        return None, "window-mostly-free", None
    key = f"salvage|{identity}|{int(credit['expires'] // 60)}"
    return (
        (None, "already-attempted", key)
        if key in attempted
        else ("salvage", "expiring", key)
    )


def public_credit(value):
    """A complete JSON credit; never a token or account header."""
    return None if value is None else dict(value)


def settle(account, exact, operation, reason):
    return {
        "cli": account.cli,
        "operation": operation,
        "credit_id": exact["id"],
        "decision": "settled",
        "result": "settled",
        "confirmed": False,
        "reason": reason,
        "credit": public_credit(exact),
    }


def outcome_unknown(account, exact, operation, reason, error=""):
    report = {
        "cli": account.cli,
        "operation": operation,
        "credit_id": exact["id"],
        "decision": "outcome_unknown",
        "result": "outcome_unknown",
        "confirmed": False,
        "reason": reason,
        "credit": public_credit(exact),
    }
    if error:
        report["error"] = error
    return report


def refusal(cli, message):
    return {
        "cli": cli,
        "decision": "refused",
        "result": "refused",
        "error": message,
    }


def consume_answer(account, answer):
    """Classify a conservative provider answer; unknown never clears a retry."""
    if answer == "reset":
        return "reset"
    if answer.startswith("http_"):
        code = number(answer.removeprefix("http_"))
        if code is not None and 400 <= code < 500 and code not in (408, 429):
            return "refused"
    return "outcome_unknown"


class IdentityMismatch(RuntimeError):
    """Credentials changed between preparation and the attempted operation."""


def read_account(account, credit_id=None, expected_email=""):
    result = account.read() if credit_id is None else account.read(credit_id)
    if expected_email and str(account.email).lower() != expected_email:
        raise IdentityMismatch("selected account identity changed")
    return result


def sweep(arguments):
    settings = {
        "min_blocked_minutes": arguments.min_blocked_minutes,
        "keep": arguments.keep,
        "salvage_hours": arguments.salvage_hours,
    }
    attempted = set(arguments.attempted or [])
    machine = getattr(arguments, "machine", "local")

    # An explicitly supplied target flag selects only that CLI, even when its
    # value is empty (a machine's own sign-in). Without either flag, retain the
    # helper's original standalone read-and-report behavior for both.
    targets = []
    for kind, plan_argument, home_argument in (
        (Claude, "claude_plan", "claude_plan_home"),
        (Codex, "codex_plan", "codex_plan_home"),
    ):
        if getattr(arguments, plan_argument, None) is not None:
            targets.append(
                (
                    kind,
                    getattr(arguments, plan_argument) or "",
                    getattr(arguments, home_argument, "local"),
                )
            )
    if not targets:
        targets = [(Claude, "", "local"), (Codex, "", "local")]
    if arguments.now:
        targets = [target for target in targets if target[0].cli == arguments.now]

    operation = getattr(arguments, "operation", "") or ""
    pending_credit = getattr(arguments, "pending_credit", "") or ""
    expected_email = (getattr(arguments, "expected_email", "") or "").strip().lower()
    prepare = bool(getattr(arguments, "prepare", False))
    reconcile = bool(getattr(arguments, "reconcile_only", False))
    wants_spend = prepare is False and (arguments.apply or arguments.now is not None)
    if (reconcile or wants_spend) and len(targets) != 1:
        return {
            "time": time.time(),
            "phase": "refused",
            "accounts": [
                refusal(
                    kind.cli, "select exactly one CLI before consuming or reconciling"
                )
                for kind, _, _ in targets
            ],
        }
    try:
        uuid.UUID(operation)
        operation_valid = bool(operation)
    except ValueError:
        operation_valid = False
    credit_valid = (
        isinstance(pending_credit, str)
        and bool(pending_credit)
        and len(pending_credit) <= 256
        and not any(character.isspace() for character in pending_credit)
    )
    if (reconcile or wants_spend) and (not operation_valid or not credit_valid):
        message = (
            "spending requires both --operation and --pending-credit; "
            "run --prepare first"
        )
        return {
            "time": time.time(),
            "phase": "refused",
            "accounts": [refusal(kind.cli, message) for kind, _, _ in targets],
        }

    given = None
    if arguments.claude_credentials_stdin:
        try:
            given = json.loads(sys.stdin.readline())
        except ValueError:
            given = None

    accounts = []
    for kind, selected_plan, plan_home in targets:
        report = {"cli": kind.cli, "plan": selected_plan, "machine": machine}
        accounts.append(report)
        try:
            if kind is Claude:
                account = (
                    kind(given)
                    if not selected_plan
                    else kind(given, selected_plan, plan_home, machine)
                )
            else:
                account = (
                    kind()
                    if not selected_plan
                    else kind(None, selected_plan, plan_home, machine)
                )

            if not prepare and not reconcile and not wants_spend:
                windows, credit = read_account(account, expected_email=expected_email)
                report.update(
                    email=account.email,
                    windows={
                        name: {"used": round(used, 3), "resets": at}
                        for name, (used, at) in windows.items()
                    },
                    credit=public_credit(credit),
                )
                action, reason, key = plan(
                    kind.cli,
                    account.email,
                    windows,
                    credit,
                    time.time(),
                    settings,
                    attempted,
                    arguments.now == kind.cli,
                )
                report.update(decision=action or reason)
                if key:
                    report["attempt"] = key
                continue

            if reconcile:
                try:
                    windows, exact = read_account(
                        account, pending_credit, expected_email
                    )
                except Unavailable as error:
                    report.update(
                        outcome_unknown(
                            account,
                            {"id": pending_credit},
                            operation,
                            "credit listing was unreadable or malformed",
                            str(error),
                        )
                    )
                    continue
                report.update(
                    email=account.email,
                    windows={
                        name: {"used": round(used, 3), "resets": at}
                        for name, (used, at) in windows.items()
                    },
                )
                if exact is None:
                    report.update(
                        settle(
                            account,
                            {"id": pending_credit},
                            operation,
                            "credit is absent",
                        )
                    )
                elif exact.get("status") == "consumed":
                    report.update(
                        settle(
                            account,
                            exact,
                            operation,
                            "provider marked the credit consumed",
                        )
                    )
                elif exact.get("expired", False):
                    report.update(settle(account, exact, operation, "credit expired"))
                else:
                    report.update(
                        outcome_unknown(
                            account,
                            exact,
                            operation,
                            "credit is still available; do not replay the consume",
                        )
                    )
                continue

            if prepare:
                windows, credit = read_account(account, expected_email=expected_email)
                report.update(
                    email=account.email,
                    windows={
                        name: {"used": round(used, 3), "resets": at}
                        for name, (used, at) in windows.items()
                    },
                    credit=public_credit(credit),
                )
                action, reason, key = plan(
                    kind.cli,
                    account.email,
                    windows,
                    credit,
                    time.time(),
                    settings,
                    attempted,
                    arguments.now == kind.cli,
                )
                report["decision"] = "prepared" if action else reason
                if action:
                    report["action"] = action
                    report["attempt"] = key
                    report["credit_id"] = credit["id"]
                elif key:
                    report["attempt"] = key
                continue

            # Consume is intentionally policy-blind once a credit was selected
            # and persisted: reconciliation comes first, and only that exact
            # credit may be used.
            try:
                windows, exact = read_account(account, pending_credit, expected_email)
            except Unavailable as error:
                report.update(
                    outcome_unknown(
                        account,
                        {"id": pending_credit},
                        operation,
                        "credit listing was unreadable or malformed",
                        str(error),
                    )
                )
                continue
            report.update(
                email=account.email,
                windows={
                    name: {"used": round(used, 3), "resets": at}
                    for name, (used, at) in windows.items()
                },
            )
            if exact is None:
                report.update(
                    settle(
                        account, {"id": pending_credit}, operation, "credit is absent"
                    )
                )
                continue
            if exact.get("status") == "consumed":
                report.update(
                    settle(
                        account, exact, operation, "provider marked the credit consumed"
                    )
                )
                continue
            if exact.get("expired", False):
                report.update(settle(account, exact, operation, "credit expired"))
                continue
            if not exact.get("available") or not exact.get("usable"):
                report.update(
                    outcome_unknown(
                        account,
                        exact,
                        operation,
                        "exact credit is not currently consumable",
                    )
                )
                continue
            if not (arguments.apply or arguments.now == kind.cli):
                report.update(refusal(kind.cli, "spending requires --apply or --now"))
                continue

            report["attempt"] = f"consume|{operation}|{exact['id']}"
            report["credit_id"] = exact["id"]
            report["operation"] = operation
            report["credit"] = public_credit(exact)
            try:
                answer = account.spend(exact, operation)
            except Unavailable as error:
                report.update(
                    outcome_unknown(
                        account,
                        exact,
                        operation,
                        "consume outcome was not observed",
                        str(error),
                    )
                )
                continue
            except Exception as error:
                report.update(
                    outcome_unknown(
                        account,
                        exact,
                        operation,
                        "consume outcome was not observed",
                        f"unusable provider answer: {error}",
                    )
                )
                continue
            result = consume_answer(account, answer)
            report["answer"] = answer
            report["result"] = result
            report["decision"] = result
            if result == "reset":
                report["confirmed"] = confirmed(account, exact)
            else:
                report["confirmed"] = False
        except IdentityMismatch as error:
            report.update(refusal(kind.cli, str(error)))
        except Unavailable as error:
            report["error"] = str(error)
        except Exception as error:
            # One malformed account report cannot suppress another target.
            report["error"] = f"unusable account: {error}"

    phase = "prepared" if prepare else ("reconciled" if reconcile else "final")
    return {"time": time.time(), "phase": phase, "accounts": accounts}


def confirmed(account, spent):
    """Whether the account now reads as reset: the reset it spent is gone and
    no plan window is exhausted. An unreadable account is not confirmed."""
    try:
        windows, credit = account.read()
    except Unavailable:
        return False
    except Exception:
        return False
    same = (
        credit is not None
        and credit["id"] == spent["id"]
        and credit["remaining"] >= spent["remaining"]
    )
    return not same and all(used < EXHAUSTED for used, _ in windows.values())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--apply", action="store_true", help="spend a reset when the rules say so"
    )
    parser.add_argument(
        "--now", choices=["claude", "codex"], help="spend this CLI's reset at once"
    )
    parser.add_argument("--min-blocked-minutes", type=float, default=60)
    parser.add_argument("--keep", type=int, default=0)
    parser.add_argument("--salvage-hours", type=float, default=12)
    parser.add_argument("--attempted", nargs="*", default=[])
    parser.add_argument(
        "--claude-credentials-stdin",
        action="store_true",
        help="read Claude Code's stored credentials as one JSON line on stdin",
    )
    parser.add_argument(
        "--prepare",
        action="store_true",
        help="choose one credit and print it without spending",
    )
    parser.add_argument(
        "--reconcile-only",
        action="store_true",
        help="read the persisted credit without consuming it again",
    )
    parser.add_argument("--claude-plan", help="use this configured Claude plan")
    parser.add_argument("--claude-plan-home", default="local")
    parser.add_argument("--codex-plan", help="use this configured Codex plan")
    parser.add_argument("--codex-plan-home", default="local")
    parser.add_argument("--machine", default="local")
    parser.add_argument("--operation", help="stable identity for this consume attempt")
    parser.add_argument("--pending-credit", help="credit tied to --operation")
    parser.add_argument(
        "--expected-email", help="verified account identity from preparation"
    )
    print(json.dumps(sweep(parser.parse_args(argv))))


if __name__ == "__main__":
    main()
