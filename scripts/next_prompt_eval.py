#!/usr/bin/env python3
"""How well lapis predicts the next prompt, from transcripts and from its log.

`replay` samples prompts the person typed to Claude Code and Codex (on this Mac,
and on any `--machine` over ssh), has the model predict each from only the
conversation before it, and has a judge compare the guesses with what was
actually typed. `log` does the same for lapis's own predictions
(~/.lapis/runtime/next_prompt.jsonl): what was offered, used or dismissed, and how the
offers compare with what came next. Both use the helper lapis runs,
apps/desktop/src/next_prompt.py, so they measure what lapis does.

Reports go to build/reports/next-prompt/ as numbers only; `--keep-text` adds the
prompts, which never belong in the repository.
"""

import argparse
import json
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "apps" / "desktop" / "src" / "next_prompt.py"
sys.path.insert(0, str(HELPER.parent))
import next_prompt  # noqa: E402

JUDGE = """You grade next-message predictions for a person supervising coding agents.
For each item you get the end of the agent's latest reply, the message the person
actually sent, and up to three predicted messages. Grade each prediction:
2 = they could have sent it instead, with the same effect on the agent (wording
    and minor details may differ);
1 = right intent or action, but key specifics are missing or different, so they
    would have to edit it;
0 = a different intent.
Also label the actual message with one category: approve (go/yes/continue/send),
status (asks how it is going or to check), ship (install, merge, release, send a
drafted thing), fix (reports a bug or asks for a fix), new (new task or
feature), question (asks why/how/explain), correct (pushback or redirecting the
agent), other. Return only JSON:
{"grades": [{"id": 0, "scores": [2, 0, 1], "category": "approve"}]}"""
THRESHOLDS = (0.0, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9)


def on_machine(machine, arguments):
    """The helper's JSON answer, run here or on `machine` with itself on stdin."""
    try:
        if not machine:
            command = [sys.executable, str(HELPER), *arguments]
            run = subprocess.run(command, capture_output=True, text=True, timeout=900)
        else:
            remote = "python3 - " + " ".join(
                "'" + a.replace("'", "'\\''") + "'" for a in arguments
            )
            command = [
                "ssh",
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=10",
                "-T",
                "--",
            ]
            run = subprocess.run(
                command + [machine, remote],
                input=HELPER.read_text(),
                capture_output=True,
                text=True,
                timeout=900,
            )
    except subprocess.TimeoutExpired:
        return {"error": "timed out"}
    except OSError:
        return {"error": "helper unavailable"}
    if run.returncode != 0:
        return {"error": "helper failed"}
    try:
        return json.loads(run.stdout)
    except ValueError:
        return {"error": (run.stderr or "no answer").strip()[-300:]}


def judge(items, model):
    """Grades for [{id, last, actual, candidates}], eight to a call."""

    def batch(chunk):
        blocks = [
            "## Item {}\nAgent's latest reply (end):\n{}\n\nActual message:\n{}\n\n"
            "Predictions:\n{}".format(
                item["id"],
                item["last"][-1500:],
                item["actual"][:2000],
                "\n".join(
                    "{}. {}".format(k + 1, c["text"][:1500])
                    for k, c in enumerate(item["candidates"])
                ),
            )
            for item in chunk
        ]
        try:
            text, _ = next_prompt.ask(JUDGE, "\n\n".join(blocks), model, timeout=600)
            grades = json.loads(text[text.index("{") : text.rindex("}") + 1])["grades"]
            if not isinstance(grades, list):
                raise ValueError("invalid judge grades")
            expected = {item["id"]: len(item["candidates"]) for item in chunk}
            valid = []
            for grade in grades:
                if not isinstance(grade, dict):
                    continue
                identifier = grade.get("id")
                scores = grade.get("scores")
                if (
                    type(identifier) is int
                    and identifier in expected
                    and isinstance(scores, list)
                    and len(scores) == expected[identifier]
                    and all(
                        type(score) is int and score in (0, 1, 2) for score in scores
                    )
                ):
                    if grade.get("category") not in next_prompt.CATEGORIES:
                        grade["category"] = "other"
                    valid.append(grade)
            return valid
        except (
            ValueError,
            KeyError,
            RuntimeError,
            OSError,
            subprocess.TimeoutExpired,
        ) as error:
            print("judge batch failed: {}".format(error), file=sys.stderr)
            return []

    chunks = [items[i : i + 8] for i in range(0, len(items), 8)]
    with ThreadPoolExecutor(4) as pool:
        return {g["id"]: g for chunk in pool.map(batch, chunks) for g in chunk}


