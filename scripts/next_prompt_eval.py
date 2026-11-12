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
import math
import os
import re
import subprocess
import sys
import tempfile
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
    prefix = "sample-{}-".format(datetime.now().strftime("%Y%m%dT%H%M%S"))
    fd, name = tempfile.mkstemp(prefix=prefix, suffix=".json", dir=MANIFESTS)
    path = Path(name)
    with os.fdopen(fd, "w") as out:
        json.dump({"items": items}, out)
    return path


HELPER = ROOT / "apps" / "desktop" / "src" / "next_prompt.py"
sys.path.insert(0, str(HELPER.parent))
import next_prompt  # noqa: E402

JUDGE = """You grade next-message predictions for a person supervising coding agents.
For each item you get the end of the agent's latest reply, the message the person
actually sent, and up to five predicted messages. Grade each prediction:
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
{"grades": [{"id": 0, "scores": [2, 0, 1, 0, 2], "category": "approve"}]}"""
THRESHOLDS = (0.0, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9)
# A redo: the person's message after the agent answered the guessed prompt
# pushes back, asks what it meant, tells it to continue, or asks for status.
# Each pattern reads only the message's start, as the history analysis that
# chose the prompt checks did (Oct 2026: 28% of tasks needed a redo, each
# round a median 47 minutes).
REDO = (
    (
        "pushback",
        220,
        r"^(no|nope|wrong|nah|stop)\b|that'?s not (it|what|right|true)|not what i|"
        r"\bi (said|asked|told you)\b|you didn'?t|why did you|why would you|"
        r"why are you|you'?re wrong|incorrect|doesn'?t make sense|makes no sense|"
        r"nonsense|\blie\b|\blying\b|\bfake\b|made (that|this|it) up|did you (even|actually)|"
        r"the whole point|i already (told|said|asked|did)|"
        r"still (not|doesn'?t|isn'?t|broken|wrong)|wtf|what the (fuck|hell)|"
        r"bullshit|stupid|useless|that'?s wrong",
    ),
    (
        "confused",
        300,
        r"what do you mean|what does .{1,40} mean\b|plain english|plain language|"
        r"i don'?t (understand|get it|follow)|confus|explain (this|that|it|again|like)|"
        r"eli5|in simpler terms|i'?m lost|unclear|too (dense|technical)|jargon",
    ),
    (
        "continue",
        80,
        r"^(keep going|continue|go on|carry on|keep at it|resume|keep pushing|"
        r"don'?t stop|and\?|so\?|finish (it|the rest)|you stopped|why did you stop)",
    ),
    (
        "status",
        60,
        r"^(status|eta|update|progress|how'?s it going|where are we|any update|how far)\b",
    ),
)
REDO_PATTERNS = tuple((kind, n, re.compile(rx, re.I)) for kind, n, rx in REDO)


