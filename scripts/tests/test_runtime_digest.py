import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runtime_digest  # noqa: E402


def stamp(minutes_ago):
    at = datetime.now(timezone.utc) - timedelta(minutes=minutes_ago)
    return at.strftime("%Y-%m-%dT%H:%M:%S.000Z")


class DigestTest(unittest.TestCase):
    def test_counts_use_and_a_failing_streak(self):
        with tempfile.TemporaryDirectory() as runtime:
            events = [
                {
                    "event": "predicted",
                    "offer": "a:1",
                    "arm": "control",
                    "t": stamp(60 * 30),
                },
                {
                    "event": "predicted",
                    "offer": "a:2",
                    "arm": "control",
                    "t": stamp(50),
                },
                {"event": "seen", "offer": "a:2", "t": stamp(49)},
                {"event": "used", "offer": "a:2", "t": stamp(48)},
                {"event": "predicted", "offer": "b:1", "arm": "model", "t": stamp(40)},
                {
                    "event": "outcome",
                    "offer": "c:1",
                    "hidden": True,
                    "result": "own",
                    "t": stamp(39),
                },
            ] + [
                {
                    "event": "failed",
                    "stage": "predict",
                    "kind": "limit",
                    "t": stamp(30 - i),
                }
                for i in range(4)
            ]
            Path(runtime, "next_prompt.jsonl").write_text(
                "".join(json.dumps(e) + "\n" for e in events)
            )
            Path(runtime, "attention.jsonl").write_text(
                json.dumps({"decision": "none: judged fyi", "at": stamp(5)[:23]}) + "\n"
            )
            report = runtime_digest.digest(runtime, 24)
            guesses = report["guesses"]
            self.assertEqual((guesses["predicted"], guesses["attempts"]), (2, 6))
            self.assertEqual(guesses["failed_by_kind"], {"limit": 4})
            self.assertEqual(guesses["failing_now"], 4)
            self.assertEqual(guesses["by_arm"]["control"]["acceptance"], 1.0)
            self.assertEqual(guesses["held_out"], {"settled": 1, "exact": 0.0})
            self.assertEqual(report["pings"], {"judged fyi": 1})
            text = "\n".join(runtime_digest.lines(report))
            self.assertIn("FAILING NOW, 4 in a row", text)

    def test_malformed_offer_shapes_are_ignored_rather_than_crashing(self):
        with tempfile.TemporaryDirectory() as runtime:
            Path(runtime, "next_prompt.jsonl").write_text(
                "".join(
                    json.dumps(event) + "\n"
                    for event in (
                        {
                            "event": "predicted",
                            "offer": [],
                            "arm": "control",
                            "t": stamp(30),
                        },
                        {"event": "seen", "offer": {}, "t": stamp(29)},
                        {"event": "used", "offer": {"bad": True}, "t": stamp(28)},
                        {
                            "event": "predicted",
                            "offer": "good:1",
                            "arm": "control",
                            "t": stamp(27),
                        },
                    )
                )
            )
            guesses = runtime_digest.digest(runtime, 24)["guesses"]
            self.assertEqual((guesses["predicted"], guesses["attempts"]), (1, 1))


if __name__ == "__main__":
    unittest.main()