def summarize(rows):
    """rows: {words, category, p (top guess), scores} -> the report's numbers."""
    n = len(rows)
    if not n:
        return {"count": 0}

    def rate(part, test):
        return round(sum(1 for r in part if test(r)) / len(part), 3) if part else None

    report = {
        "count": n,
        "top1_sendable": rate(rows, lambda r: r["scores"][:1] == [2]),
        "top1_intent": rate(rows, lambda r: (r["scores"] or [0])[0] >= 1),
        "top3_sendable": rate(rows, lambda r: 2 in r["scores"]),
        "top3_intent": rate(rows, lambda r: max(r["scores"] or [0]) >= 1),
        "by_length": {},
        "by_category": {},
        "by_confidence": [],
    }
    for label, test in (
        ("up to 4 words", lambda w: w <= 4),
        ("longer", lambda w: w > 4),
    ):
        part = [r for r in rows if test(r["words"])]
        report["by_length"][label] = {
            "count": len(part),
            "top3_sendable": rate(part, lambda r: 2 in r["scores"]),
        }
    groups = defaultdict(list)
    for r in rows:
        groups[r["category"]].append(r)
    for category, part in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        report["by_category"][category] = {
            "count": len(part),
            "top3_sendable": rate(part, lambda r: 2 in r["scores"]),
            "top3_intent": rate(part, lambda r: max(r["scores"] or [0]) >= 1),
        }
    # Offered only at or above a threshold: how often, and how often right.
    for threshold in THRESHOLDS:
        shown = [r for r in rows if r["p"] >= threshold]
        report["by_confidence"].append(
            {
                "min_confidence": threshold,
                "shown": round(len(shown) / n, 3),
                "sendable_when_shown": rate(shown, lambda r: r["scores"][:1] == [2]),
                "intent_when_shown": rate(
                    shown, lambda r: (r["scores"] or [0])[0] >= 1
                ),
            }
        )
    return report


def grading_coverage(requested: int, graded: int) -> dict[str, object]:
    """Judge availability is coverage, not a prediction error or an accuracy score."""
    return {
        "requested": requested,
        "graded": graded,
        "coverage": round(graded / requested, 3) if requested else None,
        "metrics_basis": "successfully graded predictions",
    }


