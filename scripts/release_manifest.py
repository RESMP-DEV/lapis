"""Durable source and artifact bindings for macOS release packaging.

The manifest deliberately separates facts that must never change during a
release build (the Git source snapshot) from facts that legitimately change
when codesign, notarytool, and stapler rewrite artifacts.  Release preflight
compares every mutable byte again immediately before publication.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import subprocess
import xml.etree.ElementTree as ET
from collections.abc import Callable
from pathlib import Path

MANIFEST_SCHEMA_VERSION = 1
MANIFEST_KIND = "lapis-macos-package"
SPARKLE_XMLNS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
_SHA256 = re.compile(r"[0-9a-f]{64}")


class ManifestError(RuntimeError):
    """The manifest is absent, malformed, or does not match release inputs."""


ManifestSink = Callable[[list[str | Path]], str]


def _git(root: Path, arguments: list[str], capture: ManifestSink) -> str:
    try:
        return capture(["git", "-C", str(root), *arguments]).strip()
    except subprocess.CalledProcessError as error:
        raise ManifestError(f"cannot read Git source state: {error}") from error


def _git_raw(root: Path, arguments: list[str], capture: ManifestSink) -> str:
    try:
        return capture(["git", "-C", str(root), *arguments]).rstrip("\r\n")
    except subprocess.CalledProcessError as error:
        raise ManifestError(f"cannot read Git source state: {error}") from error


def project_version(root: Path) -> str:
    """Read the version CMake injects into the app bundle."""
    try:
        text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    except OSError as error:
        raise ManifestError(f"cannot read CMakeLists.txt: {error}") from error
    found = re.search(
        r"^project\s*\(\s*lapis\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\b",
        text,
        re.MULTILINE,
    )
    if found is None:
        raise ManifestError("CMakeLists.txt has no complete lapis project version")
    return found.group(1)


def source_snapshot(root: Path, capture: ManifestSink) -> dict[str, object]:
    """Capture the exact worktree source that will be compiled.

    A clean commit is required for publication, but dirty snapshots are still
    recorded during local builds so a later release cannot silently mistake an
    uncommitted artifact for that commit.
    """
    commit = _git(root, ["rev-parse", "HEAD"], capture)
    tree = _git(root, ["rev-parse", "HEAD^{tree}"], capture)
    # Do not strip the status columns: a leading space is a meaningful state.
    status_output = _git_raw(root, ["status", "--porcelain=v1"], capture)
    status = [line for line in status_output.splitlines() if line]
    if len(commit) != 40 or any(
        character not in "0123456789abcdef" for character in commit
    ):
        raise ManifestError(f"Git returned an invalid commit: {commit!r}")
    return {
        "commit": commit,
        "tree": tree,
        "clean": not status,
        "status": status,
        "version": project_version(root),
    }


def dependency_snapshot(
    root: Path,
    *,
    qt_version: str,
    qt_modules: dict[str, str],
    sparkle_version: str,
    sparkle_sha256: str,
    notices: dict[str, Path],
    ghostty_prefix: Path,
    moltenvk_library: Path,
) -> dict[str, object]:
    """Record source pins, exact dependency inputs, and notice texts."""
    missing = [str(path) for path in notices.values() if not path.is_file()]
    ghostty_receipt = ghostty_prefix.parent / "reports" / "receipt.json"
    ghostty_header = ghostty_prefix / "include" / "ghostty" / "vt.h"
    ghostty_library = ghostty_prefix / "lib" / "libghostty-vt.a"
    moltenvk_library = Path(moltenvk_library)
    missing.extend(
        str(path)
        for path in (
            root / "tools/terminal_probe/ghostty/sources.json",
            ghostty_receipt,
            ghostty_header,
            ghostty_library,
            moltenvk_library,
        )
        if not path.is_file()
    )
    if missing:
        raise ManifestError(
            "missing dependency or notice input(s): " + ", ".join(missing)
        )
    return {
        "qt": {
            "version": qt_version,
            "source_sha256": dict(qt_modules),
        },
        "sparkle": {
            "version": sparkle_version,
            "archive_sha256": sparkle_sha256,
        },
        "ghostty": {
            "sources_sha256": digest(
                root / "tools/terminal_probe/ghostty/sources.json"
            ),
            "receipt_sha256": digest(ghostty_receipt),
            "header_sha256": digest(ghostty_header),
            "library_sha256": digest(ghostty_library),
        },
        "moltenvk": {
            "library_sha256": digest(moltenvk_library),
        },
        "notices": {name: digest(Path(path)) for name, path in sorted(notices.items())},
    }


def digest(path: Path) -> str:
    """Hash a file, or a bundle's files, links, modes, and paths."""
    path = Path(path)
    hasher = hashlib.sha256()
    if path.is_file():
        hasher.update(b"lapis-file\x00")
        _hash_file(path, hasher)
        return hasher.hexdigest()
    if not path.is_dir():
        raise ManifestError(f"cannot hash missing artifact: {path}")
    hasher.update(b"lapis-tree\x00")
    hasher.update(path.name.encode("utf-8") + b"\x00")
    _hash_tree(path, "", hasher)
    return hasher.hexdigest()


