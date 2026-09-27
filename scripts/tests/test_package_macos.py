"""Icon failures cannot preserve an earlier success or replace valid artwork."""

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import package_macos as package


class IconGenerationTests(unittest.TestCase):
    def test_failed_generation_invalidates_receipt_and_preserves_assets(self):
        for failure in ("renderer", "iconutil", "svg", "render"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                release = root / "build/release"
                receipt = release / "icon/receipt.json"
                receipt.parent.mkdir(parents=True)
                receipt.write_text('{"passed": true}\n')
                source = root / "source.svg"
                source.write_bytes(package.ICON_SOURCE.read_bytes())
                if failure == "svg":
                    source.write_text("<broken")
                mac, phone = root / "mac.icns", root / "phone.png"
                mac.write_bytes(b"previous mac")
                phone.write_bytes(b"previous phone")

                def which(name):
                    missing = {"renderer": "rsvg-convert", "iconutil": "iconutil"}
                    return None if name == missing.get(failure) else name

                with (
                    patch.object(package, "ROOT", root),
                    patch.object(package, "RELEASE", release),
                    patch.object(package, "ICON_SOURCE", source),
                    patch.object(package, "MAC_ICON", mac),
                    patch.object(package, "PHONE_ICON", phone),
                    patch.object(package.shutil, "which", side_effect=which),
                    patch.object(
                        package,
                        "run",
                        side_effect=subprocess.CalledProcessError(1, "render"),
                    ),
                    self.assertRaises(
                        (package.PackageError, subprocess.SubprocessError)
                    ),
                ):
                    package.command_icon(None)
                self.assertFalse(receipt.exists())
                self.assertEqual(mac.read_bytes(), b"previous mac")
                self.assertEqual(phone.read_bytes(), b"previous phone")
                self.assertEqual(list(receipt.parent.iterdir()), [])
