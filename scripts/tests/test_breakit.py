"""Non-GUI regressions for GUI harness input geometry and soak setup."""

import argparse
import importlib
import sys
import tempfile
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
    def test_cleanup_restores_original_files_and_allows_repeated_runs(self):
        breakit = load_breakit_with_xlib_stubs()
        for keep in (False, True):
            with self.subTest(keep=keep), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                agent = root / "tools/qa/fake_agent.py"
                agent.parent.mkdir(parents=True)
                agent.write_text("fixture")
                config, registry = root / "config.json", root / "workspace.json"
                if keep:
                    config.write_bytes(b"original config")
                    registry.write_bytes(b"original registry")
                with (
                    patch.object(breakit, "ROOT", root),
                    patch.object(breakit, "CONFIG", config),
                    patch.object(breakit, "REGISTRY", registry),
                    patch.object(breakit, "Keyboard"),
                ):
                    run = breakit.Run(root / "output", keep)
                    run.services = lambda: []
                    run.fake_agents = lambda: []
                    for _ in range(2):
                        run.prepare()
                        config.write_bytes(b"changed config")
                        registry.write_bytes(b"changed registry")
                        run.cleanup()
                        run.cleanup()  # Idempotent; never deletes restored bytes.
                        if keep:
                            self.assertEqual(config.read_bytes(), b"original config")
                            self.assertEqual(
                                registry.read_bytes(), b"original registry"
                            )
                        else:
                            self.assertFalse(config.exists())
                            self.assertFalse(registry.exists())

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
