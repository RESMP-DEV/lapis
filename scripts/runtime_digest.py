"""What lapis did for the person in the last day, from its own runtime logs.

A short, read-only digest of use rather than of code: whether guesses worked
and were taken (by arm, and held out), Ultra Tab answers against what composing
cost, how Tab moved, ping decisions, limit-reset outcomes and log warnings. It
reads ~/.lapis/runtime only and prints counts, never prompt or transcript text.

    python3 scripts/runtime_digest.py            # the last 24 hours
    python3 scripts/runtime_digest.py --hours 168 --json
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import Counter
from datetime import datetime, timedelta, timezone

LOG_TIME = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})")


def when(record):
    """A record's time, from whichever field its log uses."""
    for key in ("t", "time", "wall", "at"):
        value = record.get(key)
        if isinstance(value, str) and len(value) >= 19:
            try:
                return datetime.fromisoformat(value.replace("Z", "+00:00"))
            except ValueError:
                continue
    return None


def records(runtime, name, since):
    """Records of a JSON-lines log and its rotated files since `since`."""
    base = os.path.join(runtime, name)
    paths = sorted(glob.glob(glob.escape(base) + ".2*")) + [base + ".1", base]
    for path in paths:
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as lines:
            for line in lines:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                at = when(record) if isinstance(record, dict) else None
                if at is not None and at.tzinfo is None:
                    at = at.astimezone()
                if at is not None and at >= since:
                    yield record


def rate(part, whole):
    return round(part / whole, 3) if whole else None


def offer_key(event):
    """A usable offer identity, or none for malformed log data."""
    value = event.get("offer")
    return value if isinstance(value, str) and value else None


def guesses(runtime, since):
    events = list(records(runtime, "next_prompt.jsonl", since))
    kinds = Counter(e.get("event") for e in events)
    failed = [
        e for e in events if e.get("event") == "failed" and e.get("stage") == "predict"
    ]
    predicted = {
        key: e
        for e in events
        if e.get("event") == "predicted" and (key := offer_key(e)) is not None
    }
    seen = {
        key
        for e in events
        if e.get("event") in ("seen", "used") and (key := offer_key(e)) is not None
    }
    used = {
        key
        for e in events
        if e.get("event") == "used" and (key := offer_key(e)) is not None
    }
    arms = {}
    for key, offer in predicted.items():
        arm = arms.setdefault(offer.get("arm") or "control", Counter())
        arm["predicted"] += 1
        arm["seen"] += key in seen
        arm["used"] += key in used
    outcomes = [e for e in events if e.get("event") == "outcome"]
    hidden = [o for o in outcomes if o.get("hidden")]
    streak = 0
    for event in reversed(events):
        if event.get("event") == "predicted":
            break
        if event.get("event") == "failed" and event.get("stage") == "predict":
            streak += 1
    attempts = len(predicted) + len(failed)
    return {
        "attempts": attempts,
        "predicted": len(predicted),
        "success": rate(len(predicted), attempts),
        "failed_by_kind": dict(
            Counter(e.get("kind") or e.get("error") for e in failed)
        ),
        "failing_now": streak,
        "skipped": kinds.get("skipped", 0),
        "by_arm": {
            arm: {**counts, "acceptance": rate(counts["used"], counts["seen"])}
            for arm, counts in sorted(arms.items())
        },
        "held_out": {
            "settled": len(hidden),
            "exact": rate(
                sum(o.get("result") == "as_offered" for o in hidden), len(hidden)
            ),
        },
    }


def ultratab(runtime, since):
    composed = Counter(
        e.get("event") for e in records(runtime, "ultratab_compose.jsonl", since)
    )
    answers = Counter(
        e.get("how") for e in records(runtime, "ultratab_answers.jsonl", since)
    )
    return {"composed": dict(composed), "answers": dict(answers)}


def tab(runtime, since):
    events = Counter(e.get("event") for e in records(runtime, "tab_away.jsonl", since))
    focus = Counter()
    for e in records(runtime, "interaction.jsonl", since):
        if e.get("kind") == "agent-focus":
            focus[e.get("via") or "other"] += 1
    return {
        "tab_moves": events.get("tab", 0),
        "choices": events.get("choice", 0),
        "agent_focus_by": dict(focus.most_common(6)),
    }