def redo_kind(text):
    """The kind of redo a person's message is, or None."""
    text = (text or "").strip()
    for kind, n, pattern in REDO_PATTERNS:
        if pattern.search(text[:n]):
            return kind
    return None


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
    guess_words = sorted(r["guess_words"] for r in rows if "guess_words" in r)
    if guess_words:
        report["top_guess_words_median"] = guess_words[len(guess_words) // 2]
        report["top_guess_five_words_or_fewer"] = rate(
            [r for r in rows if "guess_words" in r], lambda r: r["guess_words"] <= 5
        )
        report["guessed_corrections"] = rate(
            [r for r in rows if "guess_category" in r],
            lambda r: r["guess_category"] == "correct",
        )
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
    items = []
    if arguments.manifest:
        try:
            loaded = json.loads(manifest_path(arguments.manifest).read_text())
        except (OSError, ValueError) as error:
            print("manifest: {}".format(error), file=sys.stderr)
            items = None
        else:
            items = loaded.get("items", loaded) if isinstance(loaded, dict) else loaded
    else:
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
                    # The same folder walls as live guesses, from lapis.json,
                    # resolved on each machine sampled.
                    "--walls",
                    next_prompt.config_walls(),
                ],
            )
            if "error" in sample:
                print(
                    "{}: {}".format(machine or "this Mac", sample["error"]),
                    file=sys.stderr,
                )
                continue
            items.extend(sample["items"])

    def stopped(message):
        report = {
            "error": message,
            "sampled": len(items or []),
            "predicted": 0,
            "prediction_failures": 0,
            "model": arguments.model,
            "context": arguments.context,
            "context_arm": arguments.context_arm,
            "prompt_arm": getattr(arguments, "prompt_arm", "base"),
            "grading": grading_coverage(0, 0),
        }
        return report, []

    if items is None:
        return stopped("manifest unavailable")
    if not isinstance(items, list):
        print(
            "manifest: expected an item list or an object containing one",
            file=sys.stderr,
        )
        return stopped("manifest is not an item list")
    for item in items:
        if not isinstance(item, dict):
            print("manifest: every item must be an object", file=sys.stderr)
            return stopped("manifest item is not an object")
        item.setdefault("machine", "")
        missing = [key for key in ("cli", "time", "actual") if key not in item]
        if missing:
            print(
                "manifest item is missing: {}".format(", ".join(missing)),
                file=sys.stderr,
            )
            return stopped("manifest item is incomplete")
        if "turns" in item and not isinstance(item["turns"], list):
            print("manifest item field is not a list: turns", file=sys.stderr)
            return stopped("manifest item is malformed")

    if items and not arguments.manifest:
        saved = save_manifest(items)
        print("manifest:", saved.relative_to(ROOT), file=sys.stderr)
    for number, item in enumerate(items):
        item["id"] = number

    oracle = {}
    if arguments.oracle:
        try:
            loaded_oracle = json.loads(Path(arguments.oracle).read_text())
        except (OSError, ValueError) as error:
            print("oracle file: {}".format(error), file=sys.stderr)
        else:
            if not isinstance(loaded_oracle, dict):
                print("oracle file: expected a JSON object", file=sys.stderr)
                return stopped("oracle is not an object")
            oracle = loaded_oracle

    if arguments.providers is None:
        providers = next_prompt.config_providers()
    else:
        try:
            providers = next_prompt.clean_providers(json.loads(arguments.providers))
        except (TypeError, ValueError) as error:
            print("providers JSON: {}".format(error), file=sys.stderr)
            return stopped("providers JSON is invalid")

    def guess(item):
        when = item["time"]
        try:
            when = datetime.fromisoformat(when.replace("Z", "+00:00")).astimezone()
            when = when.strftime("%a %-I:%M %p")
        except ValueError:
            pass
        turns = item.get("turns", [])
        if arguments.context == "last":
            # Only what the agent last said: whether the rest of the context
            # earns its cost.
            said = [t for t in turns if t["role"] == "agent"]
            turns = said[-1:]
        conversation = item.get("conversation", "")
        bundle = {
            "agent": {"cli": item["cli"], "machine": item["machine"]},
            "context": {
                "turns": turns,
                "recent": item.get("recent", []),
                "conversation": conversation,
            },
            "time": when,
            "target": item["time"],
        }
        if type(item.get("returned_minutes")) is int:
            bundle["returned_minutes"] = item["returned_minutes"]
        topic = oracle.get(conversation)
        if topic:
            bundle["oracle"] = topic
        return next_prompt.predict(
            bundle,
            arguments.model,
            arguments.effort,
            arm=arguments.context_arm,
            providers=providers,
            prompt_arm=getattr(arguments, "prompt_arm", "base"),
        )

    with ThreadPoolExecutor(arguments.parallel) as pool:
        guesses = list(pool.map(guess, items))
    graded = []
    for item, answer in zip(items, guesses):
        if answer.get("candidates"):
            agent_turns = [t for t in item.get("turns", []) if t["role"] == "agent"]
            graded.append(
                {
                    "id": item["id"],
                    "last": agent_turns[-1]["text"] if agent_turns else "",
                    "actual": item["actual"],
                    "candidates": answer["candidates"],
                    "ms": answer.get("ms", 0),
                    "guess_category": answer.get("category", "other"),
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
            "guess_words": len(str(g["candidates"][0].get("text", "")).split()),
            "guess_category": g["guess_category"],
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
    # The provider chain answers when one is configured, so `model` alone
    # does not say which model wrote the guesses.
    answered = defaultdict(int)
    for answer in guesses:
        if answer.get("candidates"):
            answered[answer.get("model") or arguments.model] += 1
    report["answered_by"] = dict(answered)
    report["context"] = arguments.context
    report["context_arm"] = arguments.context_arm
    report["prompt_arm"] = getattr(arguments, "prompt_arm", "base")
    report["oracle"] = bool(oracle)
    if graded:
        report["median_ms"] = sorted(g["ms"] for g in graded)[len(graded) // 2]
    return report, rows


def acceptance(events):
    """Offers by id from lapis's log, and the rates that matter: of the offers
    seen on screen, how many were used (sent with Tab, or typed with
    Option-Tab), how many after typing first, how fast, and what the settled
    prompt turned out to be.

    Older records carry fields of finished experiments (a model or local
    `arm`, `hidden` holdout guesses, a `min_confidence` floor, a `keep`
    return); they are read without those comparisons. A held-out guess was
    never shown, so it counts as predicted, not offered."""
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
                context_arm=str(e.get("context_arm") or "none"),
                prompt_arm=str(e.get("prompt_arm") or "none"),
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
        "skipped": sum(1 for e in events if e.get("event") == "skipped"),
        "by_category": {},
        "by_confidence": [],
    }
    # The live arms (see NextPromptSettings): context and prompt.
    contexts = defaultdict(list)
    for o in offers.values():
        if "shown" in o:
            contexts[o["context_arm"]].append(o)
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
                arm_waits[math.ceil(len(arm_waits) * 0.95) - 1] if arm_waits else None
            ),
            "outcome_own": sum(
                1 for o in part if (o["outcome"] or {}).get("result") == "own"
            ),
        }
    # The prompt arm is drawn apart from the context arm, so each compares
    # on every guess. A guess's length is what the checks aim to shorten.
    prompts = defaultdict(list)
    for o in offers.values():
        if "shown" in o:
            prompts[o["prompt_arm"]].append(o)
    report["by_prompt_arm"] = {}
    for arm, part in sorted(prompts.items()):
        arm_seen = [o for o in part if o["shown"] and o["seen"]]
        arm_used = [o for o in arm_seen if o["used"]]
        words = sorted(
            len(str((o["candidates"] or [{}])[0].get("text", "")).split()) for o in part
        )
        report["by_prompt_arm"][arm] = {
            "predicted": len(part),
            "seen": len(arm_seen),
            "used": len(arm_used),
            "acceptance": rate(arm_used, arm_seen),
            "top_guess_words_median": words[len(words) // 2] if words else None,
            "guessed_corrections": rate(
                [o for o in part if o["category"] == "correct"], part
            ),
        }
    # A return guesses again: was the fresh guess made, and then used? An
    # older record's `keep` return kept its old guess and is left out.
    reguesses = sorted(
        (o for o in offers.values() if isinstance(o.get("returned"), int)),
        key=lambda o: o["t"],
    )
    returns = {"returns": 0, "guessed": 0, "used": 0}
    for e in events:
        if e.get("event") != "return" or e.get("arm", "reguess") != "reguess":
            continue
        returns["returns"] += 1
        fresh = next(
            (
                o
                for o in reguesses
                if o["agent"] == e.get("agent") and o["t"] >= e.get("t", "")
            ),
            None,
        )
        if fresh:
            returns["guessed"] += 1
            returns["used"] += bool(fresh["used"])
    report["returns"] = returns
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


def next_messages(offers):
    """For each offer with a settled prompt, the person's message after the
    agent answered it: turn + 1 in the same conversation. Local transcripts
    are read once each; another machine's go through its helper in parallel."""
    prompts = {}
    found = {}
    remote = []
    for key, o in offers.items():
        if not (o.get("outcome") and o.get("conversation")):
            continue
        conversation, cli, machine = (
            o["conversation"],
            o.get("cli", "claude"),
            o.get("machine", ""),
        )
        turn = int(o.get("turn") or 0) + 1
        arguments = [
            "actual",
            "--cli",
            cli,
            "--conversation",
            conversation,
            "--turn",
            str(turn),
        ]
        if machine:
            remote.append((key, machine, arguments))
            continue
        if (cli, conversation) not in prompts:
            path = next_prompt.find_transcript(cli, conversation, "")
            prompts[(cli, conversation)] = (
                [
                    t["text"]
                    for t in next_prompt.turns_of(cli, path)
                    if t["role"] == "person"
                ]
                if path
                else None
            )
        said = prompts[(cli, conversation)]
        if said is None:
            found[key] = None
        else:
            found[key] = said[turn] if turn < len(said) else ""
    if remote:

        def remote_actual(request):
            return on_machine(request[1], request[2])

        with ThreadPoolExecutor(min(6, len(remote))) as pool:
            answers = list(pool.map(remote_actual, remote))
    else:
        answers = []
    for (key, _machine, _arguments), answer in zip(remote, answers):
        if answer.get("error"):
            found[key] = None
        elif answer.get("pending"):
            found[key] = ""
        elif isinstance(answer.get("text"), str):
            found[key] = answer["text"]
        else:
            found[key] = None
    return found


def redo_after(offers, found=None):
    """Of prompts settled after a guess (sent as offered, edited, or the
    person's own), how often the person's next message was a redo, by how
    the prompt was made and by prompt arm. Pending: no next message yet."""
    found = next_messages(offers) if found is None else found
    groups = defaultdict(
        lambda: {
            "settled": 0,
            "pending": 0,
            "missing": 0,
            "redo": 0,
            "kinds": defaultdict(int),
        }
    )

    def add(group, text):
        if text is None:
            group["missing"] += 1
            return
        group["settled"] += 1
        if not text:
            group["pending"] += 1
            return
        kind = redo_kind(text)
        if kind:
            group["redo"] += 1
            group["kinds"][kind] += 1

    for key, o in offers.items():
        if key not in found:
            continue
        result = (o.get("outcome") or {}).get("result", "other")
        text = found[key]
        add(groups["by_result:" + result], text)
        if result in ("as_offered", "edited"):
            add(groups["taken_by_prompt_arm:" + o.get("prompt_arm", "none")], text)
    report = {}
    for name, g in sorted(groups.items()):
        answered = g["settled"] - g["pending"]
        report[name] = {
            "settled": g["settled"],
            "answered": answered,
            "pending": g["pending"],
            "missing": g["missing"],
            "redo": g["redo"],
            "redo_rate": round(g["redo"] / answered, 3) if answered else None,
            "kinds": dict(g["kinds"]),
        }
    return report


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
    if getattr(arguments, "redo", False):
        report["redo"] = redo_after(offers)
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
        help="base asks for three candidates; wide asks for five",
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
    replay_mode.add_argument(
        "--prompt-arm",
        choices=next_prompt.PROMPT_ARMS,
        default="base",
        help="checks adds the helper's CHECKS to the system prompt",
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
    log_mode.add_argument(
        "--redo",
        action="store_true",
        help="read each settled prompt's next message and report how often it "
        "was a redo (pushback, confusion, continue, status)",
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
