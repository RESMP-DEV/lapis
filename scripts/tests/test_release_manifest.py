"""Release manifests must fail closed before stale packages can publish."""

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import package_macos as package
from scripts import release_manifest as manifest

COMMIT = "a" * 40
TREE = "b" * 40
REMOTE_COMMIT = "c" * 40


class SourceSnapshotTests(unittest.TestCase):
    def test_snapshot_records_commit_tree_dirty_state_and_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(
                "project(lapis VERSION 0.5.0 LANGUAGES CXX)\n"
            )
            commands = []

            def capture(command):
                commands.append([str(part) for part in command])
                if command[3:] == ["rev-parse", "HEAD"]:
                    return COMMIT + "\n"
                if command[3:] == ["rev-parse", "HEAD^{tree}"]:
                    return TREE + "\n"
                if command[3:] == ["status", "--porcelain=v1"]:
                    return " M apps/desktop/main.cpp\n?? new.txt\n"
                raise AssertionError(command)

            snapshot = manifest.source_snapshot(root, capture)
        self.assertEqual(snapshot["commit"], COMMIT)
        self.assertEqual(snapshot["tree"], TREE)
        self.assertEqual(snapshot["version"], "0.5.0")
        self.assertFalse(snapshot["clean"])
        self.assertEqual(snapshot["status"], [" M apps/desktop/main.cpp", "?? new.txt"])
        self.assertEqual(len(commands), 3)


class ManifestBindingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.release = self.root / "build/release"
        self.manifest_path = self.release / "package-manifest.json"
        self.app = self.release / "stage/lapis.app"
        self.dmg = self.release / "lapis-macos-arm64.dmg"
        (self.root / "CMakeLists.txt").write_text(
            "project(lapis VERSION 0.5.0 LANGUAGES CXX)\n"
        )
        self.app.mkdir(parents=True)
        (self.app / "Contents").mkdir()
        (self.app / "Contents/lapis").write_bytes(b"app")
        self.dmg.write_bytes(b"dmg")
        self.dependencies = {
            "qt": {"version": "6", "source_sha256": {}},
            "sparkle": {"version": "1", "archive_sha256": "0" * 64},
        }

    def write_manifest(self):
        value = manifest.new_manifest(
            {
                "commit": COMMIT,
                "tree": TREE,
                "clean": True,
                "status": [],
                "version": "0.5.0",
            },
            self.dependencies,
        )
        manifest.write_manifest(self.manifest_path, value)
        return value

    def test_missing_and_malformed_manifests_are_rejected(self):
        with self.assertRaisesRegex(manifest.ManifestError, "manifest is missing"):
            manifest.load_manifest(self.manifest_path)
        self.manifest_path.write_text("{not json")
        with self.assertRaisesRegex(manifest.ManifestError, "manifest is malformed"):
            manifest.load_manifest(self.manifest_path)
        self.manifest_path.write_text("[]")
        with self.assertRaisesRegex(manifest.ManifestError, "JSON object"):
            manifest.load_manifest(self.manifest_path)

    def test_release_preflight_requires_exact_source_tag_and_bytes(self):
        self.write_manifest()
        manifest.set_artifacts(
            self.manifest_path,
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            provenance={"dmg_source_app_sha256": manifest.digest(self.app)},
        )
        manifest.set_notarized(self.manifest_path, True, True)
        manifest.set_verification(
            self.manifest_path,
            scope="notarized",
            artifacts={
                "app": manifest.digest(self.app),
                "dmg": manifest.digest(self.dmg),
            },
        )
        dependencies = {
            "qt": {"version": "6", "source_sha256": {}},
            "sparkle": {
                "version": "1",
                "archive_sha256": manifest.digest(self.sparkle_archive()),
            },
        }
        recorded = manifest.load_manifest(self.manifest_path)
        recorded["dependencies"] = dependencies
        manifest.write_manifest(self.manifest_path, recorded)
        self.assertEqual(
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                dependencies=dependencies,
                downloads=self.root / "downloads",
                appcast=None,
                release_url="https://example.test/releases",
                capture=self.capture,
                require_appcast=False,
            )["version"],
            "0.5.0",
        )

        with self.assertRaisesRegex(manifest.ManifestError, "source mismatch"):
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                dependencies=dependencies,
                downloads=self.root / "downloads",
                appcast=None,
                release_url="https://example.test/releases",
                capture=lambda command: self.capture(command, dirty=True),
                require_appcast=False,
            )
        self.dmg.write_bytes(b"changed DMG")
        with self.assertRaisesRegex(manifest.ManifestError, "artifact dmg mismatch"):
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                dependencies=dependencies,
                downloads=self.root / "downloads",
                appcast=None,
                release_url="https://example.test/releases",
                capture=self.capture,
                require_appcast=False,
            )

    def test_preflight_rejects_wrong_tag_and_stale_qualification(self):
        recorded = self.write_manifest()
        recorded["artifacts"] = {
            "app": manifest.digest(self.app),
            "dmg": manifest.digest(self.dmg),
        }
        recorded["provenance"] = {"dmg_source_app_sha256": manifest.digest(self.app)}
        recorded["notarized"] = {"app": True, "dmg": True}
        recorded["verification"] = {
            "scope": "signed",
            "passed": True,
            "artifacts": {
                "app": manifest.digest(self.app),
                "dmg": manifest.digest(self.dmg),
            },
        }
        manifest.write_manifest(self.manifest_path, recorded)
        arguments = {
            "root": self.root,
            "version": "0.5.0",
            "artifacts": {"app": self.app, "dmg": self.dmg},
            "dependencies": self.dependencies,
            "downloads": self.root / "downloads",
            "appcast": None,
            "release_url": "https://example.test/releases",
            "capture": self.capture,
            "require_appcast": False,
        }
        with self.assertRaisesRegex(manifest.ManifestError, "qualification was not"):
            manifest.preflight_release(self.manifest_path, tag="v0.5.0", **arguments)
        with self.assertRaisesRegex(manifest.ManifestError, "tag 'v9.0.0' is not"):
            manifest.preflight_release(self.manifest_path, tag="v9.0.0", **arguments)

    def test_failed_remote_branch_probe_is_not_treated_as_a_missing_tag(self):
        self.write_manifest()
        manifest.set_artifacts(
            self.manifest_path,
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            provenance={"dmg_source_app_sha256": manifest.digest(self.app)},
        )
        manifest.set_notarized(self.manifest_path, True, True)
        manifest.set_verification(
            self.manifest_path,
            scope="notarized",
            artifacts={
                "app": manifest.digest(self.app),
                "dmg": manifest.digest(self.dmg),
            },
        )
        sparkle_archive = self.sparkle_archive()
        recorded = manifest.load_manifest(self.manifest_path)
        self.dependencies["sparkle"]["archive_sha256"] = manifest.digest(
            sparkle_archive
        )
        recorded["dependencies"] = self.dependencies
        manifest.write_manifest(self.manifest_path, recorded)

        def failed_git_probe(command):
            if command[-4:] == ["branch", "-r", "--contains", COMMIT]:
                raise subprocess.CalledProcessError(128, command)
            return self.capture(command)

        with self.assertRaisesRegex(
            manifest.ManifestError, "cannot inspect remote branches"
        ):
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                dependencies=self.dependencies,
                downloads=self.root / "downloads",
                appcast=None,
                release_url="https://example.test/releases",
                capture=failed_git_probe,
                require_appcast=False,
            )

    def sparkle_archive(self):
        downloads = self.root / "downloads"
        downloads.mkdir(exist_ok=True)
        archive = downloads / "Sparkle-1.tar.xz"
        archive.write_bytes(b"sparkle")
        return archive

    def capture(self, command, *, dirty=False):
        command = [str(part) for part in command]
        arguments = command[3:]
        if arguments == ["rev-parse", "HEAD"]:
            return COMMIT + "\n"
        if arguments == ["rev-parse", "HEAD^{tree}"]:
            return TREE + "\n"
        if arguments == ["status", "--porcelain=v1"]:
            return " M source.cpp\n" if dirty else ""
        if arguments == ["branch", "-r", "--contains", COMMIT]:
            return "origin/main\n"
        if arguments == [
            "rev-parse",
            "--verify",
            "--end-of-options",
            "v0.5.0^{commit}",
        ]:
            return COMMIT + "\n"
        if arguments == [
            "rev-parse",
            "--verify",
            "--end-of-options",
            "v9.0.0^{commit}",
        ]:
            raise subprocess.CalledProcessError(128, command)
        raise AssertionError(command)


class ReleaseCommandTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.root.joinpath("CMakeLists.txt").write_text(
            "project(lapis VERSION 0.5.0 LANGUAGES CXX)\n"
        )
        self.release = self.root / "build/release"
        self.app = self.release / "stage/lapis.app"
        self.dmg = self.release / "lapis-macos-arm64.dmg"
        self.app.mkdir(parents=True)
        (self.app / "Contents").mkdir()
        (self.app / "Contents/lapis").write_bytes(b"final app")
        self.dmg.write_bytes(b"final dmg")
        self.appcast = self.release / "appcast.xml"
        self.downloads = self.release / "downloads"
        self.downloads.mkdir(parents=True)
        (self.downloads / "qtbase-everywhere-src-6.tar.xz").write_bytes(b"qt")
        (self.downloads / "Sparkle-1.tar.xz").write_bytes(b"sparkle")
        self.dependencies = {
            "qt": {
                "version": "6",
                "source_sha256": {
                    "qtbase": manifest.digest(
                        self.downloads / "qtbase-everywhere-src-6.tar.xz"
                    )
                },
            },
            "sparkle": {
                "version": "1",
                "archive_sha256": manifest.digest(self.downloads / "Sparkle-1.tar.xz"),
            },
        }
        self.manifest_path = self.release / "package-manifest.json"
        recorded = manifest.new_manifest(
            {
                "commit": COMMIT,
                "tree": TREE,
                "clean": True,
                "status": [],
                "version": "0.5.0",
            },
            self.dependencies,
        )
        manifest.write_manifest(self.manifest_path, recorded)
        manifest.set_artifacts(
            self.manifest_path,
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            provenance={"dmg_source_app_sha256": manifest.digest(self.app)},
        )
        manifest.set_notarized(self.manifest_path, True, True)
        self.command_log = []
        self.verify_commands = []
        self.published = False
        self.qualification_observer = None

    def write_appcast(self):
        signature = "ed" * 64
        length = str(self.dmg.stat().st_size)
        self.appcast.write_text(
            f'''<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel><item>
    <title>lapis 0.5.0</title>
    <sparkle:version>0.5.0</sparkle:version>
    <sparkle:shortVersionString>0.5.0</sparkle:shortVersionString>
    <enclosure url="https://example.test/releases/download/v0.5.0/{self.dmg.name}"
               type="application/octet-stream"
               sparkle:edSignature="{signature}" length="{length}"/>
  </item></channel>
</rss>
'''
        )

    def qualify(self, arguments):
        self.verify_commands.append(arguments)
        if arguments.get("fail"):
            raise package.PackageError("injected qualification failure")
        try:
            qualified = {"app": manifest.digest(self.app)}
            if arguments.get("notarized"):
                qualified["dmg"] = manifest.digest(self.dmg)
                manifest.set_notarized(self.manifest_path, True, True)
            manifest.set_verification(
                self.manifest_path,
                scope="notarized",
                artifacts=qualified,
            )
        except manifest.ManifestError as error:
            raise package.PackageError(str(error)) from error
        if self.qualification_observer is not None:
            self.qualification_observer()

    def run_command(self, command, **_kwargs):
        command = [str(part) for part in command]
        self.command_log.append(command)
        if command[:2] == ["xcrun", "stapler"]:
            return None
        if command[0] == "gh":
            self.published = True
            return None
        raise AssertionError(command)

    def capture(self, command):
        command = [str(part) for part in command]
        arguments = command[3:]
        if arguments == ["rev-parse", "HEAD"]:
            return COMMIT + "\n"
        if arguments == ["rev-parse", "HEAD^{tree}"]:
            return TREE + "\n"
        if arguments == ["status", "--porcelain=v1"]:
            return ""
        if arguments == ["branch", "-r", "--contains", COMMIT]:
            return "origin/main\n"
        if arguments == [
            "rev-parse",
            "--verify",
            "--end-of-options",
            "v0.5.0^{commit}",
        ]:
            return COMMIT + "\n"
        raise AssertionError(command)

    def invoke_release(self, *, verify_failure=False):
        arguments = type("Arguments", (), {})()
        arguments.tag = "v0.5.0"
        arguments.draft = True
        arguments.notarized = False

        def release_dependencies(_ghostty):
            return self.dependencies

        def write_appcast(_tag, _version):
            self.write_appcast()

        with (
            patch.object(package, "ROOT", self.root),
            patch.object(package, "RELEASE", self.release),
            patch.object(package, "APP", self.app),
            patch.object(package, "DMG", self.dmg),
            patch.object(package, "APPCAST", self.appcast),
            patch.object(package, "PACKAGE_MANIFEST", self.manifest_path),
            patch.object(package, "DOWNLOADS", self.downloads),
            patch.object(package, "RELEASES", "https://example.test/releases"),
            patch.object(package, "run", side_effect=self.run_command),
            patch.object(package, "capture", side_effect=self.capture),
            patch.object(package, "ghostty_prefix", return_value=self.root / "ghostty"),
            patch.object(package, "release_dependencies", release_dependencies),
            patch.object(
                package,
                "command_verify",
                side_effect=lambda value: self.qualify(
                    {
                        "notarized": value.notarized,
                        "fail": verify_failure,
                    }
                ),
            ),
            patch.object(package, "write_appcast", side_effect=write_appcast),
        ):
            package.command_release(arguments)

    def test_failed_qualification_refuses_publication(self):
        with self.assertRaisesRegex(package.PackageError, "qualification failure"):
            self.invoke_release(verify_failure=True)
        self.assertFalse(self.published)
        self.assertEqual([command[0] for command in self.command_log], ["xcrun"])
        self.assertFalse(self.appcast.exists())
        self.assertFalse(
            manifest.load_manifest(self.manifest_path)
            .get("verification", {})
            .get("passed")
        )

    def test_exact_match_qualifies_binds_appcast_then_publishes(self):
        self.invoke_release()
        self.assertTrue(self.published)
        self.assertEqual([command[0] for command in self.command_log], ["xcrun", "gh"])
        self.assertEqual(len(self.verify_commands), 1)
        self.assertTrue(self.verify_commands[0]["notarized"])
        binding = manifest.load_manifest(self.manifest_path)["release"]["appcast"]
        self.assertEqual(binding["tag"], "v0.5.0")
        self.assertEqual(binding["version"], "0.5.0")
        self.assertEqual(binding["sha256"], manifest.digest(self.appcast))
        self.assertEqual(binding["dmg_sha256"], manifest.digest(self.dmg))

    def test_changed_bytes_before_appcast_binding_refuses_publication(self):
        def mutate_after_qualification():
            self.dmg.write_bytes(b"stale replacement")

        self.qualification_observer = mutate_after_qualification
        with self.assertRaisesRegex(package.PackageError, "artifact dmg mismatch"):
            self.invoke_release()
        self.assertFalse(self.published)
        self.assertFalse(self.appcast.exists())

    def test_malformed_manifest_stops_before_staple_validation_writes(self):
        self.manifest_path.write_text("{}")
        with self.assertRaisesRegex(package.PackageError, "schema"):
            self.invoke_release()
        self.assertTrue(self.manifest_path.exists())
        self.assertEqual(
            self.command_log, [["xcrun", "stapler", "validate", str(self.dmg)]]
        )
        self.assertFalse(self.published)