def replay(arguments):
    items = []
    for machine in [""] + (arguments.machine or []):
        sample = on_machine(
            machine,
            [
                "sample",
                "--count",
                str(arguments.count),
                "--seed",
                str(arguments.seed),
                *(["--since", arguments.since] if arguments.since else []),
                *(["--exclude", *arguments.exclude] if arguments.exclude else []),
            ],
        )
        if "error" in sample:
            print(
                "{}: {}".format(machine or "this Mac", sample["error"]), file=sys.stderr
            )
            continue
        for item in sample["items"]:
            item["machine"] = machine
            items.append(item)
    for number, item in enumerate(items):
        item["id"] = number

    def guess(item):
        when = item["time"]
        try:
            when = datetime.fromisoformat(when.replace("Z", "+00:00")).astimezone()
            when = when.strftime("%a %-I:%M %p")
        except ValueError:
            pass
        bundle = {
            "agent": {"cli": item["cli"], "machine": item["machine"]},
            "context": {"turns": item["turns"]},
            "time": when,
        }
        return next_prompt.predict(bundle, arguments.model, arguments.effort)

    with ThreadPoolExecutor(arguments.parallel) as pool:
        guesses = list(pool.map(guess, items))
    graded = []
    for item, answer in zip(items, guesses):
        if answer.get("candidates"):
            agent_turns = [t for t in item["turns"] if t["role"] == "agent"]
            graded.append(
                {
                    "id": item["id"],
                    "last": agent_turns[-1]["text"] if agent_turns else "",
                    "actual": item["actual"],
                    "candidates": answer["candidates"],
                    "ms": answer.get("ms", 0),
                }
            )
    grades = judge(graded, arguments.judge_model)
    rows = []
    for g in graded:
        if g["id"] not in grades:
            continue
        grade = grades[g["id"]]
        row = {
            "words": len(g["actual"].split()),
            "category": grade.get("category", "other"),
            "p": g["candidates"][0]["p"],
            "scores": grade.get("scores", [])[: len(g["candidates"])],
            "ms": g["ms"],
        }
        if arguments.keep_text:
            row.update(actual=g["actual"], candidates=g["candidates"])
        rows.append(row)
    report = summarize(rows)
    report["sampled"] = len(items)
    report["predicted"] = len(graded)
    report["prediction_failures"] = len(items) - len(graded)
    report["judge_missing_grades"] = sum(g["id"] not in grades for g in graded)
    report["grading"] = grading_coverage(len(graded), len(rows))
    report["model"] = arguments.model
    if graded:
        report["median_ms"] = sorted(g["ms"] for g in graded)[len(graded) // 2]
    return report, rows


def acceptance(events):
    """Offers by id from lapis's log, and the rates that matter: of the offers
    seen on screen, how many were used (sent with Tab, or typed with
    Option-Tab), how many after typing first, and how fast."""
    offers = {}
    for e in events:
        key = e.get("offer")
        if not key:
            continue
        offer = offers.setdefault(key, {"seen": False, "used": None, "withdrawn": None})
        kind = e.get("event")
        if kind == "predicted":
            candidates = e.get("candidates") or [{}]
            offer.update(
                shown=bool(e.get("shown")),
                p=float(candidates[0].get("p", 0) or 0),
                category=e.get("category") or "other",
                machine=e.get("machine", ""),
                cli=e.get("cli", "claude"),
                conversation=e.get("conversation", ""),
                turn=e.get("turn", 0),
                candidates=candidates,
            )
        elif kind == "seen":
            offer["seen"] = True
        elif kind == "used":
            offer["seen"] = True
            offer["used"] = e
        elif kind == "withdrawn":
            offer["withdrawn"] = e.get("reason")
    failures = defaultdict(int)
    for e in events:
        if e.get("event") == "failed":
            known = {
                "no transcript",
                "no Claude Code CLI on this Mac",
                "invalid conversation id",
                "timeout",
                "output too large",
                "invalid helper JSON",
                "helper unavailable",
                "helper failed",
            }
            raw = e.get("error")
            reason = raw if isinstance(raw, str) and raw in known else "helper failed"
            raw_stage = e.get("stage")
            stage = (
                raw_stage
                if isinstance(raw_stage, str) and raw_stage in {"context", "predict"}
                else "unknown"
            )
            failures["{}: {}".format(stage, reason)] += 1
    shown = [o for o in offers.values() if o.get("shown")]
    seen = [o for o in shown if o["seen"]]
    used = [o for o in seen if o["used"]]

    def rate(part, whole):
        return round(len(part) / len(whole), 3) if whole else None

    waits = sorted(
        o["used"].get("ms_after_seen", -1)
        for o in used
        if o["used"].get("ms_after_seen", -1) >= 0
    )
    report = {
        "predicted": sum(1 for o in offers.values() if "shown" in o),
        "offered": len(shown),
        "seen": len(seen),
        "used": len(used),
        "acceptance": rate(used, seen),
        "sent_with_tab": sum(1 for o in used if o["used"].get("sent")),
        "typed_first_then_used": sum(
            1 for o in used if o["used"].get("typed_first", 0) > 0
        ),
        "seen_not_used": sum(1 for o in seen if not o["used"]),
        "offered_never_seen": sum(1 for o in shown if not o["seen"]),
        "median_ms_to_use": waits[len(waits) // 2] if waits else None,
        "attempts": sum(
            1 for e in events if e.get("event") in ("predicted", "failed", "skipped")
        ),
        "legacy_records": sum(
            1
            for e in events
            if not e.get("offer") and e.get("event") in ("used", "dismissed")
        ),
        "failed": dict(sorted(failures.items(), key=lambda kv: -kv[1])),
        "skipped": sum(1 for e in events if e.get("event") == "skipped"),
        "by_category": {},
        "by_confidence": [],
    }
    groups = defaultdict(list)
    for o in seen:
        groups[o["category"]].append(o)
    for category, part in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        report["by_category"][category] = {
            "seen": len(part),
            "acceptance": rate([o for o in part if o["used"]], part),
        }
    for low in THRESHOLDS:
        part = [o for o in seen if o["p"] >= low]
        report["by_confidence"].append(
            {
                "min_confidence": low,
                "seen": len(part),
                "acceptance": rate([o for o in part if o["used"]], part),
            }
        )
    return offers, report


def log(arguments):
    events = []
    path = Path(arguments.log).expanduser()
    for line in path.read_text().splitlines() if path.exists() else []:
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    offers, report = acceptance(events)
    if not arguments.judge:
        return report, []
    # Offers seen but not used, graded against what was typed instead.
    graded = []
    context_counts = {
        "eligible": 0,
        "failed": 0,
        "pending": 0,
        "missing_conversation": 0,
    }
    report["judge_context"] = context_counts
    for number, o in enumerate(offers.values()):
        if not (o.get("shown") and o["seen"] and not o["used"]):
            continue
        context_counts["eligible"] += 1
        if not o.get("conversation"):
            context_counts["missing_conversation"] += 1
            continue
        answer = on_machine(
            o["machine"],
            [
                "actual",
                "--cli",
                o["cli"],
                "--conversation",
                o["conversation"],
                "--turn",
                str(o["turn"]),
            ],
        )
        if answer.get("error"):
            context_counts["failed"] += 1
            print(
                "actual for {}: {}".format(o["conversation"], answer["error"]),
                file=sys.stderr,
            )
        elif not answer.get("text"):
            context_counts["pending"] += 1
        if answer.get("text"):
            graded.append(
                {
                    "id": number,
                    "last": "",
                    "actual": answer["text"],
                    "candidates": o["candidates"],
                    "p": o["p"],
                }
            )
    if graded:
        grades = judge(graded, arguments.judge_model)
        report["judge_missing_grades"] = sum(g["id"] not in grades for g in graded)
        rows = [
            {
                "words": len(g["actual"].split()),
                "category": grades.get(g["id"], {}).get("category", "other"),
                "p": g["p"],
                "scores": grades.get(g["id"], {}).get("scores", []),
            }
            for g in graded
            if g["id"] in grades
        ]
        report["seen_not_used_graded"] = summarize(rows)
        report["grading"] = grading_coverage(len(graded), len(rows))
        return report, rows
    report["grading"] = grading_coverage(0, 0)
    return report, []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    parser.add_argument("--judge-model", default="claude-opus-5-5")
    parser.add_argument(
        "--keep-text", action="store_true", help="save prompts in the report"
    )
    modes = parser.add_subparsers(dest="mode", required=True)
    replay_mode = modes.add_parser("replay", help="predict sampled past prompts")
    replay_mode.add_argument(
        "--count", type=int, default=40, help="prompts per machine"
    )
    replay_mode.add_argument(
        "--since", default="", help="ISO date of the oldest prompt"
    )
    replay_mode.add_argument("--seed", type=int, default=0)
    replay_mode.add_argument(
        "--machine", action="append", help="an ssh host to sample too"
    )
    replay_mode.add_argument(
        "--exclude", nargs="*", help="conversation ids to leave out"
    )
    replay_mode.add_argument("--model", default="claude-opus-5-5")
    replay_mode.add_argument("--effort", default="")
    replay_mode.add_argument("--parallel", type=int, default=6)
    log_mode = modes.add_parser(
        "log", help="lapis's own predictions and what came of them"
    )
    log_mode.add_argument("--log", default="~/.lapis/runtime/next_prompt.jsonl")
    log_mode.add_argument(
        "--judge", action="store_true", help="grade offers seen but not used"
    )
    arguments = parser.parse_args(argv)
    report, rows = (replay if arguments.mode == "replay" else log)(arguments)
    folder = ROOT / "build" / "reports" / "next-prompt"
    folder.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%dT%H%M%S")
    receipt = folder / "{}-{}.json".format(arguments.mode, stamp)
    receipt.write_text(json.dumps({"report": report, "rows": rows}, indent=2))
    receipt.chmod(0o600)
    print(json.dumps(report, indent=2))
    if not arguments.json:
        print("receipt:", receipt.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
