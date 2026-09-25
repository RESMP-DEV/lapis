"""Non-GUI regressions for GUI harness input geometry and soak setup."""

import argparse
import base64
import contextlib
import io
import json
import importlib
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

REPOSITORY = Path(__file__).resolve().parents[2]
if str(REPOSITORY) not in sys.path:
    sys.path.insert(0, str(REPOSITORY))


def load_breakit_with_xlib_stubs():
    xlib = types.ModuleType("Xlib")
    x_module = types.ModuleType("Xlib.X")
    for name, value in {
        "KeyPress": 2,
        "KeyRelease": 3,
        "ButtonPress": 4,
        "ButtonRelease": 5,
        "MotionNotify": 6,
        "IsViewable": 2,
        "CurrentTime": 0,
    }.items():
        setattr(x_module, name, value)
    xk_module = types.ModuleType("Xlib.XK")
    xk_module.string_to_keysym = lambda name: 0
    display_module = types.ModuleType("Xlib.display")
    display_module.Display = lambda: None
    xlib.X = x_module
    xlib.XK = xk_module
    xlib.display = display_module
    xtest_module = types.ModuleType("Xlib.ext.xtest")
    xtest_module.fake_input = lambda *args, **kwargs: None
    ext_module = types.ModuleType("Xlib.ext")
    ext_module.xtest = xtest_module
    xlib.ext = ext_module
    stubs = {
        "Xlib": xlib,
        "Xlib.X": x_module,
        "Xlib.XK": xk_module,
        "Xlib.display": display_module,
        "Xlib.ext": ext_module,
        "Xlib.ext.xtest": xtest_module,
    }
    with patch.dict(sys.modules, stubs):
        sys.path.insert(0, str(REPOSITORY))
        try:
            return importlib.import_module("tools.qa.breakit")
        finally:
            sys.path.remove(str(REPOSITORY))


class SoakTests(unittest.TestCase):
    def test_empty_sample_window_fails_instead_of_indexing(self):
        breakit = load_breakit_with_xlib_stubs()
        run = breakit.Run.__new__(breakit.Run)
        run.fake_agents = lambda: []
        run.gui_alive = lambda: False
        run.keys = None
        run.new_agent = lambda name: None
        run.new_category = lambda name: None
        run.next_agent = lambda: None
        run.shot = lambda label: f"{label}.png"
        run.usage = lambda: ({"rss": 0, "cpu": 0}, {"rss": 0, "cpu": 0, "count": 0})
        with self.assertRaisesRegex(breakit.Failure, "pass a positive --soak"):
            run.soak(0)

    def test_non_positive_soak_is_rejected_before_setup(self):
        breakit = load_breakit_with_xlib_stubs()
        for value in (-1, 0):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    breakit.positive_minutes(str(value))
        self.assertEqual(breakit.positive_minutes("1.5"), 1.5)


class ResumeFixtureTests(unittest.TestCase):
    def test_conversations_are_paired_with_their_receiving_harness(self):
        breakit = load_breakit_with_xlib_stubs()
        expected = {
            ("grok", "/fixture/project-a", "grok-thread"),
            ("kimi", "/fixture/project-b", "kimi-thread"),
        }
        valid = [
            {
                "args": "python3 /fixture/bin/grok --resume grok-thread",
                "cwd": "/fixture/project-a",
            },
            {
                "args": "python3 /fixture/bin/kimi --session kimi-thread",
                "cwd": "/fixture/project-b",
            },
        ]
        swapped = [
            {
                "args": "python3 /fixture/bin/grok --resume kimi-thread",
                "cwd": "/fixture/project-a",
            },
            {
                "args": "python3 /fixture/bin/kimi --session grok-thread",
                "cwd": "/fixture/project-b",
            },
        ]
        self.assertTrue(expected <= breakit.resumed_pairs(valid, Path("/fixture/bin")))
        self.assertFalse(
            expected <= breakit.resumed_pairs(swapped, Path("/fixture/bin"))
        )

    def test_same_harness_conversations_are_paired_by_directory(self):
        breakit = load_breakit_with_xlib_stubs()
        expected = {
            ("grok", "/fixture/project-a", "grok-a"),
            ("grok", "/fixture/project-b", "grok-b"),
        }
        valid = [
            {
                "args": "python3 /fixture/bin/grok --resume grok-a",
                "cwd": "/fixture/project-a",
            },
            {
                "args": "python3 /fixture/bin/grok --resume grok-b",
                "cwd": "/fixture/project-b",
            },
        ]
        swapped = [
            {
                "args": "python3 /fixture/bin/grok --resume grok-b",
                "cwd": "/fixture/project-a",
            },
            {
                "args": "python3 /fixture/bin/grok --resume grok-a",
                "cwd": "/fixture/project-b",
            },
        ]
        self.assertTrue(expected <= breakit.resumed_pairs(valid, Path("/fixture/bin")))
        self.assertFalse(
            expected <= breakit.resumed_pairs(swapped, Path("/fixture/bin"))
        )

    def test_agy_checkpoint_retains_its_explicit_conversation(self):
        from tools.qa import fake_agent

        output = io.StringIO()
        with patch.object(
            sys, "argv", ["agy", "--conversation", "retained-agy-thread"]
        ):
            with (
                patch.object(fake_agent, "NAME", "agy"),
                contextlib.redirect_stdout(output),
            ):
                conversation, resumed = fake_agent.checkpoint()
        self.assertEqual(conversation, "retained-agy-thread")
        self.assertTrue(resumed)
        payload = output.getvalue().split("agent_checkpoint=", 1)[1].split("\x07", 1)[0]
        record = json.loads(base64.b64decode(payload))
        self.assertEqual(record["session_id"], "retained-agy-thread")
        self.assertEqual(record["agent"], "agy")


if __name__ == "__main__":
    unittest.main()