def _hash_tree(root: Path, relative: str, hasher) -> None:
    entries = sorted(os.scandir(root), key=lambda entry: entry.name)
    for entry in entries:
        entry_relative = f"{relative}/{entry.name}" if relative else entry.name
        hasher.update(entry_relative.encode("utf-8") + b"\x00")
        if entry.is_symlink():
            hasher.update(b"symlink\x00")
            hasher.update(os.readlink(entry.path).encode("utf-8") + b"\x00")
        elif entry.is_dir(follow_symlinks=False):
            hasher.update(b"directory\x00")
            _hash_tree(Path(entry.path), entry_relative, hasher)
        elif entry.is_file(follow_symlinks=False):
            hasher.update(
                f"file:{entry.stat(follow_symlinks=False).st_mode:o}\x00".encode()
            )
            _hash_file(Path(entry.path), hasher)
        else:
            raise ManifestError(f"unsupported artifact entry: {entry_relative}")


def _hash_file(path: Path, hasher) -> None:
    stat = path.stat()
    hasher.update(f"{stat.st_size}:{stat.st_mode:o}\x00".encode())
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            hasher.update(block)


def new_manifest(
    source: dict[str, object], dependencies: dict[str, object]
) -> dict[str, object]:
    return {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "manifest_kind": MANIFEST_KIND,
        "source": source,
        "version": source["version"],
        "dependencies": dependencies,
    }


def load_manifest(path: Path) -> dict[str, object]:
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ManifestError(f"package manifest is missing: {path}") from error
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"package manifest is malformed: {error}") from error
    if not isinstance(value, dict):
        raise ManifestError("package manifest must be a JSON object")
    if value.get("schema_version") != MANIFEST_SCHEMA_VERSION:
        raise ManifestError(
            f"unsupported package manifest schema: {value.get('schema_version')!r}"
        )
    if value.get("manifest_kind") != MANIFEST_KIND:
        raise ManifestError(
            f"wrong package manifest kind: {value.get('manifest_kind')!r}"
        )
    return value


