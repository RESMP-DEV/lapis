#!/usr/bin/env python3
"""How well Tab's ranker picks the agent the person goes to next.

`log` reads lapis's own record (~/.lapis/runtime/tab_away.jsonl): every choice
the person made among waiting agents, with the features lapis computed, and
every Tab move. It replays the choices in time order and scores the fixed
order, the prior and the learned ranker refitted on the choices before each.

`history` rebuilds such choices from what lapis already kept before the ranker
existed: turn ends and guess impressions in next_prompt.jsonl and the times
the person typed to each Claude Code conversation on this Mac. An agent's turn
waits from its end to the next prompt; the first time the person reached it
(the guess was seen, or they replied) while others waited is a choice. It reads
only ids, categories, timestamps and prompt origins, never prompt text.

The ranker here mirrors apps/desktop/src/tab_ranker.cpp (features, prior and
fitting). Reports are numbers only, in build/reports/tab-away/.
"""

import argparse
import glob
import json
import math
import os
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FEATURES = (
    "work",
    "request",
    "unseen",
    "guess",
    "log_wait",
    "newest",
    "stale",
    "log_turns_hour",
    "log_since_turn",
)
PRIOR = (3.2, 3.0, 2.5, 0.3, -0.5, 0.5, -1.0, 0.3, -0.2)
PULL, STEP, ITERATIONS = 0.1, 0.3, 150
STALE_MINUTES, SINCE_CAP_MINUTES = 120.0, 3 * 24 * 60.0
REFIT_EVERY = 10


def features(candidates):
    """Rows as TabRanker::features computes them from raw candidate values."""
    if not candidates:
        return []
    newest = min(max(0.0, c["wait"]) for c in candidates)
    rows = []
    for c in candidates:
        wait = max(0.0, c["wait"])
        rows.append(
            [
                float(c.get("work", False)),
                float(c.get("request", False)),
                float(c.get("unseen", False)),
                float(c.get("guess", False)),
                math.log1p(wait),
                float(wait == newest),
                float(wait > STALE_MINUTES),
                math.log1p(max(0, c.get("turns_hour", 0))),
                math.log1p(min(max(c.get("since_turn", 1e9), 0.0), SINCE_CAP_MINUTES)),
            ]
        )
    return rows


def fit(decisions):
    """Weights and category weights fitted as fit_tab_ranker does."""
    weights = list(PRIOR)
    categories = {}
    if not decisions:
        return weights, categories
    n = float(len(decisions))
    for _ in range(ITERATIONS):
        gradient = [PULL * (w - p) for w, p in zip(weights, PRIOR)]
        category_gradient = {k: PULL * v for k, v in categories.items()}
        for d in decisions:
            scores = [
                categories.get(cat, 0.0) + sum(w * f for w, f in zip(weights, row))
                for row, cat in zip(d["x"], d["categories"])
            ]
            top = max(scores)
            p = [math.exp(s - top) for s in scores]
            total = sum(p)
            for c, (row, cat) in enumerate(zip(d["x"], d["categories"])):
                weight = p[c] / total - (1.0 if c == d["chosen"] else 0.0)
                for f, value in enumerate(row):
                    gradient[f] += weight * value / n
                category_gradient[cat] = category_gradient.get(cat, 0.0) + weight / n
        weights = [w - STEP * g for w, g in zip(weights, gradient)]
        for cat, g in category_gradient.items():
            categories[cat] = categories.get(cat, 0.0) - STEP * g
    return weights, categories


def learned_scores(decision, model):
    weights, categories = model
    return [
        categories.get(cat, 0.0) + sum(w * f for w, f in zip(weights, row))
        for row, cat in zip(decision["x"], decision["categories"])
    ]


def fixed_scores(decision):
    out = []
    for raw in decision["raw"]:
        if raw.get("guess") and not raw.get("guess_seen"):
            tier = 0
        elif raw.get("unseen") or raw.get("request"):
            tier = 1
        else:
            tier = 2
        out.append(-1e6 * tier + raw["wait"])
    return out


def rank_of(scores, chosen):
    """1-based rank of the chosen candidate; ties count against it."""
    return sum(1 for s in scores if s > scores[chosen]) + sum(
        1 for s in scores if s == scores[chosen]
    )


