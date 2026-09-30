"""Icon failures cannot preserve an earlier success or replace valid artwork."""

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from xml.etree import ElementTree as ET

from scripts import package_macos as package


MARK_FIXTURE = """<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
  <defs><filter id="glow"><feGaussianBlur stdDeviation="10"/></filter></defs>
  <g id="cabochon">
    <g id="stone"><ellipse cx="600" cy="500" rx="100" ry="200" fill="#000"/></g>
    <g id="star"><polygon points="600,300 620,500 580,500"/></g>
  </g>
</svg>
"""


def mark_svg(body):
    return f'<svg xmlns="http://www.w3.org/2000/svg">{body}</svg>'


class WriteMarkIconTests(unittest.TestCase):
    def write(self, svg):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        source = Path(temporary.name) / "source.svg"
        source.write_text(svg)
        target = Path(temporary.name) / "mark.svg"
        with patch.object(package, "ICON_SOURCE", source):
            package.write_mark_icon(target)
        return ET.parse(target).getroot()

    def test_square_view_box_over_stone_star_and_blur_margin(self):
        # Stone spans x 500..700, y 300..700; the margin is 3 x stdDeviation
        # (30); the square side is the height (460) and x pads by 100.
        root = self.write(MARK_FIXTURE)
        self.assertEqual(root.get("width"), "1024")
        self.assertEqual(root.get("height"), "1024")
        self.assertEqual(root.get("viewBox"), "370.0 270.0 460.0 460.0")
        self.assertIsNotNone(root.find(".//*[@id='glow']"))
        self.assertIsNotNone(root.find(".//*[@id='cabochon']"))

    def test_star_reaching_past_the_stone_expands_the_bounds(self):
        root = self.write(
            mark_svg(
                '<defs><filter id="glow"><feGaussianBlur stdDeviation="10"/>'
                "</filter></defs>"
                '<g id="cabochon"><g id="stone">'
                '<ellipse cx="600" cy="500" rx="100" ry="200"/>'
                '</g><g id="star"><polygon points="600,100 620,500 580,500"/>'
                "</g></g>"
            )
        )
        self.assertEqual(root.get("viewBox"), "270.0 70.0 660.0 660.0")

    def test_wide_stone_pads_the_other_axis(self):
        root = self.write(
            mark_svg(
                '<defs><filter id="glow"><feGaussianBlur stdDeviation="10"/>'
                "</filter></defs>"
                '<g id="cabochon"><g id="stone">'
                '<ellipse cx="600" cy="500" rx="300" ry="100"/>'
                "</g></g>"
            )
        )
        self.assertEqual(root.get("viewBox"), "270.0 170.0 660.0 660.0")

    def test_flat_coordinate_polygon_points_match_pair_form(self):
        flat = MARK_FIXTURE.replace(
            'points="600,300 620,500 580,500"', 'points="600 300 620 500 580 500"'
        )
        self.assertEqual(self.write(flat).get("viewBox"), "370.0 270.0 460.0 460.0")

    def test_two_value_std_deviation_uses_the_wider_axis(self):
        non_uniform = MARK_FIXTURE.replace('stdDeviation="10"', 'stdDeviation="4 10"')
        self.assertEqual(
            self.write(non_uniform).get("viewBox"), "370.0 270.0 460.0 460.0"
        )

    def test_malformed_star_points_and_blurs_are_rejected(self):
        cases = {
            "odd point count": (
                MARK_FIXTURE.replace(
                    'points="600,300 620,500 580,500"',
                    'points="600,300 620,500 580"',
                ),
                "x/y pairs",
            ),
            "non-numeric point": (
                MARK_FIXTURE.replace(
                    'points="600,300 620,500 580,500"',
                    'points="600,300 620,abc 580,500"',
                ),
                "points are not numeric",
            ),
            "non-numeric deviation": (
                MARK_FIXTURE.replace('stdDeviation="10"', 'stdDeviation="wide"'),
                "stdDeviation is not numeric",
            ),
            "three-value deviation": (
                MARK_FIXTURE.replace('stdDeviation="10"', 'stdDeviation="1 2 3"'),
                "one or two numbers",
            ),
        }
        for name, (svg, message) in cases.items():
            with self.subTest(malformed=name):
                temporary = tempfile.TemporaryDirectory()
                self.addCleanup(temporary.cleanup)
                source = Path(temporary.name) / "source.svg"
                source.write_text(svg)
                target = Path(temporary.name) / "mark.svg"
                with patch.object(package, "ICON_SOURCE", source):
                    with self.assertRaisesRegex(package.PackageError, message):
                        package.write_mark_icon(target)

    def test_missing_parts_are_rejected(self):
        cases = {
            "cabochon": (
                mark_svg(
                    '<defs><filter id="g">'
                    '<feGaussianBlur stdDeviation="1"/></filter></defs>'
                ),
                "cabochon and defs",
            ),
            "defs": (mark_svg('<g id="cabochon"/>'), "cabochon and defs"),
            "stone": (
                mark_svg(
                    '<defs><filter id="g">'
                    '<feGaussianBlur stdDeviation="1"/></filter></defs>'
                    '<g id="cabochon"/>'
                ),
                "stone group",
            ),
            "geometry": (
                mark_svg(
                    '<defs><filter id="g">'
                    '<feGaussianBlur stdDeviation="1"/></filter></defs>'
                    '<g id="cabochon"><g id="stone"/></g>'
                ),
                "ellipse or star geometry",
            ),
            "blur": (
                mark_svg(
                    '<defs><filter id="g"/></defs>'
                    '<g id="cabochon"><g id="stone">'
                    '<ellipse cx="600" cy="500" rx="100" ry="200"/>'
                    "</g></g>"
                ),
                "feGaussianBlur",
            ),
        }
        for name, (svg, message) in cases.items():
            with self.subTest(missing=name):
                temporary = tempfile.TemporaryDirectory()
                self.addCleanup(temporary.cleanup)
                source = Path(temporary.name) / "source.svg"
                source.write_text(svg)
                target = Path(temporary.name) / "mark.svg"
                with patch.object(package, "ICON_SOURCE", source):
                    with self.assertRaisesRegex(package.PackageError, message):
                        package.write_mark_icon(target)


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
                imageset = root / "apps/ios/Lapis/Assets.xcassets/LapisMark.imageset"
                mark_2x = imageset / "mark@2x.png"
                mark_3x = imageset / "mark@3x.png"
                assets = {
                    mac: b"previous mac",
                    phone: b"previous phone",
                    mark_2x: b"previous mark 2x",
                    mark_3x: b"previous mark 3x",
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
