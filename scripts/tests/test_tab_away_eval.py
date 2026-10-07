"""Tab's ranker evaluation: features, fitting and the logged choices it reads,
kept in step with apps/desktop/src/tab_ranker.cpp."""

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import tab_away_eval  # noqa: E402


def candidate(wait, **extra):
    return {"wait": wait, **extra}


def decision(raw, chosen, categories):
    return {
        "t": "",
        "x": tab_away_eval.features(raw),
        "raw": raw,
        "categories": categories,
        "chosen": chosen,
    }


class TabAwayEvalTest(unittest.TestCase):
    def test_matches_the_cpp_ranker(self):
        source = (ROOT / "apps/desktop/src/tab_ranker.cpp").read_text()
        prior = re.search(r"TabRanker::prior\(\) \{\s*return \{([^}]*)\}", source)
        self.assertIsNotNone(prior)
        values = tuple(float(v) for v in prior.group(1).split(","))
        self.assertEqual(values, tab_away_eval.PRIOR)
        for name, value in (
            ("kPull", tab_away_eval.PULL),
            ("kStep", tab_away_eval.STEP),
            ("kIterations", tab_away_eval.ITERATIONS),
        ):
            found = re.search(rf"{name} = ([0-9.]+);", source)
            self.assertIsNotNone(found, name)
            self.assertEqual(float(found.group(1)), float(value), name)

    def test_features(self):
        rows = tab_away_eval.features(
            [candidate(3, work=True, unseen=True), candidate(200, since_turn=1e9)]
        )
        self.assertEqual(rows[0][0], 1.0)
        self.assertEqual(rows[0][5], 1.0, "the newest")
        self.assertEqual(rows[1][5], 0.0)
        self.assertEqual(rows[1][6], 1.0, "stale after two hours")
        self.assertAlmostEqual(rows[1][8], 8.3714, places=3, msg="capped at three days")

    def test_features_empty(self):
        self.assertEqual(tab_away_eval.features([]), [])

    def test_fit_follows_choices_and_keeps_the_prior_without_them(self):
        weights, categories = tab_away_eval.fit([])
        self.assertEqual(tuple(weights), tab_away_eval.PRIOR)
        self.assertEqual(categories, {})
        raw = [candidate(3, work=True), candidate(3)]
        choices = [decision(raw, 1, ["job", "games"]) for _ in range(30)]
        model = tab_away_eval.fit(choices)
        scores = tab_away_eval.learned_scores(choices[0], model)
        self.assertGreater(scores[1], scores[0])
        self.assertLess(model[0][0], tab_away_eval.PRIOR[0])

    def test_evaluate_scores_each_ranker(self):
        newest = [
            decision([candidate(50), candidate(2)], 1, ["a", "a"]) for _ in range(12)
        ]
        report = tab_away_eval.evaluate(newest, 6)
        self.assertEqual(report["newest first"]["top1"], 1.0)
        self.assertEqual(report["oldest first"]["top1"], 0.0)
        self.assertAlmostEqual(report["random"]["top1"], 0.5)
        self.assertEqual(report["prior"]["top1"], 1.0)

    def test_reads_only_valid_logged_choices(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "tab_away.jsonl"
            good = {
                "event": "choice",
                "chosen": 1,
                "t": "2026-10-06T00:00:00Z",
                "candidates": [
                    {"agent": "a", "category": "c", "x": [0] * 9},
                    {"agent": "b", "category": "d", "x": [1] * 9, "guess_seen": True},
                ],
            }
            short = dict(good, candidates=[{"x": [0] * 9}])
            narrow = dict(good, candidates=[{"x": [0] * 3}, {"x": [0] * 3}])
            tab = {"event": "tab", "picked": 0, "candidates": good["candidates"]}
            log.write_text(
                "\n".join(json.dumps(e) for e in (good, short, narrow, tab))
                + "\nnot json\n"
            )
            decisions, tabs = tab_away_eval.from_log(log)
        self.assertEqual(len(decisions), 1)
        self.assertEqual(len(tabs), 1)
        self.assertEqual(decisions[0]["chosen"], 1)
        self.assertTrue(decisions[0]["raw"][1]["guess_seen"])


if __name__ == "__main__":
    unittest.main()