def write_manifest(path: Path, manifest: dict[str, object]) -> None:
    """Atomically persist the manifest and its containing directory entry."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def set_artifacts(
    path: Path,
    artifacts: dict[str, object],
    *,
    provenance: dict[str, object] | None = None,
) -> dict[str, object]:
    manifest = load_manifest(path)
    if not isinstance(manifest.get("artifacts"), dict):
        manifest["artifacts"] = {}
    if not isinstance(manifest.get("provenance"), dict):
        manifest["provenance"] = {}
    manifest["artifacts"].update(artifacts)
    if provenance is not None:
        manifest["provenance"].update(provenance)
    write_manifest(path, manifest)
    return manifest


def set_notarized(path: Path, app: bool, dmg: bool) -> dict[str, object]:
    manifest = load_manifest(path)
    states = manifest.get("notarized", {})
    if not isinstance(states, dict):
        states = {}
    states.update({"app": app, "dmg": dmg})
    manifest["notarized"] = states
    write_manifest(path, manifest)
    return manifest


def set_verification(
    path: Path,
    *,
    scope: str,
    artifacts: dict[str, object],
) -> dict[str, object]:
    manifest = load_manifest(path)
    manifest["verification"] = {
        "scope": scope,
        "passed": True,
        "artifacts": artifacts,
    }
    write_manifest(path, manifest)
    return manifest


def bind_appcast(
    path: Path,
    *,
    tag: str,
    version: str,
    appcast: Path,
    dmg: Path,
    release_url: str,
) -> dict[str, object]:
    try:
        root = ET.parse(appcast).getroot()
    except (OSError, ET.ParseError) as error:
        raise ManifestError(f"appcast is malformed: {error}") from error
    items = root.findall("./channel/item")
    if len(items) != 1:
        raise ManifestError("appcast must contain exactly one update item")
    item = items[0]
    enclosures = item.findall("enclosure")
    if len(enclosures) != 1:
        raise ManifestError("appcast item must contain exactly one enclosure")
    enclosure = enclosures[0]
    expected_url = f"{release_url}/download/{tag}/{dmg.name}"
    binding = {
        "tag": tag,
        "version": version,
        "sha256": digest(appcast),
        "dmg_sha256": digest(dmg),
        "dmg_length": dmg.stat().st_size,
        "ed_signature": enclosure.attrib.get(f"{{{SPARKLE_XMLNS}}}edSignature", ""),
        "url": enclosure.attrib.get("url", ""),
        "expected_url": expected_url,
    }
    manifest = load_manifest(path)
    manifest["release"] = {"appcast": binding}
    write_manifest(path, manifest)
    return manifest


def _equal(name: str, expected: object, actual: object, errors: list[str]) -> None:
    if expected != actual:
        errors.append(f"{name} mismatch: manifest={expected!r}, current={actual!r}")


def _validate_release_source(
    manifest: dict[str, object],
    root: Path,
    *,
    tag: str,
    capture: ManifestSink,
    errors: list[str],
) -> None:
    recorded = manifest.get("source")
    if not isinstance(recorded, dict) or recorded.get("clean") is not True:
        errors.append("manifest source was not a clean committed worktree")
        return
    try:
        current = source_snapshot(root, capture)
    except ManifestError as error:
        errors.append(str(error))
        return
    _equal("source", recorded, current, errors)
    commit = str(recorded["commit"])
    try:
        remote_branches = capture(
            ["git", "-C", str(root), "branch", "-r", "--contains", commit]
        ).strip()
    except subprocess.CalledProcessError as error:
        errors.append(f"cannot inspect remote branches for {commit}: {error}")
    else:
        if not remote_branches:
            errors.append(f"commit {commit} is not on a remote branch")
    try:
        tagged = capture(
            [
                "git",
                "-C",
                str(root),
                "rev-parse",
                "--verify",
                "--end-of-options",
                f"{tag}^{{commit}}",
            ]
        ).strip()
        if tagged and tagged != commit:
            errors.append(f"tag {tag} points to {tagged}, expected {commit}")
    except subprocess.CalledProcessError as error:
        # A missing tag is normal before gh creates it; anything else is fatal.
        if error.returncode != 128:
            errors.append(f"cannot inspect tag {tag}: {error}")


def preflight_release(
    path: Path,
    *,
    root: Path,
    tag: str,
    version: str,
    artifacts: dict[str, Path],
    dependencies: dict[str, object],
    downloads: Path,
    appcast: Path | None,
    release_url: str,
    capture: ManifestSink,
    require_appcast: bool,
) -> dict[str, object]:
    """Return a valid publication binding, or explain every failed check."""
    manifest = load_manifest(path)
    errors: list[str] = []
    if tag != f"v{version}":
        errors.append(f"tag {tag!r} is not v{version!r}")
    _equal("version", manifest.get("version"), version, errors)
    _equal("dependencies", manifest.get("dependencies"), dependencies, errors)
    _validate_release_source(manifest, root, tag=tag, capture=capture, errors=errors)

    recorded_artifacts = manifest.get("artifacts")
    if not isinstance(recorded_artifacts, dict):
        recorded_artifacts = {}
        errors.append("manifest has no artifact bindings")
    for name, path in artifacts.items():
        expected = recorded_artifacts.get(name)
        if not isinstance(expected, str) or _SHA256.fullmatch(expected) is None:
            errors.append(f"manifest has no valid {name} SHA-256")
            continue
        try:
            _equal(f"artifact {name}", expected, digest(Path(path)), errors)
        except ManifestError as error:
            errors.append(str(error))

    provenance = manifest.get("provenance", {})
    if not isinstance(provenance, dict):
        provenance = {}
        errors.append("manifest has no provenance section")
    _equal(
        "DMG source app",
        recorded_artifacts.get("app"),
        provenance.get("dmg_source_app_sha256"),
        errors,
    )

    notarized = manifest.get("notarized", {})
    if (
        not isinstance(notarized, dict)
        or notarized.get("app") is not True
        or notarized.get("dmg") is not True
    ):
        errors.append("app and DMG are not both marked notarized/stapled")
    verification = manifest.get("verification", {})
    if not isinstance(verification, dict) or verification.get("passed") is not True:
        errors.append("package has no passing qualification receipt")
    elif verification.get("scope") != "notarized":
        errors.append("qualification was not run with --notarized")
    elif not isinstance(verification.get("artifacts"), dict):
        errors.append("qualification has no artifact bindings")
    else:
        qualified = verification["artifacts"]
        _equal(
            "qualified app", qualified.get("app"), recorded_artifacts.get("app"), errors
        )
        _equal(
            "qualified DMG", qualified.get("dmg"), recorded_artifacts.get("dmg"), errors
        )

    try:
        validate_downloads(manifest, downloads)
    except ManifestError as error:
        errors.append(str(error))

    if appcast is not None:
        try:
            validate_appcast(
                manifest,
                appcast=appcast,
                tag=tag,
                version=version,
                dmg=digest(artifacts["dmg"]),
                release_url=release_url,
            )
        except ManifestError as error:
            errors.append(str(error))
    elif require_appcast:
        errors.append("release appcast is not bound")

    if errors:
        raise ManifestError("release preflight failed:\n  " + "\n  ".join(errors))
    return manifest


def validate_downloads(manifest: dict[str, object], downloads: Path) -> None:
    dependencies = manifest.get("dependencies")
    if not isinstance(dependencies, dict):
        raise ManifestError("manifest dependency section is malformed")
    qt = dependencies.get("qt")
    sparkle = dependencies.get("sparkle")
    if not isinstance(qt, dict) or not isinstance(qt.get("source_sha256"), dict):
        raise ManifestError("manifest Qt pins are malformed")
    if not isinstance(sparkle, dict):
        raise ManifestError("manifest Sparkle pin is malformed")
    downloads = Path(downloads)
    errors: list[str] = []
    for module, expected in qt["source_sha256"].items():
        archive = downloads / f"{module}-everywhere-src-{qt.get('version')}.tar.xz"
        try:
            if digest(archive) != expected:
                errors.append(f"{archive.name} differs from its pin")
        except ManifestError as error:
            errors.append(str(error))
    sparkle_archive = downloads / f"Sparkle-{sparkle.get('version')}.tar.xz"
    try:
        if digest(sparkle_archive) != sparkle.get("archive_sha256"):
            errors.append(f"{sparkle_archive.name} differs from its pin")
    except ManifestError as error:
        errors.append(str(error))
    if errors:
        raise ManifestError("pinned downloads differ: " + "; ".join(errors))


def validate_appcast(
    manifest: dict[str, object],
    *,
    appcast: Path,
    tag: str,
    version: str,
    dmg: str,
    release_url: str,
) -> None:
    release = manifest.get("release")
    if not isinstance(release, dict) or not isinstance(release.get("appcast"), dict):
        raise ManifestError("manifest has no appcast binding")
    expected = release["appcast"]
    try:
        root = ET.parse(appcast).getroot()
        items = root.findall("./channel/item")
        if len(items) != 1 or len(items[0].findall("enclosure")) != 1:
            raise ManifestError("appcast must have one item and enclosure")
        item = items[0]
        enclosure = item.find("enclosure")
        assert enclosure is not None
        signature = enclosure.attrib.get(f"{{{SPARKLE_XMLNS}}}edSignature", "")
        length = enclosure.attrib.get("length", "")
        url = enclosure.attrib.get("url", "")
    except (OSError, ET.ParseError) as error:
        raise ManifestError(f"appcast is malformed: {error}") from error
    errors: list[str] = []
    _equal("appcast tag", expected.get("tag"), tag, errors)
    _equal("appcast version", expected.get("version"), version, errors)
    _equal("appcast bytes", expected.get("sha256"), digest(appcast), errors)
    _equal("appcast DMG binding", expected.get("dmg_sha256"), dmg, errors)
    if str(expected.get("dmg_length")) != length:
        errors.append(
            f"appcast DMG length mismatch: manifest={expected.get('dmg_length')}, XML={length}"
        )
    if expected.get("ed_signature") != signature:
        errors.append("appcast EdDSA signature differs from manifest")
    if expected.get("url") != url:
        errors.append("appcast enclosure URL differs from manifest")
    if (
        url != expected.get("expected_url")
        or url != f"{release_url}/download/{tag}/{Path(url).name}"
    ):
        errors.append(f"appcast enclosure URL is not the {tag} DMG URL")
    versions = [
        item.findtext(f"{{{SPARKLE_XMLNS}}}version", ""),
        item.findtext(f"{{{SPARKLE_XMLNS}}}shortVersionString", ""),
    ]
    if versions != [version, version]:
        errors.append(f"appcast item versions do not match {version}")
    if errors:
        raise ManifestError("appcast binding failed:\n  " + "\n  ".join(errors))
