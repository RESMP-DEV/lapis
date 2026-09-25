"""The committed lapis.json must hold defaults only.

The app rewrites lapis.json in place, so local overrides are normal; this reads
the committed file. A committed override silently replaces the built-in
bindings on every fresh checkout (it once bound Control-W and Control-Q, which
the terminal needs).
"""

import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class TrackedConfig(unittest.TestCase):
    def test_committed_config_keeps_default_bindings(self):
        result = subprocess.run(
            ["git", "show", "HEAD:lapis.json"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            self.skipTest("lapis.json is not committed in this checkout")
        config = json.loads(result.stdout)
        self.assertEqual(config.get("keybindings", {}), {})
        # Appearance and sidebar choices are per-user state, not project config.
        self.assertEqual(set(config) - {"version", "keybindings"}, set())


if __name__ == "__main__":
    unittest.main()