def evaluate(decisions, test_from):
    """Top-1 accuracy and mean reciprocal rank of each ranker on the test slice."""
    test = decisions[test_from:]
    if not test:
        return {}
    rankers = {
        "random": None,
        "oldest first": lambda d, _: [r["wait"] for r in d["raw"]],
        "newest first": lambda d, _: [-r["wait"] for r in d["raw"]],
        "work, then oldest": lambda d, _: [
            1e6 * r.get("work", False) + r["wait"] for r in d["raw"]
        ],
        "fixed (earlier order)": lambda d, _: fixed_scores(d),
        "prior": lambda d, _: learned_scores(d, (list(PRIOR), {})),
        "learned (refit every %d)" % REFIT_EVERY: lambda d, m: learned_scores(d, m),
    }
    report = {}
    models = {}
    for name, scorer in rankers.items():
        top1 = mrr = 0.0
        for i, d in enumerate(test):
            k = len(d["x"])
            if scorer is None:
                top1 += 1.0 / k
                mrr += sum(1.0 / r for r in range(1, k + 1)) / k
                continue
            block = i // REFIT_EVERY
            if block not in models and name.startswith("learned"):
                models[block] = fit(decisions[: test_from + block * REFIT_EVERY])
            r = rank_of(scorer(d, models.get(block)), d["chosen"])
            top1 += r == 1
            mrr += 1.0 / r
        report[name] = {"top1": top1 / len(test), "mrr": mrr / len(test)}
    return report


def from_log(path):
    """Choices as lapis logged them."""
    decisions, tabs = [], []
    for name in (str(path) + ".1", str(path)):
        if not os.path.exists(name):
            continue
        with open(name, encoding="utf-8", errors="replace") as lines:
            for line in lines:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                candidates = event.get("candidates") or []
                if event.get("event") == "tab":
                    tabs.append(event)
                if event.get("event") != "choice" or len(candidates) < 2:
                    continue
                x = [c.get("x") for c in candidates]
                if any(not isinstance(r, list) or len(r) != len(FEATURES) for r in x):
                    continue
                raw = [
                    {
                        "wait": math.expm1(r[4]),
                        "work": bool(r[0]),
                        "request": bool(r[1]),
                        "unseen": bool(r[2]),
                        "guess": bool(r[3]),
                        "guess_seen": bool(c.get("guess_seen")),
                    }
                    for r, c in zip(x, candidates)
                ]
                decisions.append(
                    {
                        "t": event.get("t", ""),
                        "x": x,
                        "raw": raw,
                        "categories": [c.get("category", "") for c in candidates],
                        "chosen": int(event.get("chosen", 0)),
                        "via": event.get("via", ""),
                    }
                )
    decisions.sort(key=lambda d: d["t"])
    return decisions, tabs


def utc(stamp):
    """Parse the ISO timestamps written by lapis and Claude Code transcripts."""
    text = str(stamp)
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    if "T" not in text and " " in text:
        text = text.replace(" ", "T", 1)
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as error:
        raise ValueError(f"unrecognized timestamp: {stamp!r}") from error
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.timestamp()


def evaluation_start(count, held_out):
    """Return the first test index for a held-out fraction in (0, 1]."""
    if not 0.0 < held_out <= 1.0:
        raise ValueError("--held-out must be in (0, 1]")
    return min(int(count * (1.0 - held_out)), count - 1)


def jsonl(path):
    out = []
    for name in sorted(glob.glob(str(path) + "*"), reverse=True):
        with open(name, encoding="utf-8", errors="replace") as lines:
            for line in lines:
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
    return out


