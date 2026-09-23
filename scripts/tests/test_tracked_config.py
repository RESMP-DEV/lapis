"""The committed lapis.json must not pin keybindings over the platform defaults.

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


if __name__ == "__main__":
    unittest.main()
