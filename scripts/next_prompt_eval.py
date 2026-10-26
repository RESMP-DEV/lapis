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
import os
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Fixed samples: every replay that samples saves its items here (private
# transcript text, owner-only, ignored by git), so a comparison can be rerun on
# the identical prompts with `--manifest NAME`.
MANIFESTS = ROOT / "build" / "reports" / "next-prompt" / "manifests"


def manifest_path(name):
    """A manifest by path, or by name in MANIFESTS."""
    path = Path(name).expanduser()
    if path.exists() or path.is_absolute() or len(path.parts) > 1:
        return path
    return MANIFESTS / (name if name.endswith(".json") else name + ".json")


def save_manifest(items):
    MANIFESTS.mkdir(parents=True, exist_ok=True)
    path = MANIFESTS / "sample-{}.json".format(datetime.now().strftime("%Y%m%dT%H%M%S"))
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as out:
        json.dump({"items": items}, out)
    return path


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
        "top_scored": sum(1 for r in rows if r.get("top_scored") is True),
        "top_unscored": sum(1 for r in rows if r.get("top_scored") is False),
        "top_scored_unknown": sum(
            1
            for r in rows
            if r.get("top_scored") is not True and r.get("top_scored") is not False
        ),
        "top1_sendable": rate(rows, lambda r: r["scores"][:1] == [2]),
        "top1_intent": rate(rows, lambda r: (r["scores"] or [0])[0] >= 1),
        "top3_sendable": rate(rows, lambda r: 2 in r["scores"][:3]),
        "top3_intent": rate(rows, lambda r: max(r["scores"][:3] or [0]) >= 1),
        "top5_sendable": rate(rows, lambda r: 2 in r["scores"][:5]),
        "top5_intent": rate(rows, lambda r: max(r["scores"][:5] or [0]) >= 1),
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
            "top3_sendable": rate(part, lambda r: 2 in r["scores"][:3]),
        }
    groups = defaultdict(list)
    for r in rows:
        groups[r["category"]].append(r)
    for category, part in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        report["by_category"][category] = {
            "count": len(part),
            "top3_sendable": rate(part, lambda r: 2 in r["scores"][:3]),
            "top3_intent": rate(part, lambda r: max(r["scores"][:3] or [0]) >= 1),
        }
    # Offered only at or above a threshold: how often, and how often right.
    for threshold in THRESHOLDS:
        shown = [r for r in rows if r["p"] >= threshold]
        report["by_confidence"].append(
            {
                "min_confidence": threshold,
                "shown": round(len(shown) / n, 3),
                "scored": sum(1 for r in shown if r.get("top_scored") is True),
                "unscored": sum(1 for r in shown if r.get("top_scored") is False),
                "scored_unknown": sum(
                    1
                    for r in shown
                    if r.get("top_scored") is not True
                    and r.get("top_scored") is not False
                ),
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
    oracle = {}
    if arguments.oracle:
        try:
            oracle = json.loads(Path(arguments.oracle).read_text())
        except (OSError, ValueError) as error:
            print("oracle file: {}".format(error), file=sys.stderr)
    items = []
    if arguments.manifest:
        loaded = json.loads(manifest_path(arguments.manifest).read_text())
        items = loaded.get("items", loaded) if isinstance(loaded, dict) else loaded
        for item in items:
            item.setdefault("machine", "")
    for machine in [] if arguments.manifest else [""] + (arguments.machine or []):
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
    if items and not arguments.manifest:
        saved = save_manifest(items)
        print("manifest:", saved.relative_to(ROOT), file=sys.stderr)
    for number, item in enumerate(items):
        item["id"] = number

    if arguments.providers is None:
        providers = next_prompt.config_providers()
    else:
        try:
            providers = next_prompt.clean_providers(json.loads(arguments.providers))
        except (TypeError, ValueError):
            providers = []

    def guess(item):
        when = item["time"]
        try:
            when = datetime.fromisoformat(when.replace("Z", "+00:00")).astimezone()
            when = when.strftime("%a %-I:%M %p")
        except ValueError:
            pass
        turns = (
            item.get("legacy_turns", item["turns"])
            if arguments.context_arm == "legacy"
            else item["turns"]
        )
        if arguments.context == "last":
            # Only what the agent last said: whether the rest of the context
            # earns its cost.
            said = [t for t in turns if t["role"] == "agent"]
            turns = said[-1:]
        bundle = {
            "agent": {"cli": item["cli"], "machine": item["machine"]},
            "context": {
                "turns": turns,
                "recent": item.get("recent", []),
                "snippets": item.get("snippets", []),
                "conversation": item["conversation"],
            },
            "time": when,
            "target": item["time"],
        }
        if item.get("returned_minutes"):
            bundle["returned_minutes"] = item["returned_minutes"]
        topic = oracle.get(item["conversation"])
        if topic:
            bundle["oracle"] = topic
        return next_prompt.predict(
            bundle,
            arguments.model,
            arguments.effort,
            arm=arguments.context_arm,
            providers=providers,
        )

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
            "top_scored": (
                True
                if g["candidates"][0].get("scored") is True
                else False
                if g["candidates"][0].get("scored") is False
                else None
            ),
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
    report["context"] = arguments.context
    report["context_arm"] = arguments.context_arm
    report["oracle"] = bool(oracle)
    if graded:
        report["median_ms"] = sorted(g["ms"] for g in graded)[len(graded) // 2]
    return report, rows


def held_out(offers, settled_shown):
    """Prediction apart from acceptance: guesses made but never shown, against
    what the person then typed, beside the same figures for shown guesses
    (which the person could take, so they match more often)."""
    hidden = [o for o in offers.values() if o.get("hidden")]
    settled = [o for o in hidden if o["outcome"]]

    def figures(part):
        if not part:
            return {"settled": 0, "exact": None, "mean_similarity": None}
        exact = sum(1 for o in part if o["outcome"].get("result") == "as_offered")
        similarity = [float(o["outcome"].get("similarity", 0) or 0) for o in part]
        return {
            "settled": len(part),
            "exact": round(exact / len(part), 3),
            "mean_similarity": round(sum(similarity) / len(similarity), 3),
        }

    return {
        "made": len(hidden),
        "hidden": figures(settled),
        "shown": figures(settled_shown),
    }


def acceptance(events):
    """Offers by id from lapis's log, and the rates that matter: of the offers
    seen on screen, how many were used (sent with Tab, or typed with
    Option-Tab), how many after typing first, how fast, and what the settled
    prompt turned out to be."""
    offers = {}
    for e in events:
        key = e.get("offer")
        if not key:
            continue
        offer = offers.setdefault(
            key, {"seen": False, "used": None, "outcome": None, "withdrawn": None}
        )
        kind = e.get("event")
        if kind == "predicted":
            candidates = e.get("candidates") or [{}]
            top_scored = e.get("top_scored")
            if top_scored is None:
                top_scored = candidates[0].get("scored")
            top_scored = (
                True if top_scored is True else False if top_scored is False else None
            )
            offer.update(
                shown=bool(e.get("shown")),
                hidden=bool(e.get("hidden")),
                arm=str(e.get("arm") or "control"),
                context_arm=str(e.get("context_arm") or "none"),
                floor=e.get("min_confidence"),
                returned=e.get("returned_minutes"),
                agent=e.get("agent", ""),
                t=e.get("t", ""),
                p=float(candidates[0].get("p", 0) or 0),
                top_scored=top_scored,
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
        elif kind == "outcome":
            offer["outcome"] = e
        elif kind == "withdrawn":
            offer["withdrawn"] = e.get("reason")
    failures = defaultdict(int)
    failure_kinds = defaultdict(int)
    for e in events:
        if e.get("event") == "failed" and isinstance(e.get("kind"), str):
            failure_kinds[e["kind"][:16]] += 1
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
    settled = [o for o in shown if o["outcome"]]

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
        "offered_scored": sum(1 for o in shown if o["top_scored"] is True),
        "offered_unscored": sum(1 for o in shown if o["top_scored"] is False),
        "offered_top_scored_unknown": sum(1 for o in shown if o["top_scored"] is None),
        "seen": len(seen),
        "used": len(used),
        "acceptance": rate(used, seen),
        "sent_with_tab": sum(1 for o in used if o["used"].get("sent")),
        "typed_first_then_used": sum(
            1 for o in used if o["used"].get("typed_first", 0) > 0
        ),
        "seen_not_used": sum(1 for o in seen if not o["used"]),
        "outcomes": sum(1 for o in offers.values() if o["outcome"]),
        "outcome_settled": rate(settled, shown),
        "outcome_unsettled": len(shown) - len(settled),
        "outcome_settled_of_used": rate([o for o in used if o["outcome"]], used),
        "outcome_as_offered": sum(
            1
            for o in offers.values()
            if (o["outcome"] or {}).get("result") == "as_offered"
        ),
        "outcome_edited": sum(
            1 for o in offers.values() if (o["outcome"] or {}).get("result") == "edited"
        ),
        "outcome_own": sum(
            1 for o in offers.values() if (o["outcome"] or {}).get("result") == "own"
        ),
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
        "failed_by_kind": dict(sorted(failure_kinds.items(), key=lambda kv: -kv[1])),
        "held_out": held_out(offers, settled),
        "by_arm": {},
        "skipped": sum(1 for e in events if e.get("event") == "skipped"),
        "by_category": {},
        "by_confidence": [],
    }
    arms = defaultdict(list)
    for o in offers.values():
        if "shown" in o:
            arms[o["arm"]].append(o)
    for arm, part in sorted(arms.items()):
        arm_seen = [o for o in part if o["shown"] and o["seen"]]
        report["by_arm"][arm] = {
            "predicted": len(part),
            "seen": len(arm_seen),
            "acceptance": rate([o for o in arm_seen if o["used"]], arm_seen),
        }
    # The live arms (see NextPromptSettings): context, floor and return.
    contexts = defaultdict(list)
    floors = defaultdict(list)
    for o in offers.values():
        if "shown" in o:
            contexts[o["context_arm"]].append(o)
            floors[str(o["floor"])].append(o)
    report["by_context_arm"] = {}
    for arm, part in sorted(contexts.items()):
        arm_seen = [o for o in part if o["shown"] and o["seen"]]
        arm_used = [o for o in arm_seen if o["used"]]
        arm_waits = sorted(
            o["used"].get("ms_after_seen", -1)
            for o in arm_used
            if o["used"].get("ms_after_seen", -1) >= 0
        )
        report["by_context_arm"][arm] = {
            "predicted": len(part),
            "seen": len(arm_seen),
            "used": len(arm_used),
            "acceptance": rate(arm_used, arm_seen),
            "p95_ms_to_use": (
                arm_waits[min(len(arm_waits) - 1, int(len(arm_waits) * 0.95))]
                if arm_waits
                else None
            ),
            "outcome_own": sum(
                1 for o in part if (o["outcome"] or {}).get("result") == "own"
            ),
        }
    report["by_floor"] = {}
    for floor, part in sorted(floors.items()):
        floor_seen = [o for o in part if o["shown"] and o["seen"]]
        floor_used = [o for o in floor_seen if o["used"]]
        report["by_floor"][floor] = {
            "predicted": len(part),
            "offered": sum(1 for o in part if o["shown"]),
            "seen": len(floor_seen),
            "used": len(floor_used),
            "acceptance": rate(floor_used, floor_seen),
            "used_per_guess": rate(floor_used, part),
        }
    # A return keeps the old guess or guesses again; either way, was the
    # guess then on screen used?
    reguesses = sorted(
        (o for o in offers.values() if isinstance(o.get("returned"), int)),
        key=lambda o: o["t"],
    )
    returns = {"keep": [0, 0], "reguess": [0, 0, 0]}
    for e in events:
        if e.get("event") != "return" or e.get("arm") not in returns:
            continue
        counts = returns[e["arm"]]
        counts[0] += 1
        if e["arm"] == "keep":
            counts[1] += bool((offers.get(e.get("offer")) or {}).get("used"))
            continue
        fresh = next(
            (
                o
                for o in reguesses
                if o["agent"] == e.get("agent") and o["t"] >= e.get("t", "")
            ),
            None,
        )
        if fresh:
            counts[1] += 1
            counts[2] += bool(fresh["used"])
    report["returns"] = {
        "keep": {"returns": returns["keep"][0], "used": returns["keep"][1]},
        "reguess": {
            "returns": returns["reguess"][0],
            "guessed": returns["reguess"][1],
            "used": returns["reguess"][2],
        },
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
                "scored": sum(1 for o in part if o["top_scored"] is True),
                "unscored": sum(1 for o in part if o["top_scored"] is False),
                "scored_unknown": sum(
                    1
                    for o in part
                    if o["top_scored"] is not True and o["top_scored"] is not False
                ),
                "acceptance": rate([o for o in part if o["used"]], part),
            }
        )
    return offers, report


def log_files(path):
    """The log and what rotation moved aside, oldest first: dated archives
    (log.YYYYMMDD-HHMMSS), then log.1, then the log itself."""
    archives = sorted(path.parent.glob(path.name + ".2*"))
    return [p for p in archives + [Path(str(path) + ".1"), path] if p.is_file()]


def log(arguments):
    events = []
    path = Path(arguments.log).expanduser()
    for part in log_files(path):
        for line in part.read_text(errors="replace").splitlines():
            try:
                events.append(json.loads(line))
            except ValueError:
                continue
    offers, report = acceptance(events)
    if not arguments.judge:
        # Both report shapes carry the coverage field; without judging there
        # is nothing graded, which is the honest zero, not a missing key.
        report["grading"] = grading_coverage(0, 0)
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
        if not (o.get("shown") and o["seen"] and not o["used"] and not o["outcome"]):
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
                    "top_scored": o["top_scored"],
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
                "top_scored": g["top_scored"],
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
        "--manifest",
        default="",
        help="JSON sample (or item list) to replay instead of sampling again: a "
        "path, or a name in build/reports/next-prompt/manifests (every sampled "
        "replay saves one there)",
    )
    replay_mode.add_argument(
        "--machine", action="append", help="an ssh host to sample too"
    )
    replay_mode.add_argument(
        "--exclude", nargs="*", help="conversation ids to leave out"
    )
    replay_mode.add_argument("--model", default="claude-opus-5-5")
    replay_mode.add_argument(
        "--context",
        choices=("full", "last"),
        default="full",
        help="the conversation, or only the agent's last message",
    )
    replay_mode.add_argument(
        "--context-arm",
        choices=next_prompt.CONTEXT_ARMS,
        default="base",
        help="base is the final-block view; legacy is the old merged clip",
    )
    replay_mode.add_argument(
        "--oracle",
        default="",
        help="JSON file mapping conversation id to a topic hint",
    )
    replay_mode.add_argument(
        "--providers",
        default=None,
        help="JSON provider array; absent uses nextPrompt.providers from lapis.json",
    )
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