def human_prompts(transcript):
    """Times the person typed into a Claude Code conversation (no text kept)."""
    times = []
    with open(transcript, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            if entry.get("type") != "user" or entry.get("isSidechain"):
                continue
            origin = entry.get("origin")
            content = (entry.get("message") or {}).get("content")
            if origin is not None:
                human = origin.get("kind") == "human"
            else:
                human = (
                    isinstance(content, str)
                    and not entry.get("isMeta")
                    and not content.startswith("<")
                )
            if human and entry.get("timestamp"):
                times.append(utc(entry["timestamp"]))
    return sorted(times)


def from_history(runtime, work, projects, stale_hours=24.0):
    """Choices rebuilt from next_prompt.jsonl, the registry and transcripts."""
    rows = jsonl(Path(runtime) / "next_prompt.jsonl")
    interactions = jsonl(Path(runtime) / "interaction.jsonl")
    registry = json.loads((Path(runtime) / "workspace.json").read_text())
    names = {c["id"]: c["name"] for c in registry.get("categories", [])}
    category = {a["id"]: a.get("category", "") for a in registry.get("agents", [])}
    for event in interactions:
        if event.get("kind") == "agent-focus" and event.get("session"):
            category.setdefault(event["session"], event.get("category", ""))
    finishes, conversations, seen = defaultdict(list), defaultdict(set), {}
    for event in rows:
        if event.get("event") == "seen" and event.get("offer"):
            seen.setdefault(event["offer"], utc(event["t"]))
    for event in rows:
        if event.get("event") not in ("predicted", "failed"):
            continue
        end = utc(event["t"]) - float(event.get("ms") or 0) / 1000.0
        finishes[event["agent"]].append(
            {"t": end, "offer": event.get("offer"), "shown": bool(event.get("shown"))}
        )
        if event.get("conversation"):
            conversations[event["agent"]].add(event["conversation"])
    prompts = defaultdict(list)
    for agent, ids in conversations.items():
        for conversation in ids:
            for transcript in glob.glob(f"{projects}/*/{conversation}.jsonl"):
                prompts[agent].extend(human_prompts(transcript))
        prompts[agent].sort()
    turns = []
    for agent, ends in finishes.items():
        ends.sort(key=lambda e: e["t"])
        for i, e in enumerate(ends):
            following = ends[i + 1]["t"] if i + 1 < len(ends) else math.inf
            reply = next((p for p in prompts[agent] if p > e["t"] + 1), None)
            if reply is not None and reply > following:
                reply = None
            saw = seen.get(e["offer"])
            arrive = (
                min(x for x in (saw, reply) if x is not None)
                if (saw or reply)
                else None
            )
            end = min(reply if reply is not None else math.inf, following)
            turns.append(
                {
                    "agent": agent,
                    "f": e["t"],
                    "end": min(end, e["t"] + stale_hours * 3600),
                    "arrive": arrive if arrive is not None and arrive <= end else None,
                    "guess": e["shown"],
                }
            )
    everyone = sorted((p, a) for a, ps in prompts.items() for p in ps)
    decisions = []
    for chosen in turns:
        t = chosen["arrive"]
        if t is None:
            continue
        last = None
        for p, a in everyone:
            if p >= t:
                break
            last = a
        if last == chosen["agent"]:
            continue  # stayed on the agent they were on: not a move
        waiting = [
            x for x in turns if x["f"] < t - 1 and x["end"] >= t and x["agent"] != last
        ]
        if chosen not in waiting:
            waiting.append(chosen)
        if len(waiting) < 2:
            continue
        raw = []
        for x in waiting:
            before = [p for p in prompts[x["agent"]] if p < t]
            group = category.get(x["agent"], "")
            raw.append(
                {
                    "wait": (t - x["f"]) / 60.0,
                    "work": names.get(group, "").lower() in work,
                    "unseen": x["arrive"] is None or x["arrive"] >= t,
                    "guess": x["guess"],
                    "guess_seen": x["arrive"] is not None and x["arrive"] < t,
                    "turns_hour": sum(1 for p in before if p >= t - 3600),
                    "since_turn": (t - before[-1]) / 60.0 if before else 1e9,
                    "category": group,
                }
            )
        decisions.append(
            {
                "t": t,
                "x": features(raw),
                "raw": raw,
                "categories": [r["category"] for r in raw],
                "chosen": waiting.index(chosen),
            }
        )
    decisions.sort(key=lambda d: d["t"])
    return decisions


def print_report(title, decisions, test_from, report, out):
    sizes = [len(d["x"]) for d in decisions[test_from:]]
    lines = [
        title,
        f"choices {len(decisions)}: fitted on {test_from}, scored on "
        f"{len(decisions) - test_from} (mean candidates "
        f"{sum(sizes) / max(len(sizes), 1):.1f})",
    ]
    for name, score in report.items():
        lines.append(f"  {name:28s} top-1 {score['top1']:.3f}  MRR {score['mrr']:.3f}")
    print("\n".join(lines))
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(
        json.dumps(
            {
                "choices": len(decisions),
                "fitted_on": test_from,
                "ranker": report,
            },
            indent=2,
        )
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--runtime", default=os.path.expanduser("~/.lapis/runtime"), type=Path
    )
    parser.add_argument(
        "--held-out",
        type=float,
        default=0.3,
        help="the newest share of choices scored (the rest only fits)",
    )
    parser.add_argument("--out", type=Path, default=ROOT / "build/reports/tab-away")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("log", help="the choices lapis logged")
    history = commands.add_parser("history", help="choices rebuilt from older logs")
    history.add_argument(
        "--work", action="append", default=[], help="a work category's name"
    )
    history.add_argument("--projects", default=os.path.expanduser("~/.claude/projects"))
    args = parser.parse_args(argv)
    if args.command == "log":
        decisions, tabs = from_log(args.runtime / "tab_away.jsonl")
        title = f"logged Tab moves {len(tabs)}"
    else:
        work = {w.lower() for w in args.work} or {"work"}
        decisions = from_history(args.runtime, work, args.projects)
        title = "rebuilt from next_prompt.jsonl and transcripts"
    if not decisions:
        print("no choices among two or more waiting agents yet")
        return 1
    try:
        test_from = evaluation_start(len(decisions), args.held_out)
    except ValueError as error:
        parser.error(str(error))
    print_report(title, decisions, test_from, evaluate(decisions, test_from), args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
