"""Non-GUI regressions for GUI harness input geometry and soak setup."""

import argparse
import importlib
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

REPOSITORY = Path(__file__).resolve().parents[2]


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


if __name__ == "__main__":
    unittest.main()
