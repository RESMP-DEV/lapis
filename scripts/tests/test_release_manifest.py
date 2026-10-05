"""Release manifests must fail closed before stale packages can publish."""

import contextlib
import json
import plistlib
import subprocess
import tempfile
import unittest
import warnings
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import Mock, patch

from scripts import package_macos as package
from scripts import release_manifest as manifest

COMMIT = "a" * 40
TREE = "b" * 40
REMOTE_COMMIT = "c" * 40
GATE_TIMESTAMP = "2026-10-04T00:00:00Z"


def write_candidate_gate_map(
    directory, source_revision, version, artifacts, appcast, timestamp=GATE_TIMESTAMP
):
    """Create a minimal, sanitized map with one receipt per required gate."""
    directory.mkdir(parents=True, exist_ok=True)
    gates = {}
    for name in manifest.CANDIDATE_GATE_NAMES:
        receipt_name = f"{name}.json"
        appcast_digest = (
            appcast if name in manifest.CANDIDATE_GATES_REQUIRING_APPCAST else None
        )
        identity = {
            "source_revision": source_revision,
            "version": version,
            "artifacts": dict(artifacts),
            "appcast_digest": appcast_digest,
            "recorded_at": timestamp,
            "command": f"run candidate check: {name}",
            "exit_status": 0,
        }
        receipt = {
            "schema_version": 1,
            "receipt_kind": manifest.CANDIDATE_GATE_RECEIPT_KIND,
            "scope": name,
            "passed": True,
            "gate": name,
            **identity,
        }
        (directory / receipt_name).write_text(
            json.dumps(receipt, sort_keys=True), encoding="utf-8"
        )
        gates[name] = {
            **identity,
            "receipt": receipt_name,
            "receipt_sha256": manifest.content_sha256(directory / receipt_name),
        }
    gate_map = {
        "schema_version": manifest.CANDIDATE_GATE_MAP_SCHEMA_VERSION,
        "map_kind": manifest.CANDIDATE_GATE_MAP_KIND,
        "gates": gates,
    }
    path = directory / "gate-map.json"
    path.write_text(json.dumps(gate_map, sort_keys=True), encoding="utf-8")
    return path


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


