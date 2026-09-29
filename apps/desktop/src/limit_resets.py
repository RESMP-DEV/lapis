"""Spends a saved Claude Code or Codex limit reset when it helps, on this machine.

    python3 limit_resets.py [--apply] [--now CLI] [--min-blocked-minutes 60]
        [--keep 0] [--salvage-hours 12] [--attempted KEY ...]

lapis runs this on each machine with Claude Code or Codex agents, locally or by
ssh with the script on stdin (`python3 - ARGS`). It reads the sign-in the CLI
itself keeps there, asks the account which saved resets it has, and prints one
JSON object. With --apply it follows OMP's rules for spending one:

- restore: a usage window is exhausted and would stay so for at least
  --min-blocked-minutes, and the account's selected reset clears every
  exhausted window (Claude's weekly session reset only a five-hour block),
  keeping --keep resets in reserve;
- salvage: a reset expires within --salvage-hours while the weekly window is at
  least a quarter used, so it is not lost unspent.

After spending, it reads the account again and reports whether the reset took
(`confirmed`), as modelctl's reset sequence does.

--now CLI spends that CLI's selected reset at once, as the claude.ai button
does. A key in --attempted (printed back as `attempt`) is not tried again.
Credentials come from the CLI's own files, or for Claude Code on the Mac from
its keychain item, which lapis reads and passes as one JSON line on stdin with
--claude-credentials-stdin. They are never printed.
"""

from __future__ import annotations

import argparse
import json
import os
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
MAX_REMAINING = {"five_hour": 6 * HOUR, "seven_day": 8 * 24 * HOUR}


class Unavailable(Exception):
    """The account could not be read or asked; nothing was spent."""


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
        return float(value)
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
    candidates = [given] if given else []
    try:
        with open(os.path.join(home, ".credentials.json")) as file:
            candidates.append(json.load(file))
    except (OSError, ValueError):
        pass
    for stored in candidates:
        oauth = stored.get("claudeAiOauth") if isinstance(stored, dict) else None
        if not isinstance(oauth, dict) or not oauth.get("accessToken"):
            continue
        if (oauth.get("expiresAt") or 0) / 1000 > time.time() + 60:
            return oauth["accessToken"]
    raise Unavailable(
        "no current Claude Code sign-in (start a Claude session to refresh it)"
    )


def claude_windows(usage):
    """{window: (used fraction, resets at)} for Claude's plan windows."""
    windows = {}
    for key in ("five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"):
        window = usage.get(key)
        if isinstance(window, dict) and isinstance(
            window.get("utilization"), (int, float)
        ):
            windows[key] = (
                window["utilization"] / 100.0,
                parse_time(window.get("resets_at")),
            )
    return windows


def claude_credit(usage, at_wall):
    """The reset the account would spend next, as {id, program, clears,
    expires, remaining, usable, requires_limit}, or None."""
    cedar = usage.get(CEDAR)
    if isinstance(cedar, dict) and cedar.get("eligible"):
        for grant in cedar.get("grants") or []:
            if grant.get("id") != cedar.get("next_grant_id"):
                continue
            cooldown = parse_time(cedar.get("cooldown_until"))
            return {
                "id": grant["id"],
                "program": CEDAR,
                "title": grant.get("label") or "Claude limit reset",
                "clears": [c for c in grant.get("clears") or [] if c in MAX_REMAINING],
                "expires": parse_time(grant.get("ends_at")),
                "remaining": int(grant.get("resets_left") or 0),
                "usable": bool(grant.get("usable_now"))
                and not grant.get("paused")
                and not (cooldown and cooldown > time.time()),
                "requires_limit": bool(grant.get("use_requires_limit", True)),
            }
    juniper = (at_wall or {}).get(JUNIPER)
    if (
        isinstance(juniper, dict)
        and juniper.get("eligible")
        and juniper.get("arm") == "reset"
    ):
        return {
            "id": JUNIPER,
            "program": JUNIPER,
            "title": "Claude session limit reset",
            "clears": ["five_hour"],
            "expires": parse_time(juniper.get("weekly_resets_at")),
            "remaining": 1 if juniper.get("available") else 0,
            "usable": bool(juniper.get("available")),
            "requires_limit": True,
        }
    return None


class Claude:
    cli = "claude"

    def __init__(self, given=None):
        self.token = claude_token(given)
        self.headers = dict(CLAUDE_HEADERS, authorization="Bearer " + self.token)
        status, profile = request(f"{CLAUDE_API}/api/oauth/profile", self.headers)
        if status != 200 or not isinstance(profile, dict):
            raise Unavailable(f"profile: HTTP {status}")
        account = profile.get("account") or {}
        organization = profile.get("organization") or {}
        self.email = account.get("email") or ""
        self.org = organization.get("uuid") or ""

    def read(self):
        status, usage = request(
            f"{CLAUDE_API}/api/oauth/usage?cedar_ember=1&skip_spend=1", self.headers
        )
        if status != 200 or not isinstance(usage, dict):
            raise Unavailable(f"usage: HTTP {status}")
        at_wall = None
        cedar = usage.get(CEDAR)
        if not (isinstance(cedar, dict) and cedar.get("next_grant_id")):
            status, at_wall = request(
                f"{CLAUDE_API}/api/oauth/usage?at_wall=1&skip_spend=1", self.headers
            )
            at_wall = at_wall if status == 200 and isinstance(at_wall, dict) else None
        return claude_windows(usage), claude_credit(usage, at_wall)

    def spend(self, credit):
        body = {"program": credit["program"]}
        if credit["program"] == CEDAR:
            body.update(grant_id=credit["id"], request_id=uuid.uuid4().hex)
        status, payload = request(
            f"{CLAUDE_API}/api/organizations/{self.org}/reset_rate_limits",
            self.headers,
            body,
            25,
        )
        result = payload.get("result") if isinstance(payload, dict) else None
        return result or f"http_{status}"