def pings(runtime, since):
    decisions = Counter()
    for e in records(runtime, "attention.jsonl", since):
        # {"kind", "decision"} records, or older ones with "chime" and
        # "notification" fields.
        for value in (e.get("decision"), e.get("chime"), e.get("notification")):
            if isinstance(value, str):
                judged = re.search(r"judged \w+", value)
                decisions[judged.group(0) if judged else value.split(":")[0]] += 1
    return dict(decisions.most_common(6))


def window_log(runtime, since):
    """Limit-reset outcomes and the most frequent warnings in lapis.log."""
    resets, warnings = Counter(), Counter()
    path = os.path.join(runtime, "lapis.log")
    if not os.path.isfile(path):
        return {}
    stamp = since.astimezone().strftime("%Y-%m-%dT%H:%M:%S")
    with open(path, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            found = LOG_TIME.match(line)
            if not found or found.group(1) < stamp:
                continue
            if "Limit reset" in line:
                reason = re.search(
                    r"plan identity could not be verified|reset helper failed|"
                    r"no eligible reset|skipping automatic reset|spent|restored",
                    line,
                )
                resets[reason.group(0) if reason else "other"] += 1
            elif " warning: " in line:
                text = line.split(" warning: ", 1)[1]
                warnings[re.sub(r"\d+", "N", text)[:70].strip()] += 1
    return {"limit_resets": dict(resets), "warnings": dict(warnings.most_common(3))}


def digest(runtime, hours):
    since = datetime.now(timezone.utc) - timedelta(hours=hours)
    return {
        "hours": hours,
        "guesses": guesses(runtime, since),
        "ultratab": ultratab(runtime, since),
        "tab": tab(runtime, since),
        "pings": pings(runtime, since),
        "window": window_log(runtime, since),
    }


def lines(report):
    g, u, t, w = report["guesses"], report["ultratab"], report["tab"], report["window"]
    out = [f"lapis, last {report['hours']} h"]
    out.append(
        f"guesses: {g['predicted']} of {g['attempts']} attempts worked"
        + (f" ({g['success']:.0%})" if g["success"] is not None else "")
        + (
            f"; FAILING NOW, {g['failing_now']} in a row"
            if g["failing_now"] >= 3
            else ""
        )
    )
    if g["failed_by_kind"]:
        out.append(
            "  failed: " + ", ".join(f"{k} {v}" for k, v in g["failed_by_kind"].items())
        )
    for arm, counts in g["by_arm"].items():
        taken = counts["acceptance"]
        out.append(
            f"  {arm}: {counts['predicted']} made, {counts['seen']} seen, {counts['used']} taken"
            + (f" ({taken:.0%})" if taken is not None else "")
        )
    if g["held_out"]["settled"]:
        out.append(
            f"  held out: {g['held_out']['settled']} settled, exact {g['held_out']['exact']:.0%}"
        )
    composed = u["composed"].get("composed", 0) + u["composed"].get("fallback", 0)
    answered = sum(u["answers"].values())
    out.append(f"ultra tab: {answered} answers, {composed} cards composed")
    out.append(
        f"tab: {t['tab_moves']} Tab moves, {t['choices']} choices; focus by "
        + ", ".join(f"{k} {v}" for k, v in t["agent_focus_by"].items())
    )
    if report["pings"]:
        out.append(
            "pings: " + ", ".join(f"{k} {v}" for k, v in report["pings"].items())
        )
    if w.get("limit_resets"):
        out.append(
            "limit resets: "
            + ", ".join(f"{k} {v}" for k, v in w["limit_resets"].items())
        )
    for text, count in w.get("warnings", {}).items():
        out.append(f"warning x{count}: {text}")
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hours", type=float, default=24)
    parser.add_argument(
        "--runtime", default=os.path.join(os.path.expanduser("~/.lapis"), "runtime")
    )
    parser.add_argument("--json", action="store_true")
    arguments = parser.parse_args(argv)
    report = digest(arguments.runtime, arguments.hours)
    if arguments.json:
        json.dump(report, sys.stdout, indent=2)
        print()
    else:
        print("\n".join(lines(report)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
