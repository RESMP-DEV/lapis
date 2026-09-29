"""Icon failures cannot preserve an earlier success or replace valid artwork."""

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import package_macos as package


class IconGenerationTests(unittest.TestCase):
    OLD_MTIME = 1_700_000_000_000_000_000

    def write_previous(self, path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(0o640)
        os.utime(path, ns=(self.OLD_MTIME, self.OLD_MTIME))

    def assert_previous(self, assets):
        for path, content in assets.items():
            with self.subTest(path=path):
                self.assertEqual(path.read_bytes(), content)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o640)
                self.assertEqual(path.stat().st_mtime_ns, self.OLD_MTIME)

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

    def test_late_publication_failure_restores_the_previous_set(self):
        for failure in ("staged-copy", "replacement", "receipt"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                release = root / "build/release"
                receipt = release / "icon/receipt.json"
                receipt.parent.mkdir(parents=True)
                receipt.write_text('{"passed": true}\n')
                source = root / "source.svg"
                source.write_bytes(package.ICON_SOURCE.read_bytes())
                site = root / "site"
                mac = root / "mac.icns"
                phone = root / "phone.png"
                new_icon = site / "icon.png"
                favicon = site / "favicon.png"
                site_svg = site / "icon.svg"
                assets = {
                    mac: b"previous mac",
                    phone: b"previous phone",
                    favicon: b"previous favicon",
                    site_svg: b"previous site svg",
                }
                for target, content in assets.items():
                    self.write_previous(target, content)

                def generated(_command):
                    command = [str(part) for part in _command]
                    if command[0] == "rsvg-convert":
                        Path(command[6]).write_bytes(f"rendered {command[2]}".encode())
                    elif command[0] == "iconutil":
                        Path(command[4]).write_bytes(b"rendered icns")

                with (
                    patch.object(package, "ROOT", root),
                    patch.object(package, "RELEASE", release),
                    patch.object(package, "ICON_SOURCE", source),
                    patch.object(package, "MAC_ICON", mac),
                    patch.object(package, "PHONE_ICON", phone),
                    patch.object(
                        package.shutil, "which", side_effect=lambda name: name
                    ),
                    patch.object(package, "run", side_effect=generated),
                    patch.object(package, "capture", return_value="fake renderer"),
                ):
                    if failure == "staged-copy":
                        real_copy = shutil.copy2

                        def copy_or_fail(origin, target):
                            if origin.name == "site.png":
                                raise OSError("injected staged-copy failure")
                            return real_copy(origin, target)

                        context = patch.object(
                            package.shutil, "copy2", side_effect=copy_or_fail
                        )
                    else:
                        real_replace = os.replace

                        def replace_or_fail(origin, target):
                            if target == (receipt if failure == "receipt" else favicon):
                                raise OSError(f"injected {failure} failure")
                            return real_replace(origin, target)

                        context = patch.object(
                            package.os, "replace", side_effect=replace_or_fail
                        )
                    with (
                        context,
                        self.assertRaisesRegex(OSError, f"injected {failure} failure"),
                    ):
                        package.command_icon(None)

                self.assert_previous(assets)
                self.assertFalse(new_icon.exists())
                self.assertFalse(receipt.exists())
                self.assertEqual(
                    sorted(path.name for path in site.iterdir()),
                    ["favicon.png", "icon.svg"],
                )
                self.assertEqual(list(receipt.parent.iterdir()), [])

    def test_failed_rollback_preserves_recovery_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, first, second = (
                root / name for name in ("source", "first", "second")
            )
            source.write_bytes(b"new")
            first.write_bytes(b"old")
            publication = package.IconPublication()
            publication.stage(source, first)
            publication.stage(source, second)
            replace = os.replace

            def fail_publication_and_restore(origin, target):
                if target == second or origin.name.startswith(".lapis-old-"):
                    raise OSError("injected device failure")
                return replace(origin, target)

            with patch.object(
                package.os, "replace", side_effect=fail_publication_and_restore
            ):
                with self.assertRaisesRegex(package.PackageError, "recovery copy:"):
                    publication.commit(lambda _target: None)
            backups = list(root.glob(".lapis-old-*"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_bytes(), b"old")
            self.assertFalse(second.exists())
            self.assertEqual(list(root.glob(".lapis-new-*")), [])