# Codex ---------------------------------------------------------------------


class Codex:
    cli = "codex"

    def __init__(self, given=None):
        home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
        try:
            with open(os.path.join(home, "auth.json")) as file:
                tokens = json.load(file).get("tokens") or {}
        except (OSError, ValueError, AttributeError):
            raise Unavailable("no Codex sign-in") from None
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

    def read(self):
        status, usage = request(f"{CODEX_API}/wham/usage", self.headers)
        if status != 200 or not isinstance(usage, dict):
            raise Unavailable(f"usage: HTTP {status}")
        self.email = usage.get("email") or ""
        windows = {}
        limits = usage.get("rate_limit") or {}
        for key in ("primary_window", "secondary_window"):
            window = limits.get(key)
            if not isinstance(window, dict) or window.get("used_percent") is None:
                continue
            seconds = window.get("limit_window_seconds") or 0
            name = "seven_day" if seconds >= 24 * HOUR else "five_hour"
            windows[name] = (
                window["used_percent"] / 100.0,
                parse_time(window.get("reset_at")),
            )
        status, listing = request(
            f"{CODEX_API}/wham/rate-limit-reset-credits", self.headers
        )
        credits = [
            c
            for c in (
                listing.get("credits")
                if status == 200 and isinstance(listing, dict)
                else []
            )
            or []
            if isinstance(c, dict) and (c.get("status") or "available") == "available"
        ]
        credits.sort(key=lambda c: parse_time(c.get("expires_at")) or float("inf"))
        if not credits:
            return windows, None
        soonest = credits[0]
        return windows, {
            "id": soonest["id"],
            "program": "codex",
            "title": soonest.get("title") or "Codex rate limit reset",
            # A saved reset clears the account's chat limits generally.
            "clears": list(MAX_REMAINING),
            "expires": parse_time(soonest.get("expires_at")),
            "remaining": len(credits),
            "usable": True,
            "requires_limit": False,
        }

    def spend(self, credit):
        body = {"credit_id": credit["id"], "redeem_request_id": str(uuid.uuid4())}
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
    blockers = [name for name in blockers if name in MAX_REMAINING] or blockers
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
    weekly = windows.get("seven_day")
    horizon = settings["salvage_hours"] * HOUR
    if horizon <= 0 or credit["expires"] is None or credit["expires"] - now > horizon:
        return None, "not-needed", None
    if credit["requires_limit"]:
        return None, "needs-limit", None
    if weekly is None or weekly[0] < SALVAGE_MIN_USED:
        return None, "window-mostly-free", None
    key = f"salvage|{identity}|{int(credit['expires'] // 60)}"
    return (
        (None, "already-attempted", key)
        if key in attempted
        else ("salvage", "expiring", key)
    )


def sweep(arguments):
    settings = {
        "min_blocked_minutes": arguments.min_blocked_minutes,
        "keep": arguments.keep,
        "salvage_hours": arguments.salvage_hours,
    }
    attempted = set(arguments.attempted or [])
    given = None
    if arguments.claude_credentials_stdin:
        try:
            given = json.loads(sys.stdin.readline())
        except ValueError:
            given = None
    accounts = []
    for kind in (Claude, Codex):
        report = {"cli": kind.cli}
        accounts.append(report)
        try:
            account = kind(given if kind is Claude else None)
            windows, credit = account.read()
        except Unavailable as error:
            report["error"] = str(error)
            continue
        now = time.time()
        report.update(
            email=account.email,
            windows={
                name: {"used": round(used, 3), "resets": at}
                for name, (used, at) in windows.items()
            },
            credit=None
            if credit is None
            else {
                k: credit[k]
                for k in ("title", "program", "remaining", "usable", "expires")
            },
        )
        action, reason, key = plan(
            kind.cli,
            account.email,
            windows,
            credit,
            now,
            settings,
            attempted,
            arguments.now == kind.cli,
        )
        report.update(decision=action or reason)
        if key:
            report["attempt"] = key
        if action and (arguments.apply or arguments.now == kind.cli):
            try:
                report["result"] = account.spend(credit)
            except Unavailable as error:
                report["result"] = f"error: {error}"
            if report["result"] == "reset":
                report["confirmed"] = confirmed(account, credit)
    return {"time": time.time(), "accounts": accounts}


def confirmed(account, spent):
    """Whether the account now reads as reset: the reset it spent is gone and
    no plan window is exhausted. An unreadable account is not confirmed."""
    try:
        windows, credit = account.read()
    except Unavailable:
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
    print(json.dumps(sweep(parser.parse_args(argv))))


if __name__ == "__main__":
    main()