class GateTimestampTests(unittest.TestCase):
    def test_explicit_utc_timestamps_are_valid_and_other_shapes_are_not(self):
        for timestamp in (
            "2026-10-04T00:00:00Z",
            "2026-10-04T00:00:00z",
            "2026-10-04T00:00:00.000Z",
            "2026-10-04T00:00:00+00:00",
            "2026-10-04T00:00:00-00:00",
        ):
            with self.subTest(timestamp=timestamp):
                self.assertTrue(manifest._utc_timestamp(timestamp))
        for timestamp in (
            None,
            "",
            "2026-10-04",
            "2026-10-04 00:00:00Z",
            "2026-10-04_00:00:00Z",
            "2026-10-04q00:00:00Z",
            "2026-10-04T00:00:00",
            "2026-10-04T00:00:00+01:00",
            "2026-13-04T00:00:00Z",
        ):
            with self.subTest(timestamp=timestamp):
                self.assertFalse(manifest._utc_timestamp(timestamp))


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

    def write_appcast(self):
        appcast = self.release / "appcast.xml"
        signature = "ed" * 64
        appcast.write_text(
            f'''<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel><item>
    <title>lapis 0.5.0</title>
    <sparkle:version>0.5.0</sparkle:version>
    <sparkle:shortVersionString>0.5.0</sparkle:shortVersionString>
    <enclosure url="https://example.test/releases/download/v0.5.0/{self.dmg.name}"
               type="application/octet-stream"
               sparkle:edSignature="{signature}" length="{self.dmg.stat().st_size}"/>
  </item></channel>
</rss>
'''
        )
        return appcast

    def qualified(self, dependencies):
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
        recorded = manifest.load_manifest(self.manifest_path)
        recorded["dependencies"] = dependencies
        manifest.write_manifest(self.manifest_path, recorded)

    def rewrite_gate_timestamps(self, gate_map_path, timestamp):
        gate_map = json.loads(gate_map_path.read_text(encoding="utf-8"))
        for entry in gate_map["gates"].values():
            entry["recorded_at"] = timestamp
            receipt_path = gate_map_path.parent / entry["receipt"]
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            receipt["recorded_at"] = timestamp
            receipt_path.write_text(
                json.dumps(receipt, sort_keys=True), encoding="utf-8"
            )
            entry["receipt_sha256"] = manifest.content_sha256(receipt_path)
        gate_map_path.write_text(json.dumps(gate_map, sort_keys=True), encoding="utf-8")

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
            "qt": {
                "version": "6",
                "source_sha256": {
                    "qtbase": manifest.content_sha256(self.qtbase_archive())
                },
            },
            "sparkle": {
                "version": "1",
                "archive_sha256": manifest.content_sha256(self.sparkle_archive()),
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

    def test_release_requires_a_complete_digest_bound_gate_map(self):
        self.write_manifest()
        dependencies = {
            "qt": {
                "version": "6",
                "source_sha256": {
                    "qtbase": manifest.content_sha256(self.qtbase_archive())
                },
            },
            "sparkle": {
                "version": "1",
                "archive_sha256": manifest.content_sha256(self.sparkle_archive()),
            },
        }
        self.dependencies = dependencies
        self.qualified(dependencies)
        appcast = self.write_appcast()
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        arguments = {
            "root": self.root,
            "version": "0.5.0",
            "artifacts": {"app": self.app, "dmg": self.dmg},
            "dependencies": dependencies,
            "downloads": self.root / "downloads",
            "appcast": appcast,
            "release_url": "https://example.test/releases",
            "capture": self.capture,
            "require_appcast": True,
            "require_candidate_gates": True,
        }
        with self.assertRaisesRegex(
            manifest.ManifestError, "candidate gate map is missing"
        ):
            manifest.preflight_release(self.manifest_path, tag="v0.5.0", **arguments)

        gate_map = write_candidate_gate_map(
            self.release / "candidate-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=gate_map,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )
        self.assertEqual(
            manifest.preflight_release(self.manifest_path, tag="v0.5.0", **arguments)[
                "version"
            ],
            "0.5.0",
        )

    def test_gate_timestamps_may_use_any_valid_utc_form(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        gate_map = write_candidate_gate_map(
            self.release / "candidate-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        for timestamp in (
            "2026-10-04T00:00:00Z",
            "2026-10-04T00:00:00.000Z",
            "2026-10-04T00:00:00+00:00",
        ):
            with self.subTest(timestamp=timestamp):
                self.rewrite_gate_timestamps(gate_map, timestamp)
                bound = manifest.bind_candidate_gates(
                    self.manifest_path,
                    gate_map_path=gate_map,
                    source_revision=COMMIT,
                    version="0.5.0",
                    artifacts={"app": self.app, "dmg": self.dmg},
                    appcast_digest=manifest.digest(appcast),
                    replace=True,
                )
                self.assertEqual(
                    bound["candidate_gates"]["gates"]["downloaded_asset"][
                        "recorded_at"
                    ],
                    timestamp,
                )

    def test_schema1_accepts_only_matching_legacy_appcast_field(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()

        def rename_appcast_field(gate_map_path):
            gate_map = json.loads(gate_map_path.read_text(encoding="utf-8"))
            for entry in gate_map["gates"].values():
                entry["appcast_sha256"] = entry.pop("appcast_digest")
                receipt_path = gate_map_path.parent / entry["receipt"]
                receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
                receipt["appcast_sha256"] = receipt.pop("appcast_digest")
                receipt_path.write_text(
                    json.dumps(receipt, sort_keys=True), encoding="utf-8"
                )
                entry["receipt_sha256"] = manifest.content_sha256(receipt_path)
            gate_map_path.write_text(json.dumps(gate_map, sort_keys=True))

        legacy_map = write_candidate_gate_map(
            self.release / "legacy-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        rename_appcast_field(legacy_map)
        with warnings.catch_warnings(record=True) as observed:
            warnings.simplefilter("always")
            bound = manifest.bind_candidate_gates(
                self.manifest_path,
                gate_map_path=legacy_map,
                source_revision=COMMIT,
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                appcast_digest=manifest.digest(appcast),
            )
        self.assertTrue(observed)
        self.assertTrue(
            all(
                issubclass(item.category, DeprecationWarning)
                and "appcast_sha256" in str(item.message)
                and "appcast_digest" in str(item.message)
                for item in observed
            )
        )
        for name, entry in bound["candidate_gates"]["gates"].items():
            self.assertEqual(
                entry["appcast_digest"],
                manifest.digest(appcast)
                if name in manifest.CANDIDATE_GATES_REQUIRING_APPCAST
                else None,
            )
            self.assertNotIn("appcast_sha256", entry)

    def test_gate_binding_rejects_a_different_already_bound_appcast(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        bound_appcast = self.write_appcast()
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=bound_appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        replacement_appcast = self.release / "replacement-appcast.xml"
        replacement_appcast.write_bytes(bound_appcast.read_bytes() + b"\n")
        replacement_map = write_candidate_gate_map(
            self.release / "replacement-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(replacement_appcast),
        )
        with self.assertRaisesRegex(
            manifest.ManifestError,
            "candidate gates do not cover the already-bound release appcast",
        ):
            manifest.bind_candidate_gates(
                self.manifest_path,
                gate_map_path=replacement_map,
                source_revision=COMMIT,
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                appcast_digest=manifest.digest(replacement_appcast),
            )

        conflicting_map = write_candidate_gate_map(
            self.release / "conflicting-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(bound_appcast),
        )
        conflicting = json.loads(conflicting_map.read_text(encoding="utf-8"))
        conflicting["gates"]["downloaded_asset"]["appcast_sha256"] = "0" * 64
        conflicting_map.write_text(json.dumps(conflicting, sort_keys=True))
        with self.assertRaisesRegex(
            manifest.ManifestError, "conflicting appcast_digest"
        ):
            manifest.bind_candidate_gates(
                self.manifest_path,
                gate_map_path=conflicting_map,
                source_revision=COMMIT,
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                appcast_digest=manifest.digest(bound_appcast),
            )

    def test_opening_preflight_leaves_appcast_gates_to_final_preflight(self):
        self.write_manifest()
        self.dependencies["qt"]["source_sha256"]["qtbase"] = manifest.content_sha256(
            self.qtbase_archive()
        )
        self.dependencies["sparkle"]["archive_sha256"] = manifest.content_sha256(
            self.sparkle_archive()
        )
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        gate_map = write_candidate_gate_map(
            self.release / "candidate-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=gate_map,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )

        opening_arguments = {
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
        self.assertEqual(
            manifest.preflight_release(
                self.manifest_path, tag="v0.5.0", **opening_arguments
            )["version"],
            "0.5.0",
        )
        self.assertEqual(
            manifest.preflight_release(
                self.manifest_path,
                tag="v0.5.0",
                **{**opening_arguments, "appcast": appcast, "require_appcast": True},
            )["version"],
            "0.5.0",
        )

    def test_failed_gates_are_replaceable_but_approved_gates_are_immutable(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        arguments = {
            "source_revision": COMMIT,
            "version": "0.5.0",
            "artifacts": {"app": self.app, "dmg": self.dmg},
            "appcast_digest": manifest.digest(appcast),
        }
        initial_map = write_candidate_gate_map(
            self.release / "initial-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path, gate_map_path=initial_map, **arguments
        )
        differing_map = write_candidate_gate_map(
            self.release / "differing-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        self.rewrite_gate_timestamps(differing_map, "2026-10-05T00:00:00Z")
        with self.assertRaisesRegex(
            manifest.ManifestError, "candidate gates are already bound"
        ):
            manifest.bind_candidate_gates(
                self.manifest_path, gate_map_path=differing_map, **arguments
            )

        bound = manifest.load_manifest(self.manifest_path)
        for entry in bound["candidate_gates"]["gates"].values():
            entry["passed"] = False
        manifest.write_manifest(self.manifest_path, bound)
        replaced = manifest.bind_candidate_gates(
            self.manifest_path, gate_map_path=differing_map, **arguments
        )
        self.assertTrue(
            all(
                entry["passed"]
                for entry in replaced["candidate_gates"]["gates"].values()
            )
        )
        superseded = replaced["superseded_candidate_gates"]
        self.assertEqual(superseded, [bound["candidate_gates"]])
        self.assertFalse(
            all(entry["passed"] for entry in superseded[0]["gates"].values())
        )

    def test_malformed_and_artifact_replaced_gate_maps_fail_closed(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        gate_map = write_candidate_gate_map(
            self.release / "candidate-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        (gate_map.parent / "downloaded_asset.json").unlink()
        with self.assertRaisesRegex(
            manifest.ManifestError, "candidate gate downloaded_asset receipt is missing"
        ):
            manifest.bind_candidate_gates(
                self.manifest_path,
                gate_map_path=gate_map,
                source_revision=COMMIT,
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                appcast_digest=manifest.digest(appcast),
            )

        declared_digest = write_candidate_gate_map(
            self.release / "declared-digest",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        declared_map = json.loads(declared_digest.read_text(encoding="utf-8"))
        declared_map["gates"]["downloaded_asset"]["receipt_sha256"] = "f" * 64
        declared_digest.write_text(json.dumps(declared_map, sort_keys=True))
        with self.assertRaisesRegex(
            manifest.ManifestError,
            "declared candidate receipt SHA-256 mismatch",
        ):
            manifest.bind_candidate_gates(
                self.manifest_path,
                gate_map_path=declared_digest,
                source_revision=COMMIT,
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                appcast_digest=manifest.digest(appcast),
            )

        complete = write_candidate_gate_map(
            self.release / "complete",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=complete,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )
        self.dmg.write_bytes(b"rebuilt after acceptance")
        manifest.set_artifacts(self.manifest_path, {"dmg": manifest.digest(self.dmg)})
        recorded = manifest.load_manifest(self.manifest_path)
        self.assertFalse(
            recorded["candidate_gates"]["gates"]["downloaded_asset"]["passed"]
        )
        with self.assertRaisesRegex(
            manifest.ManifestError,
            "candidate gate downloaded_asset has been revoked or failed",
        ):
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                dependencies=self.dependencies,
                downloads=self.root / "downloads",
                appcast=appcast,
                release_url="https://example.test/releases",
                capture=self.capture,
                require_appcast=True,
                require_candidate_gates=True,
            )

    def test_rebinding_appcast_revokes_gates_until_replacement(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        initial_map = write_candidate_gate_map(
            self.release / "initial-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=initial_map,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        # Re-binding a different appcast retires the gates that covered the
        # previous one, exactly as the staged release flow can now do.
        appcast.write_text(appcast.read_text(encoding="utf-8") + "\n")
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        recorded = manifest.load_manifest(self.manifest_path)
        self.assertFalse(
            recorded["candidate_gates"]["gates"]["downloaded_asset"]["passed"]
        )

        replacement_map = write_candidate_gate_map(
            self.release / "replacement-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        replaced = manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=replacement_map,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )
        self.assertTrue(
            all(
                entry["passed"]
                for entry in replaced["candidate_gates"]["gates"].values()
            )
        )

    def test_first_appcast_binding_covers_or_revokes_existing_gates(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        gate_map = write_candidate_gate_map(
            self.release / "candidate-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=gate_map,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        self.assertTrue(
            manifest.load_manifest(self.manifest_path)["candidate_gates"]["gates"][
                "downloaded_asset"
            ]["passed"]
        )

        appcast.write_text(appcast.read_text(encoding="utf-8") + "\n")
        manifest.bind_appcast(
            self.manifest_path,
            tag="v0.5.0",
            version="0.5.0",
            appcast=appcast,
            dmg=self.dmg,
            release_url="https://example.test/releases",
        )
        self.assertFalse(
            manifest.load_manifest(self.manifest_path)["candidate_gates"]["gates"][
                "downloaded_asset"
            ]["passed"]
        )

    def test_preflight_aggregates_appcast_and_artifact_gate_errors(self):
        self.write_manifest()
        self.qualified(self.dependencies)
        appcast = self.write_appcast()
        gate_map = write_candidate_gate_map(
            self.release / "candidate-gates",
            COMMIT,
            "0.5.0",
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            manifest.digest(appcast),
        )
        manifest.bind_candidate_gates(
            self.manifest_path,
            gate_map_path=gate_map,
            source_revision=COMMIT,
            version="0.5.0",
            artifacts={"app": self.app, "dmg": self.dmg},
            appcast_digest=manifest.digest(appcast),
        )
        recorded = manifest.load_manifest(self.manifest_path)
        recorded["artifacts"].pop("dmg")
        manifest.write_manifest(self.manifest_path, recorded)
        appcast.unlink()
        self.dmg.unlink()
        with self.assertRaisesRegex(
            manifest.ManifestError,
            r"(?s)manifest has no valid dmg SHA-256.*"
            r"candidate gate has no recorded dmg artifact digest.*"
            r"cannot hash missing artifact.*appcast.xml",
        ):
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app, "dmg": self.dmg},
                dependencies=self.dependencies,
                downloads=self.root / "downloads",
                appcast=appcast,
                release_url="https://example.test/releases",
                capture=self.capture,
                require_appcast=True,
                require_candidate_gates=True,
            )

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
        self.dependencies["sparkle"]["archive_sha256"] = manifest.content_sha256(
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

    def test_replacing_one_artifact_withdraws_only_its_approval(self):
        self.write_manifest()
        qualified = {
            "app": manifest.digest(self.app),
            "dmg": manifest.digest(self.dmg),
        }
        manifest.set_artifacts(
            self.manifest_path,
            qualified,
            provenance={"dmg_source_app_sha256": qualified["app"]},
        )
        manifest.set_notarized(self.manifest_path, True, True)
        manifest.set_verification(
            self.manifest_path, scope="notarized", artifacts=qualified
        )
        self.dmg.write_bytes(b"rebuilt dmg")
        manifest.set_artifacts(
            self.manifest_path,
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
        )
        recorded = manifest.load_manifest(self.manifest_path)
        self.assertEqual(recorded["notarized"], {"app": True, "dmg": False})
        self.assertIs(recorded["verification"]["passed"], False)
        self.assertEqual(recorded["verification"]["scope"], "notarized")
        # The receipt still names the bytes it qualified, which is the fixed
        # dmg, not the replacement that withdrew the approval.
        self.assertEqual(recorded["verification"]["artifacts"], qualified)

    def test_set_verification_merges_into_the_existing_receipt(self):
        self.write_manifest()
        recorded = manifest.load_manifest(self.manifest_path)
        recorded["verification"] = {"runner": "packaging host"}
        manifest.write_manifest(self.manifest_path, recorded)
        manifest.set_verification(
            self.manifest_path, scope="notarized", artifacts={"app": "2" * 64}
        )
        receipt = manifest.load_manifest(self.manifest_path)["verification"]
        self.assertEqual(receipt["runner"], "packaging host")
        self.assertIs(receipt["passed"], True)
        self.assertEqual(receipt["artifacts"], {"app": "2" * 64})

    def test_weakening_notarization_revokes_the_qualification_receipt(self):
        self.write_manifest()
        manifest.set_notarized(self.manifest_path, True, True)
        manifest.set_verification(
            self.manifest_path, scope="notarized", artifacts={"app": "2" * 64}
        )
        manifest.set_notarized(self.manifest_path, True, False)
        receipt = manifest.load_manifest(self.manifest_path)["verification"]
        self.assertIs(receipt["passed"], False)
        self.assertEqual(receipt["artifacts"], {"app": "2" * 64})

    def test_preflight_without_a_dmg_binding_fails_instead_of_crashing(self):
        recorded = self.write_manifest()
        recorded["artifacts"] = {"app": manifest.digest(self.app)}
        recorded["provenance"] = {"dmg_source_app_sha256": manifest.digest(self.app)}
        recorded["notarized"] = {"app": True, "dmg": True}
        recorded["verification"] = {
            "scope": "notarized",
            "passed": True,
            "artifacts": {"app": manifest.digest(self.app)},
        }
        manifest.write_manifest(self.manifest_path, recorded)
        appcast = self.release / "appcast.xml"
        appcast.write_text("<rss/>")
        with self.assertRaisesRegex(
            manifest.ManifestError, "artifact 'dmg' is missing from artifacts"
        ):
            manifest.preflight_release(
                self.manifest_path,
                root=self.root,
                tag="v0.5.0",
                version="0.5.0",
                artifacts={"app": self.app},
                dependencies=self.dependencies,
                downloads=self.root / "downloads",
                appcast=appcast,
                release_url="https://example.test/releases",
                capture=self.capture,
                require_appcast=True,
            )

    def qtbase_archive(self):
        downloads = self.root / "downloads"
        downloads.mkdir(exist_ok=True)
        archive = downloads / "qtbase-everywhere-src-6.tar.xz"
        archive.write_bytes(b"qt")
        return archive

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
        (self.app / "Contents/Info.plist").write_bytes(
            plistlib.dumps({"CFBundleShortVersionString": "0.5.0"})
        )
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
                    "qtbase": manifest.content_sha256(
                        self.downloads / "qtbase-everywhere-src-6.tar.xz"
                    )
                },
            },
            "sparkle": {
                "version": "1",
                "archive_sha256": manifest.content_sha256(
                    self.downloads / "Sparkle-1.tar.xz"
                ),
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
        self.bind_candidate_gates = True
        self.bind_appcast_failure = False
        self.write_appcast_mock = Mock()
        self.gate_timestamp = GATE_TIMESTAMP

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

    def invoke_release(
        self,
        *,
        verify_failure=False,
        real_verify=False,
        bind_appcast_failure=False,
        appcast_exists=True,
        replace_candidate_gates=False,
        appcast_signature="ed" * 64,
    ):
        arguments = type("Arguments", (), {})()
        arguments.tag = "v0.5.0"
        arguments.draft = True
        arguments.notarized = False
        arguments.candidate_gate_map = self.release / "candidate-gates" / "map.json"
        arguments.replace_candidate_gates = replace_candidate_gates
        self.bind_appcast_failure = bind_appcast_failure
        if appcast_exists:
            self.write_appcast()
            if appcast_signature != "ed" * 64:
                root = ET.parse(self.appcast).getroot()
                enclosure = root.find("./channel/item/enclosure")
                assert enclosure is not None
                enclosure.set(
                    "{http://www.andymatuschak.org/xml-namespaces/sparkle}edSignature",
                    appcast_signature,
                )
                self.appcast.write_bytes(ET.tostring(root, encoding="utf-8"))

        def release_dependencies(_ghostty):
            return self.dependencies

        def bind_gates(manifest_path, **kwargs):
            if self.bind_candidate_gates:
                gate_map_path = write_candidate_gate_map(
                    self.release / "candidate-gates",
                    COMMIT,
                    "0.5.0",
                    {
                        "app": manifest.digest(self.app),
                        "dmg": manifest.digest(self.dmg),
                    },
                    kwargs["appcast_digest"],
                    timestamp=self.gate_timestamp,
                )
            else:
                gate_map_path = self.release / "candidate-gates" / "absent.json"
            try:
                return manifest.bind_candidate_gates(
                    manifest_path,
                    gate_map_path=gate_map_path,
                    source_revision=kwargs["source_revision"],
                    version=kwargs["version"],
                    artifacts=kwargs["artifacts"],
                    appcast_digest=kwargs["appcast_digest"],
                    replace=kwargs.get("replace", False),
                )
            except manifest.ManifestError as error:
                raise package.ManifestError(str(error)) from error

        def bind_release_appcast(manifest_path, **kwargs):
            if self.bind_appcast_failure:
                raise package.ManifestError("injected appcast binding failure")
            return manifest.bind_appcast(manifest_path, **kwargs)

        patches = [
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
                package, "write_appcast", return_value=self.write_appcast_mock
            ),
            patch.object(
                package,
                "update_signature",
                return_value=("ed" * 64, str(self.dmg.stat().st_size)),
            ),
            patch.object(package, "bind_appcast", side_effect=bind_release_appcast),
            patch.object(package, "bind_candidate_gates", side_effect=bind_gates),
        ]
        if not real_verify:
            patches.append(
                patch.object(
                    package,
                    "command_verify",
                    side_effect=lambda value: self.qualify(
                        {
                            "notarized": value.notarized,
                            "fail": verify_failure,
                        }
                    ),
                )
            )
        with contextlib.ExitStack() as stack:
            for patcher in patches:
                stack.enter_context(patcher)
            package.command_release(arguments)

    def test_failed_qualification_refuses_publication(self):
        with self.assertRaisesRegex(package.PackageError, "qualification failure"):
            self.invoke_release(verify_failure=True)
        self.assertFalse(self.published)
        self.assertEqual([command[0] for command in self.command_log], ["xcrun"])
        self.write_appcast_mock.assert_not_called()
        self.assertTrue(self.appcast.exists())
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

    def test_missing_candidate_gate_map_stops_before_publication(self):
        self.bind_candidate_gates = False
        with self.assertRaisesRegex(
            package.PackageError, "candidate gate map is missing"
        ):
            self.invoke_release()
        self.assertFalse(self.published)
        self.assertTrue(self.appcast.exists())
        unchanged = manifest.load_manifest(self.manifest_path)
        self.assertIsNone(unchanged.get("candidate_gates"))
        self.assertIsNone(unchanged.get("release"))

    def test_staging_io_failure_reports_a_package_error(self):
        with (
            self.assertRaisesRegex(
                package.PackageError, "cannot stage the release manifest"
            ),
            patch.object(
                package.shutil,
                "copy2",
                side_effect=PermissionError("staging directory is read-only"),
            ),
        ):
            self.invoke_release()
        self.assertFalse(self.published)
        unchanged = manifest.load_manifest(self.manifest_path)
        self.assertIsNone(unchanged.get("release"))
        self.assertIsNone(unchanged.get("candidate_gates"))

    def test_published_manifest_binding_flushes_its_directory(self):
        with patch.object(package, "sync_directory") as sync:
            self.invoke_release()
        self.assertTrue(self.published)
        sync.assert_called_once_with(self.release)

    def test_replace_flag_retains_the_previous_approved_binding(self):
        self.invoke_release()
        self.gate_timestamp = "2026-10-05T00:00:00Z"

        self.invoke_release(replace_candidate_gates=True)

        self.assertTrue(self.published)
        recorded = manifest.load_manifest(self.manifest_path)
        self.assertTrue(
            all(
                entry["passed"]
                for entry in recorded["candidate_gates"]["gates"].values()
            )
        )
        superseded = recorded["superseded_candidate_gates"]
        self.assertEqual(len(superseded), 1)
        self.assertTrue(
            all(entry["passed"] for entry in superseded[0]["gates"].values())
        )
        self.assertEqual(
            superseded[0]["gates"]["downloaded_asset"]["recorded_at"],
            GATE_TIMESTAMP,
        )

    def test_release_binds_the_preexisting_appcast_without_minting_it(self):
        self.write_appcast()
        appcast_bytes = self.appcast.read_bytes()

        self.invoke_release()

        self.assertTrue(self.published)
        self.assertEqual(self.appcast.read_bytes(), appcast_bytes)
        self.write_appcast_mock.assert_not_called()
        binding = manifest.load_manifest(self.manifest_path)["release"]["appcast"]
        self.assertEqual(binding["sha256"], manifest.digest(self.appcast))

    def test_release_requires_a_preexisting_appcast(self):
        with self.assertRaisesRegex(
            package.PackageError,
            "write the appcast and record candidate gates",
        ):
            self.invoke_release(appcast_exists=False)
        self.assertFalse(self.published)
        self.assertFalse(self.appcast.exists())
        self.write_appcast_mock.assert_not_called()

    def test_release_requires_the_appcast_signature_to_authenticate_the_dmg(self):
        with self.assertRaisesRegex(
            package.PackageError,
            "appcast EdDSA signature does not authenticate this DMG",
        ):
            self.invoke_release(appcast_signature="aa" * 64)
        self.assertFalse(self.published)
        unchanged = manifest.load_manifest(self.manifest_path)
        self.assertIsNone(unchanged.get("release"))
        self.assertIsNone(unchanged.get("candidate_gates"))

    def test_failed_appcast_binding_does_not_expose_staged_gates(self):
        self.bind_candidate_gates = True
        with self.assertRaisesRegex(
            package.PackageError, "injected appcast binding failure"
        ):
            self.invoke_release(bind_appcast_failure=True)
        self.assertFalse(self.published)
        unchanged = manifest.load_manifest(self.manifest_path)
        self.assertIsNone(unchanged.get("release"))
        self.assertIsNone(unchanged.get("candidate_gates"))

    def test_changed_bytes_before_appcast_binding_refuses_publication(self):
        def mutate_after_qualification():
            self.dmg.write_bytes(b"stale replacement")

        self.qualification_observer = mutate_after_qualification
        with self.assertRaisesRegex(package.PackageError, "artifact dmg mismatch"):
            self.invoke_release()
        self.assertFalse(self.published)
        self.write_appcast_mock.assert_not_called()

    def test_malformed_manifest_stops_before_staple_validation_writes(self):
        self.manifest_path.write_text("{}")
        with self.assertRaisesRegex(package.PackageError, "schema"):
            self.invoke_release(real_verify=True)
        # The unreadable manifest is the only record of this build, so it must
        # survive the rejection exactly as it was.
        self.assertEqual(self.manifest_path.read_text(), "{}")
        self.assertEqual(
            self.command_log, [["xcrun", "stapler", "validate", str(self.dmg)]]
        )
        self.assertFalse(self.published)


class DependencySnapshotTests(unittest.TestCase):
    """The dependency recorder is the only check that the release inputs exist."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "CMakeLists.txt").write_text(
            "project(lapis VERSION 0.5.0 LANGUAGES CXX)\n"
        )
        self.ghostty = self.root / "ghostty"
        self.sources = self.root / "tools/terminal_probe/ghostty/sources.json"
        self.receipt = self.root / "reports/receipt.json"
        self.header = self.ghostty / "include/ghostty/vt.h"
        self.library = self.ghostty / "lib/libghostty-vt.a"
        self.moltenvk = self.root / "moltenvk/libMoltenVK.dylib"
        self.notices = {"Qt": self.root / "notices/qt.txt"}
        for path in (
            self.sources,
            self.receipt,
            self.header,
            self.library,
            self.moltenvk,
            *self.notices.values(),
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(path.name.encode())

    def snapshot(self):
        return manifest.dependency_snapshot(
            self.root,
            qt_version="6.11.2",
            qt_modules={"qtbase": "0" * 64},
            sparkle_version="2.10.0",
            sparkle_sha256="1" * 64,
            notices=self.notices,
            ghostty_prefix=self.ghostty,
            moltenvk_library=self.moltenvk,
        )

    def test_every_dependency_input_is_hashed(self):
        recorded = self.snapshot()
        self.assertEqual(
            recorded["qt"], {"version": "6.11.2", "source_sha256": {"qtbase": "0" * 64}}
        )
        self.assertEqual(
            recorded["sparkle"], {"version": "2.10.0", "archive_sha256": "1" * 64}
        )
        self.assertEqual(
            recorded["ghostty"],
            {
                "sources_sha256": manifest.digest(self.sources),
                "receipt_sha256": manifest.digest(self.receipt),
                "header_sha256": manifest.digest(self.header),
                "library_sha256": manifest.digest(self.library),
            },
        )
        self.assertEqual(
            recorded["moltenvk"], {"library_sha256": manifest.digest(self.moltenvk)}
        )
        self.assertEqual(
            recorded["notices"], {"Qt": manifest.digest(self.notices["Qt"])}
        )

    def test_missing_inputs_are_all_named(self):
        self.sources.unlink()
        self.moltenvk.unlink()
        with self.assertRaisesRegex(
            manifest.ManifestError, "missing dependency or notice input"
        ) as caught:
            self.snapshot()
        self.assertIn("sources.json", str(caught.exception))
        self.assertIn("libMoltenVK.dylib", str(caught.exception))

    def test_a_missing_notice_is_named(self):
        self.notices["Qt"].unlink()
        with self.assertRaisesRegex(
            manifest.ManifestError, "missing dependency or notice input"
        ) as caught:
            self.snapshot()
        self.assertIn("qt.txt", str(caught.exception))


class VerifyCommandTests(unittest.TestCase):
    """Verify's manifest gates, driven through the real command_verify."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "CMakeLists.txt").write_text(
            "project(lapis VERSION 0.5.0 LANGUAGES CXX)\n"
        )
        self.release = self.root / "build/release"
        self.app = self.release / "stage/lapis.app"
        self.dmg = self.release / "lapis-macos-arm64.dmg"
        (self.app / "Contents").mkdir(parents=True)
        (self.app / "Contents/lapis").write_bytes(b"final app")
        self.dmg.write_bytes(b"final dmg")
        self.manifest_path = self.release / "package-manifest.json"
        self.write_version("0.5.0")
        manifest.write_manifest(
            self.manifest_path,
            manifest.new_manifest(
                {
                    "commit": COMMIT,
                    "tree": TREE,
                    "clean": True,
                    "status": [],
                    "version": "0.5.0",
                },
                self.dependencies(),
            ),
        )
        self.arguments = type("Arguments", (), {})()
        self.arguments.notarized = True
        self.commands = []
        self.checks = []

    def dependencies(self):
        return {"qt": {"version": "6", "source_sha256": {}}, "sparkle": {}}

    def write_version(self, version):
        (self.app / "Contents/Info.plist").write_bytes(
            plistlib.dumps({"CFBundleShortVersionString": version})
        )

    def bind(self, *, notarized=True):
        manifest.set_artifacts(
            self.manifest_path,
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
            provenance={"dmg_source_app_sha256": manifest.digest(self.app)},
        )
        manifest.set_notarized(self.manifest_path, notarized, notarized)

    def invoke(self):
        def capture(command):
            command = [str(part) for part in command]
            arguments = command[3:]
            if arguments == ["rev-parse", "HEAD"]:
                return COMMIT + "\n"
            if arguments == ["rev-parse", "HEAD^{tree}"]:
                return TREE + "\n"
            if arguments == ["status", "--porcelain=v1"]:
                return ""
            raise AssertionError(command)

        def run(command, **_kwargs):
            self.commands.append([str(part) for part in command])
            return None

        def probe(name):
            def check(_problems):
                self.checks.append(name)

            return check

        external = (
            "check_binaries",
            "check_identifying_strings",
            "check_moltenvk",
            "check_headless_host",
            "check_launchd_start",
            "check_updates",
        )
        patches = [
            patch.object(package, "ROOT", self.root),
            patch.object(package, "APP", self.app),
            patch.object(package, "DMG", self.dmg),
            patch.object(package, "PACKAGE_MANIFEST", self.manifest_path),
            patch.object(package, "capture", side_effect=capture),
            patch.object(package, "run", side_effect=run),
        ]
        patches.extend(patch.object(package, name, probe(name)) for name in external)
        with contextlib.ExitStack() as stack:
            for patcher in patches:
                stack.enter_context(patcher)
            package.command_verify(self.arguments)

    def test_a_notarized_bundle_records_its_qualification(self):
        self.bind()
        self.invoke()
        receipt = manifest.load_manifest(self.manifest_path)["verification"]
        self.assertEqual(receipt["scope"], "notarized")
        self.assertIs(receipt["passed"], True)
        self.assertEqual(
            receipt["artifacts"],
            {"app": manifest.digest(self.app), "dmg": manifest.digest(self.dmg)},
        )
        self.assertIn(["xcrun", "stapler", "validate", str(self.dmg)], self.commands)
        self.assertIn("check_binaries", self.checks)

    def test_drifted_app_bytes_fail_verification_and_keep_the_manifest(self):
        self.bind()
        recorded = manifest.load_manifest(self.manifest_path)["artifacts"]
        (self.app / "Contents/lapis").write_bytes(b"tampered")
        with self.assertRaisesRegex(
            package.PackageError, "does not match the manifest"
        ):
            self.invoke()
        surviving = manifest.load_manifest(self.manifest_path)
        self.assertEqual(surviving["artifacts"], recorded)
        self.assertNotIn("verification", surviving)

    def test_verifying_before_notarizing_names_the_step_and_keeps_the_manifest(self):
        self.bind(notarized=False)
        with self.assertRaisesRegex(
            package.PackageError, "run package_macos.py notarize before"
        ):
            self.invoke()
        surviving = manifest.load_manifest(self.manifest_path)
        self.assertEqual(surviving["artifacts"]["app"], manifest.digest(self.app))
        self.assertEqual(surviving["notarized"], {"app": False, "dmg": False})

    def test_a_stale_version_is_reported_as_a_problem(self):
        self.write_version("0.4.9")
        self.bind()
        with self.assertRaisesRegex(
            package.PackageError, "does not match source version"
        ):
            self.invoke()

    def test_an_unreadable_plist_raises_without_the_build_path(self):
        (self.app / "Contents/Info.plist").unlink()
        self.bind()
        with self.assertRaisesRegex(
            package.PackageError, "cannot read the staged app's Contents/Info.plist"
        ) as caught:
            self.invoke()
        self.assertNotIn(str(self.temporary.name), str(caught.exception))
