"""Durable source and artifact bindings for macOS release packaging.

The manifest deliberately separates facts that must never change during a
release build (the Git source snapshot) from facts that legitimately change
when codesign, notarytool, and stapler rewrite artifacts.  Release preflight
compares every mutable byte again immediately before publication.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import tempfile
import subprocess
import xml.etree.ElementTree as ET
from collections.abc import Callable
from datetime import datetime, timedelta
from pathlib import Path

MANIFEST_SCHEMA_VERSION = 1
MANIFEST_KIND = "lapis-macos-package"
CANDIDATE_GATE_MAP_SCHEMA_VERSION = 1
CANDIDATE_GATE_MAP_KIND = "lapis-release-candidate-gates"
CANDIDATE_GATE_RECEIPT_KIND = "lapis-candidate-gate-receipt"
CANDIDATE_GATE_NAMES = (
    "downloaded_asset",
    "notarized_staged_verification",
    "fresh_user_finder_launch",
    "installed_sparkle_update_with_live_sessions",
    "update_failure_recovery",
    "registry_history_migration_rollback",
)
CANDIDATE_GATES_REQUIRING_APPCAST = frozenset(
    set(CANDIDATE_GATE_NAMES) - {"notarized_staged_verification"}
)
CANDIDATE_GATE_ARTIFACTS = ("app", "dmg")
SPARKLE_XMLNS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
_SHA256 = re.compile(r"[0-9a-f]{64}")


class ManifestError(RuntimeError):
    """The manifest is absent, malformed, or does not match release inputs."""


ManifestSink = Callable[[list[str | Path]], str]


def _utc_timestamp(value: object) -> datetime | None:
    """Return an ISO timestamp only when it explicitly resolves to UTC."""
    if not isinstance(value, str) or not value:
        return None
    normalized = value[:-1] + "+00:00" if value.endswith(("Z", "z")) else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        return None
    return (
        parsed
        if parsed.tzinfo is not None and parsed.utcoffset() == timedelta(0)
        else None
    )


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
    except UnicodeDecodeError as error:
        raise ManifestError(f"CMakeLists.txt is not UTF-8: {error}") from error
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
    try:
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
    except OSError as error:
        raise ManifestError(f"cannot read artifact: {path}: {error}") from error


def content_sha256(path: Path) -> str:
    """Return the standard SHA-256 of a file's bytes.

    External archive pins are byte hashes.  Keep them separate from
    ``digest`` because release artifacts also bind names, modes, and bundle
    structure.
    """
    path = Path(path)
    try:
        if not path.is_file():
            raise ManifestError(f"cannot hash missing artifact: {path}")
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError as error:
        raise ManifestError(f"cannot read artifact: {path}: {error}") from error


def _hash_tree(root: Path, relative: str, hasher) -> None:
    with os.scandir(root) as scan:
        entries = sorted(scan, key=lambda entry: entry.name)
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
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
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


def _sync(descriptor: int) -> None:
    """Flush one descriptor to stable storage.

    APFS treats a plain ``fsync`` as a request to order writes, not to reach
    the platter, so a manifest that must survive a power loss needs the
    Darwin-specific full flush as well.
    """
    os.fsync(descriptor)
    if hasattr(fcntl, "F_FULLFSYNC"):
        fcntl.fcntl(descriptor, fcntl.F_FULLFSYNC)


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
            _sync(stream.fileno())
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            _sync(directory_fd)
        finally:
            os.close(directory_fd)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def _revoke_verification(manifest: dict[str, object]) -> None:
    """Stop a recorded qualification receipt from authorizing anything.

    The receipt is kept, including the artifacts it covered, because it is
    evidence of what was qualified; only its verdict is withdrawn.
    """
    verification = manifest.get("verification")
    if isinstance(verification, dict):
        verification["passed"] = False


def _revoke_candidate_gates(manifest: dict[str, object]) -> None:
    """Withdraw candidate approval while preserving the receipts as evidence."""
    gates = manifest.get("candidate_gates")
    if not isinstance(gates, dict):
        return
    entries = gates.get("gates")
    if isinstance(entries, dict):
        for entry in entries.values():
            if isinstance(entry, dict):
                entry["passed"] = False


def set_artifacts(
    path: Path,
    artifacts: dict[str, object],
    *,
    provenance: dict[str, object] | None = None,
) -> dict[str, object]:
    manifest = load_manifest(path)
    recorded = manifest.get("artifacts")
    if recorded is None:
        recorded = {}
        manifest["artifacts"] = recorded
    elif not isinstance(recorded, dict):
        raise ManifestError("manifest artifact bindings are malformed")
    sources = manifest.get("provenance")
    if sources is None:
        sources = {}
        manifest["provenance"] = sources
    elif not isinstance(sources, dict):
        raise ManifestError("manifest provenance bindings are malformed")
    replaced = [
        name for name, value in artifacts.items() if recorded.get(name) != value
    ]
    recorded.update(artifacts)
    if provenance is not None:
        sources.update(provenance)
    if replaced:
        # Every approval covered the previous bytes, so it cannot outlive them.
        states = manifest.get("notarized")
        if isinstance(states, dict):
            for name in replaced:
                if states.get(name) is True:
                    states[name] = False
        _revoke_verification(manifest)
        _revoke_candidate_gates(manifest)
    write_manifest(path, manifest)
    return manifest


def set_notarized(path: Path, app: bool, dmg: bool) -> dict[str, object]:
    manifest = load_manifest(path)
    states = manifest.get("notarized")
    if not isinstance(states, dict):
        states = {}
    weakened = (states.get("app") is True and app is not True) or (
        states.get("dmg") is True and dmg is not True
    )
    states.update({"app": app, "dmg": dmg})
    manifest["notarized"] = states
    if weakened:
        _revoke_verification(manifest)
        _revoke_candidate_gates(manifest)
    write_manifest(path, manifest)
    return manifest


def set_verification(
    path: Path,
    *,
    scope: str,
    artifacts: dict[str, object],
) -> dict[str, object]:
    manifest = load_manifest(path)
    verification = manifest.get("verification")
    if not isinstance(verification, dict):
        verification = {}
    verification.update({"scope": scope, "passed": True, "artifacts": dict(artifacts)})
    manifest["verification"] = verification
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
        appcast_digest = digest(appcast)
        dmg_digest = digest(dmg)
        dmg_length = dmg.stat().st_size
    except ManifestError as error:
        raise ManifestError(f"cannot bind release artifacts: {error}") from error
    except OSError as error:
        raise ManifestError(f"cannot bind release artifacts: {error}") from error
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
        "sha256": appcast_digest,
        "dmg_sha256": dmg_digest,
        "dmg_length": dmg_length,
        "ed_signature": enclosure.attrib.get(f"{{{SPARKLE_XMLNS}}}edSignature", ""),
        "url": enclosure.attrib.get("url", ""),
        "expected_url": expected_url,
    }
    manifest = load_manifest(path)
    previous = manifest.get("release")
    if isinstance(previous, dict) and previous.get("appcast") != binding:
        _revoke_candidate_gates(manifest)
    manifest["release"] = {"appcast": binding}
    write_manifest(path, manifest)
    return manifest


def _equal(name: str, expected: object, actual: object, errors: list[str]) -> None:
    if expected != actual:
        errors.append(f"{name} mismatch: manifest={expected!r}, current={actual!r}")


def _load_candidate_json(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ManifestError(f"{label} is missing: {path}") from error
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"{label} is malformed: {error}") from error
    if not isinstance(value, dict):
        raise ManifestError(f"{label} must be a JSON object")
    return value


def _validate_candidate_identity(
    value: object,
    *,
    expected_source: str,
    expected_version: str,
    expected_artifacts: dict[str, str],
    expected_appcast: str | None,
    errors: list[str],
) -> None:
    if not isinstance(value, dict):
        errors.append("candidate gate identity is malformed")
        return
    _equal(
        "candidate source revision",
        expected_source,
        value.get("source_revision"),
        errors,
    )
    _equal("candidate version", expected_version, value.get("version"), errors)
    artifacts = value.get("artifacts")
    if not isinstance(artifacts, dict) or set(artifacts) != {*CANDIDATE_GATE_ARTIFACTS}:
        errors.append("candidate gate artifacts must bind exactly app and DMG SHA-256s")
    else:
        _equal("candidate artifacts", expected_artifacts, artifacts, errors)
    _equal(
        "candidate appcast digest",
        expected_appcast,
        value.get("appcast_digest"),
        errors,
    )


def _validate_candidate_fields(value: object, errors: list[str]) -> None:
    if not isinstance(value, dict):
        errors.append("candidate gate receipt is malformed")
        return
    receipt_digest = value.get("receipt_sha256")
    if not isinstance(receipt_digest, str) or _SHA256.fullmatch(receipt_digest) is None:
        errors.append("candidate gate has no valid receipt SHA-256")
    if _utc_timestamp(value.get("recorded_at")) is None:
        errors.append("candidate gate has no UTC timestamp")
    command = value.get("command")
    if not isinstance(command, str) or not command.strip():
        errors.append("candidate gate has no command")
    exit_status = value.get("exit_status")
    if (
        isinstance(exit_status, bool)
        or not isinstance(exit_status, int)
        or exit_status != 0
    ):
        errors.append("candidate gate did not exit zero")


def candidate_gate_binding(
    gate_map_path: Path,
    *,
    source_revision: str,
    version: str,
    artifacts: dict[str, Path],
    appcast_digest: str | None,
) -> dict[str, object]:
    """Validate an external gate map and return its immutable manifest binding."""
    gate_map = _load_candidate_json(gate_map_path, "candidate gate map")
    if gate_map.get("schema_version") != CANDIDATE_GATE_MAP_SCHEMA_VERSION:
        raise ManifestError(
            f"unsupported candidate gate map schema: {gate_map.get('schema_version')!r}"
        )
    if gate_map.get("map_kind") != CANDIDATE_GATE_MAP_KIND:
        raise ManifestError(
            f"wrong candidate gate map kind: {gate_map.get('map_kind')!r}"
        )
    gates = gate_map.get("gates")
    if not isinstance(gates, dict) or set(gates) != set(CANDIDATE_GATE_NAMES):
        raise ManifestError(
            "candidate gate map must bind exactly: " + ", ".join(CANDIDATE_GATE_NAMES)
        )

    expected_artifacts = {name: digest(path) for name, path in artifacts.items()}
    if set(expected_artifacts) != set(CANDIDATE_GATE_ARTIFACTS):
        raise ManifestError("candidate gates require app and DMG artifacts")
    binding: dict[str, object] = {
        "schema_version": CANDIDATE_GATE_MAP_SCHEMA_VERSION,
        "map_kind": CANDIDATE_GATE_MAP_KIND,
        "gate_map_sha256": content_sha256(gate_map_path),
        "gates": {},
    }
    bound_gates = binding["gates"]
    assert isinstance(bound_gates, dict)
    for name in CANDIDATE_GATE_NAMES:
        entry = gates[name]
        errors: list[str] = []
        expected_appcast = (
            appcast_digest if name in CANDIDATE_GATES_REQUIRING_APPCAST else None
        )
        _validate_candidate_identity(
            entry,
            expected_source=source_revision,
            expected_version=version,
            expected_artifacts=expected_artifacts,
            expected_appcast=expected_appcast,
            errors=errors,
        )
        _validate_candidate_fields(entry, errors)
        if not isinstance(entry, dict):
            raise ManifestError(
                f"candidate gate {name} is malformed:\n  " + "\n  ".join(errors)
            )
        receipt_name = entry.get("receipt")
        if (
            not isinstance(receipt_name, str)
            or not receipt_name
            or Path(receipt_name).is_absolute()
        ):
            errors.append("candidate gate has no receipt file")
        else:
            receipt_path = gate_map_path.parent / receipt_name
            try:
                receipt_path.resolve().relative_to(gate_map_path.parent.resolve())
            except (OSError, ValueError):
                errors.append("candidate receipt escapes the gate-map directory")
            else:
                receipt = _load_candidate_json(
                    receipt_path, f"candidate gate {name} receipt"
                )
                if receipt.get("schema_version") != 1:
                    errors.append("candidate receipt has an unsupported schema")
                if receipt.get("receipt_kind") != CANDIDATE_GATE_RECEIPT_KIND:
                    errors.append("candidate receipt has the wrong kind")
                if receipt.get("scope") != name:
                    errors.append("candidate receipt is not scoped to its gate")
                if receipt.get("passed") is not True:
                    errors.append("candidate receipt is not passing")
                _equal("candidate receipt gate", name, receipt.get("gate"), errors)
                _equal(
                    "candidate receipt source",
                    entry.get("source_revision"),
                    receipt.get("source_revision"),
                    errors,
                )
                _equal(
                    "candidate receipt version",
                    entry.get("version"),
                    receipt.get("version"),
                    errors,
                )
                _equal(
                    "candidate receipt artifacts",
                    entry.get("artifacts"),
                    receipt.get("artifacts"),
                    errors,
                )
                _equal(
                    "candidate receipt appcast",
                    entry.get("appcast_digest"),
                    receipt.get("appcast_digest"),
                    errors,
                )
                _equal(
                    "candidate receipt timestamp",
                    entry.get("recorded_at"),
                    receipt.get("recorded_at"),
                    errors,
                )
                _equal(
                    "candidate receipt command",
                    entry.get("command"),
                    receipt.get("command"),
                    errors,
                )
                _equal(
                    "candidate receipt exit status",
                    entry.get("exit_status"),
                    receipt.get("exit_status"),
                    errors,
                )
                receipt_sha256 = content_sha256(receipt_path)
                _equal(
                    "declared candidate receipt SHA-256",
                    entry.get("receipt_sha256"),
                    receipt_sha256,
                    errors,
                )
                bound = {key: value for key, value in entry.items() if key != "receipt"}
                bound["receipt_sha256"] = receipt_sha256
                bound["passed"] = True
                bound_gates[name] = bound
        if errors:
            raise ManifestError(
                f"candidate gate {name} binding failed:\n  " + "\n  ".join(errors)
            )
    return binding


def bind_candidate_gates(
    path: Path,
    *,
    gate_map_path: Path,
    source_revision: str,
    version: str,
    artifacts: dict[str, Path],
    appcast_digest: str | None,
    replace: bool = False,
) -> dict[str, object]:
    """Atomically record the complete, digest-bound candidate gate map.

    ``replace=True`` is the operator escape hatch for a failed candidate run.
    It is explicit, revalidates the complete replacement, and production
    defaults to refusing to overwrite an existing binding.
    """
    binding = candidate_gate_binding(
        gate_map_path,
        source_revision=source_revision,
        version=version,
        artifacts=artifacts,
        appcast_digest=appcast_digest,
    )
    manifest = load_manifest(path)
    if "candidate_gates" in manifest and not replace:
        raise ManifestError("candidate gates are already bound to this manifest")
    manifest["candidate_gates"] = binding
    write_manifest(path, manifest)
    return manifest


def _validate_candidate_gates(
    manifest: dict[str, object],
    *,
    version: str,
    recorded_artifacts: dict[str, object],
    appcast: Path | None,
    require_gates: bool,
    errors: list[str],
) -> None:
    gates = manifest.get("candidate_gates")
    if gates is None:
        if require_gates:
            errors.append("candidate gate map is missing")
        return
    if not isinstance(gates, dict):
        errors.append("candidate gate map is malformed")
        return
    if gates.get("schema_version") != CANDIDATE_GATE_MAP_SCHEMA_VERSION:
        errors.append("candidate gate map has an unsupported schema")
    if gates.get("map_kind") != CANDIDATE_GATE_MAP_KIND:
        errors.append("candidate gate map has the wrong kind")
    gate_digest = gates.get("gate_map_sha256")
    if not isinstance(gate_digest, str) or _SHA256.fullmatch(gate_digest) is None:
        errors.append("candidate gate map has no valid SHA-256")
    source = manifest.get("source")
    source_revision = source.get("commit") if isinstance(source, dict) else None
    entries = gates.get("gates")
    if not isinstance(entries, dict) or set(entries) != set(CANDIDATE_GATE_NAMES):
        errors.append(
            "candidate gate map must bind exactly: " + ", ".join(CANDIDATE_GATE_NAMES)
        )
        return
    expected_artifacts = {
        name: recorded_artifacts.get(name) for name in CANDIDATE_GATE_ARTIFACTS
    }
    appcast_digest = digest(appcast) if appcast is not None else None
    for name in CANDIDATE_GATE_NAMES:
        entry = entries.get(name)
        if not isinstance(entry, dict):
            errors.append(f"candidate gate {name} is malformed")
            continue
        if entry.get("passed") is not True:
            errors.append(f"candidate gate {name} has been revoked or failed")
        expected_appcast = (
            appcast_digest if name in CANDIDATE_GATES_REQUIRING_APPCAST else None
        )
        _validate_candidate_identity(
            entry,
            expected_source=source_revision,
            expected_version=version,
            expected_artifacts=expected_artifacts,
            expected_appcast=expected_appcast,
            errors=errors,
        )
        _validate_candidate_fields(entry, errors)


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
    commit = recorded.get("commit")
    if (
        not isinstance(commit, str)
        or len(commit) != 40
        or any(character not in "0123456789abcdef" for character in commit)
    ):
        errors.append(f"manifest source has invalid commit: {commit!r}")
        return
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
    require_candidate_gates: bool = False,
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

    dmg_path = artifacts.get("dmg")
    if dmg_path is None:
        errors.append("artifact 'dmg' is missing from artifacts")

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

    if appcast is not None and dmg_path is not None:
        try:
            validate_appcast(
                manifest,
                appcast=appcast,
                tag=tag,
                version=version,
                dmg=digest(dmg_path),
                release_url=release_url,
            )
        except ManifestError as error:
            errors.append(str(error))
    elif require_appcast and dmg_path is not None:
        errors.append("release appcast is not bound")

    _validate_candidate_gates(
        manifest,
        version=version,
        recorded_artifacts=recorded_artifacts,
        appcast=appcast,
        require_gates=require_candidate_gates,
        errors=errors,
    )

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
        if not isinstance(expected, str) or _SHA256.fullmatch(expected) is None:
            errors.append(f"Qt pin for {module} is not a SHA-256")
            continue
        archive = downloads / f"{module}-everywhere-src-{qt.get('version')}.tar.xz"
        try:
            if content_sha256(archive) != expected:
                errors.append(f"{archive.name} differs from its pin")
        except ManifestError as error:
            errors.append(str(error))
    sparkle_archive = downloads / f"Sparkle-{sparkle.get('version')}.tar.xz"
    try:
        expected = sparkle.get("archive_sha256")
        if not isinstance(expected, str) or _SHA256.fullmatch(expected) is None:
            errors.append("Sparkle pin is not a SHA-256")
        elif content_sha256(sparkle_archive) != expected:
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
        if enclosure is None:
            raise ManifestError("appcast item has no enclosure")
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
