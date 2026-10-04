"""Read-only diagnosis of an installed lapis package and its private runtime.

The command reports structural evidence only. It never launches lapis, talks a
protocol beyond a zero-byte Unix-socket connect, restores sessions, reads
terminal bytes or transcripts, prints launch commands, or changes runtime state.
"""

import argparse
import errno
import json
import os
import plistlib
import socket
import stat
import sys
import time
import uuid
from collections.abc import Iterable
from itertools import islice
from pathlib import Path
from typing import Any

if __package__:
    from . import lapis
else:
    import lapis

SCHEMA_VERSION = 1
WIRE_VERSION = 6
REGISTRY_VERSIONS = frozenset((1, 2))
KNOWN_HARNESSES = frozenset(
    ("claude", "codex", "opencode", "grok", "omp", "agy", "kimi", "gemini")
)
MAX_METADATA_BYTES = 1024 * 1024
MAX_CATEGORIES = 32
MAX_AGENTS = 128
MAX_TERMINALS = 128
MAX_ARGUMENTS = 64
MAX_TERMINAL_ARGUMENTS = 256
MAX_ARGUMENT_CHARACTERS = 4096
MAX_ARGUMENT_BYTES = 64 * 1024
MAX_PATH_UNITS = 4096
MAX_NAME_UNITS = 80
MAX_ENDPOINT_PROBES = 8
MAX_SAMPLES = 8
MAX_INVENTORY_ENTRIES = 128
MAX_FILENAME_BYTES = 255
CONNECT_TIMEOUT_SECONDS = 0.15
CONNECT_TIMEOUT_ERRNOS = frozenset((errno.ETIMEDOUT, errno.EAGAIN, errno.EWOULDBLOCK))
DESCRIPTOR_SIZE = 73
DESCRIPTOR_MAGIC = b"LAPIS-S1\n"
FILE_OPEN_FLAGS = (
    os.O_RDONLY
    | getattr(os, "O_NOFOLLOW", 0)
    | getattr(os, "O_NONBLOCK", 0)
    | getattr(os, "O_CLOEXEC", 0)
)


def issue(code: str, detail: str) -> dict[str, str]:
    """Return one stable diagnostic without embedding a private value."""
    return {"code": code, "detail": detail}


def path_state(path: Path) -> dict[str, str]:
    """Inspect one path without following its final component."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return {"state": "missing", "kind": "missing"}
    except PermissionError:
        return {"state": "inaccessible", "kind": "unknown"}
    except OSError as error:
        return {
            "state": "unknown",
            "kind": "unknown",
            "error": error.strerror or type(error).__name__,
        }
    if stat.S_ISLNK(info.st_mode):
        kind = "symlink"
    elif stat.S_ISDIR(info.st_mode):
        kind = "directory"
    elif stat.S_ISSOCK(info.st_mode):
        kind = "socket"
    elif stat.S_ISFIFO(info.st_mode):
        kind = "fifo"
    elif stat.S_ISCHR(info.st_mode):
        kind = "character-device"
    elif stat.S_ISBLK(info.st_mode):
        kind = "block-device"
    elif stat.S_ISREG(info.st_mode):
        kind = "file"
    else:
        kind = "file"
    return {"state": "present", "kind": kind}


def default_app() -> Path:
    """Use the normal installed location; --app overrides it."""
    if sys.platform == "darwin":
        return Path("/Applications/lapis.app")
    return Path.home() / ".local/bin/lapis_desktop"


def default_runtime() -> Path:
    """Match the portable app convention, including $LAPIS_HOME."""
    home = os.environ.get("LAPIS_HOME")
    if home:
        return Path(home).expanduser() / "runtime"
    return Path.home() / ".lapis" / "runtime"


def _bounded_sample_count(value: str) -> int:
    """Reject sampling beyond the documented local diagnostic bound."""
    try:
        samples = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("must be an integer from 1 to 8") from None
    if not 1 <= samples <= MAX_SAMPLES:
        raise argparse.ArgumentTypeError("must be an integer from 1 to 8")
    return samples


def _text(value: object) -> str | None:
    """Accept only bounded version-like metadata, never package prose."""
    if not isinstance(value, str) or not 1 <= len(value) <= 64:
        return None
    return (
        value
        if value[0].isalnum() and all(c.isalnum() or c in "._+-" for c in value)
        else None
    )


def _bounded_read(
    path: Path, *, limit: int, read_data: bool = True, private: bool = False
) -> tuple[str, bytes, dict[str, str] | None]:
    """Open and inspect one inode without following its final component.

    Privacy checks use the open descriptor, so a path that passes an earlier
    lstat cannot be replaced with a different inode before the bounded read.
    """
    descriptor: int | None = None
    try:
        descriptor = os.open(path, FILE_OPEN_FLAGS)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            return (
                "invalid",
                b"",
                issue("metadata-not-a-file", "Metadata is not a regular file"),
            )
        if private and (
            info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_nlink != 1
        ):
            return (
                "invalid",
                b"",
                issue(
                    "metadata-not-private",
                    "Private metadata must be a private file owned by you",
                ),
            )
        if info.st_size > limit:
            return (
                "invalid",
                b"",
                issue("metadata-too-large", "Metadata exceeds the 1 MiB limit"),
            )
        if not read_data:
            return "ok", b"", None
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            descriptor = None
            data = stream.read(limit + 1)
        if len(data) > limit:
            return (
                "invalid",
                b"",
                issue("metadata-too-large", "Metadata exceeds the 1 MiB limit"),
            )
        return "ok", data, None
    except FileNotFoundError:
        return "missing", b"", None
    except OSError as error:
        if error.errno == errno.ELOOP:
            return (
                "invalid",
                b"",
                issue("metadata-symlink", "Metadata cannot be a symlink"),
            )
        return (
            "inaccessible",
            b"",
            issue(
                "metadata-unreadable",
                error.strerror or "Metadata cannot be opened",
            ),
        )
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _valid_executable_name(value: object) -> bool:
    """Reject names that could leave the fixed bundle executable directory."""
    return (
        isinstance(value, str)
        and bool(value)
        and value not in (".", "..")
        and "/" not in value
        and "\0" not in value
        and len(value.encode("utf-8", errors="ignore")) <= MAX_FILENAME_BYTES
    )


def _executable_observation(path: Path) -> bool:
    """Validate an installed executable inode without reading its bytes."""
    try:
        descriptor = os.open(path, FILE_OPEN_FLAGS)
    except OSError:
        return False
    try:
        info = os.fstat(descriptor)
        return stat.S_ISREG(info.st_mode) and bool(info.st_mode & 0o111)
    finally:
        os.close(descriptor)


def _plist_metadata(app: Path) -> tuple[dict[str, Any], object]:
    plist_path = app / "Contents" / "Info.plist"
    metadata: dict[str, Any] = {
        "source": "Info.plist",
        "state": "missing",
        "short_version": None,
        "bundle_version": None,
        "minimum_system": None,
        "bundle_identifier_present": False,
        "executable_name_present": False,
    }
    observed, raw, problem = _bounded_read(plist_path, limit=MAX_METADATA_BYTES)
    if observed != "ok":
        assert problem is not None
        metadata["state"] = (
            "missing"
            if observed == "missing"
            else "inaccessible"
            if observed in ("inaccessible", "unreadable")
            else "invalid"
        )
        metadata["error"] = problem["code"]
        return metadata, None
    try:
        plist = plistlib.loads(raw)
    except (OSError, plistlib.InvalidFileException, ValueError) as error:
        metadata.update(state="corrupt", error=type(error).__name__)
        return metadata, None
    if not isinstance(plist, dict):
        metadata.update(state="corrupt", error="metadata is not an object")
        return metadata, None
    executable_name: object = plist.get("CFBundleExecutable")
    metadata.update(
        state="ok",
        short_version=_text(plist.get("CFBundleShortVersionString")),
        bundle_version=_text(plist.get("CFBundleVersion")),
        minimum_system=_text(plist.get("LSMinimumSystemVersion")),
        bundle_identifier_present=bool(plist.get("CFBundleIdentifier")),
        executable_name_present=_valid_executable_name(executable_name),
    )
    return metadata, executable_name


def diagnose_app(selected: Path) -> dict[str, Any]:
    """Validate package shape and read bounded, non-secret version metadata."""
    result: dict[str, Any] = {
        "selected": str(selected),
        "state": "ok",
        "kind": "bundle",
        "issues": [],
        "metadata": {"source": None, "state": "missing"},
        "protocol": {
            "source_wire_version": WIRE_VERSION,
            "runtime_wire_version": "unknown",
            "negotiation_attempted": False,
        },
    }
    issues: list[dict[str, str]] = result["issues"]
    observed = path_state(selected)
    if observed["state"] == "missing":
        result.update(state="missing", kind="missing")
        issues.append(
            issue("app-missing", "No application exists at the selected path")
        )
        return result
    if observed["state"] != "present":
        result.update(state=observed["state"], kind=observed["kind"])
        issues.append(
            issue("app-inaccessible", "The selected application cannot be inspected")
        )
        return result
    if selected.suffix == ".app":
        if observed["kind"] != "directory":
            result.update(state="invalid", kind=observed["kind"])
            issues.append(issue("app-not-a-bundle", "An .app path must be a directory"))
            return result
        metadata, executable_name = _plist_metadata(selected)
        executable_valid = _valid_executable_name(executable_name)
        executable_ok = False
        if executable_valid:
            assert isinstance(executable_name, str)
            executable_path = selected / "Contents" / "MacOS" / executable_name
            executable_ok = _executable_observation(executable_path)
        result["metadata"] = metadata
        result["executable"] = {
            "present": executable_ok,
            "executable_bit": executable_ok,
        }
        if metadata["state"] != "ok":
            issues.append(
                issue("app-metadata-invalid", "Package version metadata is unreadable")
            )
        if not executable_ok:
            issues.append(
                issue(
                    "app-executable-invalid",
                    "The bundle executable is missing or not executable",
                )
            )
        if issues:
            result["state"] = "invalid"
        return result

    if observed["kind"] != "file" or not os.access(selected, os.X_OK):
        result.update(state="invalid", kind=observed["kind"])
        issues.append(
            issue(
                "app-executable-invalid",
                "The selected application is not an executable file",
            )
        )
        return result
    result.update(kind="executable", metadata={"source": None, "state": "missing"})
    return result


def _valid_name(value: object) -> bool:
    if not isinstance(value, str) or not value.strip() or len(value) > MAX_NAME_UNITS:
        return False
    return all(character.isprintable() and character != "\0" for character in value)


def _bounded_path(value: object) -> bool:
    return (
        isinstance(value, str)
        and value.startswith("/")
        and len(value) <= MAX_PATH_UNITS
        and "\0" not in value
    )


def _private_json(path: Path) -> tuple[dict[str, Any], list[dict[str, str]], bool]:
    """Read owner-private bounded JSON. The bool is true when parsing can continue."""
    observed, raw, problem = _bounded_read(path, limit=MAX_METADATA_BYTES, private=True)
    if observed == "missing":
        return {"state": "missing"}, [], False
    if observed != "ok":
        return (
            {"state": observed},
            [problem] if problem is not None else [],
            False,
        )
    try:
        data = json.loads(raw.decode("utf-8"))
    except UnicodeError:
        return (
            {"state": "corrupt"},
            [issue("metadata-corrupt", "Metadata is not UTF-8")],
            False,
        )
    except json.JSONDecodeError:
        return (
            {"state": "corrupt"},
            [issue("metadata-corrupt", "Metadata is not valid JSON")],
            False,
        )
    if not isinstance(data, dict):
        return (
            {"state": "corrupt"},
            [issue("metadata-corrupt", "Metadata root is not an object")],
            False,
        )
    return {"state": "ok", "data": data}, [], True


def _valid_arguments(value: object, limit: int) -> bool:
    """Match the launcher's per-count and aggregate UTF-8 byte bounds."""
    if not isinstance(value, list) or len(value) > limit:
        return False
    total_bytes = 0
    for argument in value:
        if (
            not isinstance(argument, str)
            or len(argument) > MAX_ARGUMENT_CHARACTERS
            or "\0" in argument
        ):
            return False
        total_bytes += len(argument.encode("utf-8"))
    return total_bytes <= MAX_ARGUMENT_BYTES


def _agent_report(
    data: dict[str, Any], runtime: Path
) -> tuple[dict[str, Any], list[dict[str, str]], list[str]]:
    issues: list[dict[str, str]] = []
    observations: dict[str, Any] = {
        "listed": 0,
        "categories_listed": 0,
        "valid_private": 0,
        "foreign_endpoint": 0,
        "unknown_adapter": 0,
        "invalid_records": 0,
    }
    version = data.get("version")
    if type(version) is not int or version not in REGISTRY_VERSIONS:
        issues.append(
            issue(
                "registry-version-invalid",
                "Registry version is not one supported by this source",
            )
        )
        return observations, issues, []
    agents = data.get("agents")
    if not isinstance(agents, list) or len(agents) > MAX_AGENTS:
        observations["listed"] = len(agents) if isinstance(agents, list) else 0
        issues.append(
            issue(
                "registry-agents-invalid", "Agent inventory is malformed or exceeds 128"
            )
        )
        return observations, issues, []
    observations["listed"] = len(agents)
    category_ids = _category_ids(data, issues)
    observations["categories_listed"] = len(category_ids)
    identifiers: set[str] = set()
    endpoint_ids: list[str] = []
    for agent in agents:
        if not isinstance(agent, dict):
            observations["invalid_records"] += 1
            issues.append(
                issue("registry-agent-invalid", "An agent record is malformed")
            )
            continue
        identifier = agent.get("id")
        identifier_valid = isinstance(identifier, str) and bool(identifier)
        if identifier_valid:
            try:
                uuid.UUID(identifier)
            except (ValueError, AttributeError, TypeError):
                identifier_valid = False
        if not identifier_valid or identifier in identifiers:
            observations["invalid_records"] += 1
            issues.append(
                issue(
                    "registry-agent-identity",
                    "Agent identity is malformed or duplicate",
                )
            )
            continue
        identifiers.add(identifier)
        endpoint = agent.get("endpoint")
        expected_endpoint = str(runtime / f"{identifier}.sock")
        if endpoint != expected_endpoint:
            observations["foreign_endpoint"] += 1
            issues.append(
                issue(
                    "registry-endpoint-foreign",
                    "An agent endpoint is outside the selected runtime",
                )
            )
            continue
        endpoint_ids.append(identifier)
        harness = agent.get("harness", "codex")
        if harness not in KNOWN_HARNESSES:
            observations["unknown_adapter"] += 1
            issues.append(
                issue(
                    "registry-agent-adapter",
                    "An adapter identifier is not in this source catalog",
                )
            )
        if agent.get("category") not in category_ids:
            observations["invalid_records"] += 1
            issues.append(
                issue("registry-agent-category", "An agent names no saved category")
            )
        if not _valid_name(agent.get("title")):
            observations["invalid_records"] += 1
            issues.append(issue("registry-agent-title", "An agent title is malformed"))
        resume = agent.get("resumeThread")
        if resume:
            try:
                if not isinstance(resume, str) or not resume or not uuid.UUID(resume):
                    raise ValueError
            except (ValueError, AttributeError, TypeError):
                observations["invalid_records"] += 1
                issues.append(
                    issue("registry-agent-resume", "A resume identity is malformed")
                )
        arguments_valid = _valid_arguments(agent.get("arguments", []), MAX_ARGUMENTS)
        if not arguments_valid:
            observations["invalid_records"] += 1
            issues.append(
                issue(
                    "registry-agent-arguments",
                    "Saved arguments exceed documented bounds",
                )
            )
        if not _bounded_path(agent.get("program")):
            observations["invalid_records"] += 1
            issues.append(
                issue("registry-agent-program", "An agent program path is malformed")
            )
        if not _bounded_path(agent.get("directory")):
            observations["invalid_records"] += 1
            issues.append(
                issue("registry-agent-directory", "An agent directory is malformed")
            )
        else:
            observations["valid_private"] += 1
    return observations, issues, endpoint_ids


def _category_ids(data: dict[str, Any], issues: list[dict[str, str]]) -> set[str]:
    categories = data.get("categories")
    if (
        not isinstance(categories, list)
        or not categories
        or len(categories) > MAX_CATEGORIES
    ):
        issues.append(
            issue(
                "registry-categories-invalid",
                "Categories are missing, malformed, or exceed 32",
            )
        )
        return set()
    identifiers: set[str] = set()
    invalid = 0
    for category in categories:
        identifier = category.get("id") if isinstance(category, dict) else None
        if not _valid_name(identifier) or identifier in identifiers:
            invalid += 1
            continue
        identifiers.add(identifier)
        if (
            not _valid_name(category.get("name"))
            or len(str(category.get("selected", ""))) > MAX_NAME_UNITS
        ):
            invalid += 1
    if invalid:
        issues.append(
            issue(
                "registry-category-invalid", f"{invalid} category records are malformed"
            )
        )
    return identifiers


def _terminal_report(
    data: dict[str, Any], runtime: Path
) -> tuple[dict[str, Any], list[dict[str, str]], list[str]]:
    issues: list[dict[str, str]] = []
    terminals = data.get("terminals")
    observations: dict[str, Any] = {
        "listed": 0,
        "valid_private": 0,
        "invalid_records": 0,
    }
    endpoint_ids: list[str] = []
    version = data.get("version")
    if type(version) is not int or version != 1:
        issues.append(
            issue(
                "terminals-version-invalid", "Terminal registry version is unsupported"
            )
        )
        return observations, issues, []
    if not isinstance(terminals, list) or len(terminals) > MAX_TERMINALS:
        observations["listed"] = len(terminals) if isinstance(terminals, list) else 0
        issues.append(
            issue("terminals-invalid", "Terminal inventory is malformed or exceeds 128")
        )
        return observations, issues, []
    observations["listed"] = len(terminals)
    identifiers: set[str] = set()
    for terminal in terminals:
        if not isinstance(terminal, dict):
            observations["invalid_records"] += 1
            continue
        identifier = terminal.get("id")
        endpoint = terminal.get("endpoint")
        expected_endpoint = str(runtime / f"{identifier}.sock")
        if (
            not isinstance(identifier, str)
            or not identifier.startswith("terminal-")
            or identifier in identifiers
            or endpoint != expected_endpoint
        ):
            observations["invalid_records"] += 1
            issues.append(
                issue(
                    "terminals-endpoint-foreign",
                    "A terminal endpoint is malformed or outside the runtime",
                )
            )
            continue
        identifiers.add(identifier)
        observations["valid_private"] += 1
        endpoint_ids.append(identifier)
        if not _bounded_path(terminal.get("program")) or not _bounded_path(
            terminal.get("directory")
        ):
            observations["invalid_records"] += 1
            issues.append(
                issue("terminals-launch-invalid", "A terminal launch path is malformed")
            )
        if not _valid_arguments(terminal.get("arguments", []), MAX_TERMINAL_ARGUMENTS):
            observations["invalid_records"] += 1
            issues.append(
                issue(
                    "terminals-arguments-invalid",
                    "Terminal arguments exceed documented bounds",
                )
            )
    return observations, issues, endpoint_ids


def _descriptor_observation(path: Path) -> str:
    """Apply descriptor ownership/shape rules without proving a launch match."""
    if path_state(path)["kind"] == "symlink":
        return "invalid"
    descriptor: int | None = None
    try:
        descriptor = os.open(path, FILE_OPEN_FLAGS)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            return "invalid"
        if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600:
            return "invalid"
        if info.st_size != DESCRIPTOR_SIZE:
            return "invalid"
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            descriptor = None
            value = stream.read(DESCRIPTOR_SIZE + 1)
        if len(value) != DESCRIPTOR_SIZE or not value.startswith(DESCRIPTOR_MAGIC):
            return "invalid"
        identity = value[len(DESCRIPTOR_MAGIC) : len(DESCRIPTOR_MAGIC) + 32]
        return "valid_shape" if any(identity) else "invalid"
    except FileNotFoundError:
        return "missing"
    except (OSError, ValueError):
        return "unreadable"
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _ancestor_is_trusted(path: Path) -> tuple[bool, str]:
    """Mirror endpoint-parent trust without following the runtime link itself."""
    try:
        info = os.lstat(path)
    except OSError as error:
        return False, error.strerror or "ancestor cannot be inspected"
    if not stat.S_ISDIR(info.st_mode):
        return False, "ancestor is not a directory"
    owner_ok = info.st_uid in (os.geteuid(), 0)
    writable = bool(info.st_mode & 0o022)
    sticky = bool(info.st_mode & stat.S_ISVTX)
    if not owner_ok or (writable and not sticky):
        return False, "ancestor is writable by an untrusted owner"
    return True, ""


def _connect_classification(result: int | None) -> str:
    """Classify one zero-byte connect without interpreting protocol state."""
    if result == 0:
        return "connectable"
    if result == errno.ECONNREFUSED:
        return "not_listening"
    if result in CONNECT_TIMEOUT_ERRNOS:
        return "timeout"
    return "unknown"


def _empty_distribution() -> dict[str, Any]:
    return {
        "sample_count": 0,
        "p50_ms": None,
        "p95_ms": None,
        "p99_ms": None,
        "max_ms": None,
    }


def _distribution(durations: Iterable[float]) -> dict[str, Any]:
    """Return bounded nearest-rank connect-time distributions in milliseconds."""
    values = sorted(durations)
    if not values:
        return _empty_distribution()

    def percentile(percentage: int) -> float:
        rank = -(-percentage * len(values) // 100)
        return round(values[max(0, min(len(values) - 1, rank - 1))], 3)

    return {
        "sample_count": len(values),
        "p50_ms": percentile(50),
        "p95_ms": percentile(95),
        "p99_ms": percentile(99),
        "max_ms": round(values[-1], 3),
    }


def _probe_endpoints(
    runtime: Path, identifiers: Iterable[str], samples: int = 1
) -> tuple[dict[str, Any], dict[str, Any]]:
    reachability: dict[str, Any] = {
        "method": "Unix-socket connect, immediately closed; no protocol bytes",
        "limit": MAX_ENDPOINT_PROBES,
        "probed": 0,
        "connectable": 0,
        "not_listening": 0,
        "timeout": 0,
        "missing": 0,
        "unknown": 0,
        "unexamined": 0,
        "samples_requested": samples,
        "sample_count": 0,
        "success_count": 0,
        "timeout_count": 0,
        "health": "unknown",
        "authoritative": False,
    }
    durations: list[float] = []
    classification_durations: dict[str, list[float]] = {
        name: [] for name in ("connectable", "not_listening", "timeout", "unknown")
    }
    measurement: dict[str, Any] = {
        "method": (
            "Monotonic duration around a zero-byte Unix-socket connect; "
            "no protocol bytes"
        ),
        "unit": "milliseconds",
        "claim": (
            "Local connect diagnostics only; not service health, protocol "
            "compatibility, workload capacity, or resource acceptance"
        ),
        "samples_requested": samples,
        "sample_count": 0,
        "success_count": 0,
        "timeout_count": 0,
        "aggregate": _empty_distribution(),
        "by_classification": {
            name: _empty_distribution()
            for name in ("connectable", "not_listening", "timeout", "unknown")
        },
    }
    descriptors: dict[str, Any] = {
        "examined": 0,
        "valid_shape": 0,
        "invalid": 0,
        "missing": 0,
        "unreadable": 0,
        "identity_match": "unknown without the launch fingerprint",
        "proof_of_liveness": False,
    }
    all_ids = list(identifiers)
    reachability["unexamined"] = max(0, len(all_ids) - MAX_ENDPOINT_PROBES)
    for identifier in all_ids[:MAX_ENDPOINT_PROBES]:
        endpoint = runtime / f"{identifier}.sock"
        observed = path_state(endpoint)
        if observed["state"] != "present" or observed["kind"] not in ("socket",):
            if observed["state"] == "missing":
                reachability["missing"] += 1
            else:
                reachability["unknown"] += 1
        else:
            for _ in range(samples):
                started: int | None = None
                try:
                    with socket.socket(
                        socket.AF_UNIX, socket.SOCK_STREAM
                    ) as connection:
                        connection.settimeout(CONNECT_TIMEOUT_SECONDS)
                        started = time.monotonic_ns()
                        result = connection.connect_ex(str(endpoint))
                except (OSError, OverflowError) as error:
                    result = (
                        errno.ETIMEDOUT
                        if isinstance(error, TimeoutError)
                        else error.errno
                    )
                    elapsed_ms = (
                        (time.monotonic_ns() - started) / 1_000_000
                        if started is not None
                        else 0.0
                    )
                else:
                    elapsed_ms = (time.monotonic_ns() - started) / 1_000_000
                classification = _connect_classification(result)
                reachability[classification] += 1
                durations.append(elapsed_ms)
                classification_durations[classification].append(elapsed_ms)
                reachability["sample_count"] += 1
                reachability["success_count"] += classification == "connectable"
                reachability["timeout_count"] += classification == "timeout"
        reachability["probed"] += 1
        state = _descriptor_observation(runtime / f"{identifier}.sock.session")
        if state != "missing":
            descriptors["examined"] += 1
        descriptors[state] += 1
    measurement["sample_count"] = reachability["sample_count"]
    measurement["success_count"] = reachability["success_count"]
    measurement["timeout_count"] = reachability["timeout_count"]
    measurement["aggregate"] = _distribution(durations)
    measurement["by_classification"] = {
        name: _distribution(values) for name, values in classification_durations.items()
    }
    reachability["measurement"] = measurement
    return reachability, descriptors


def diagnose_runtime(selected: Path, samples: int = 1) -> dict[str, Any]:
    """Apply launcher privacy rules and collect counts, not session content."""
    result: dict[str, Any] = {
        "selected": str(selected),
        "state": "ok",
        "issues": [],
        "inventory": {},
        "registry": {},
        "terminals": {"state": "missing", "issues": []},
        "reachability": {},
        "descriptors": {},
    }
    issues: list[dict[str, str]] = result["issues"]
    try:
        runtime_stat = os.lstat(selected)
        lapis.validate_runtime_directory(runtime_stat, selected)
        canonical = selected.resolve(strict=True)
        ancestor = canonical.parent
        while True:
            trusted, detail = _ancestor_is_trusted(ancestor)
            if not trusted:
                result["state"] = "invalid"
                issues.append(issue("runtime-ancestor-untrusted", detail))
                return result
            if ancestor == ancestor.parent:
                break
            ancestor = ancestor.parent
    except FileNotFoundError:
        result["state"] = "missing"
        issues.append(
            issue(
                "runtime-missing",
                "Runtime directory does not exist; it is created on first launch",
            )
        )
        return result
    except (OSError, lapis.SetupError) as error:
        result["state"] = "invalid"
        issues.append(issue("runtime-invalid", str(error)))
        return result

    try:
        entries = list(islice(selected.iterdir(), MAX_INVENTORY_ENTRIES + 1))
        truncated = len(entries) > MAX_INVENTORY_ENTRIES
        entries = entries[:MAX_INVENTORY_ENTRIES]
        socket_count = sum(path_state(entry)["kind"] == "socket" for entry in entries)
        result["inventory"] = {
            "direct_entries": {
                "observed": len(entries),
                "limit": MAX_INVENTORY_ENTRIES,
                "truncated": truncated,
            },
            "sockets": socket_count,
        }
    except OSError as error:
        result["state"] = "inaccessible"
        issues.append(
            issue("runtime-inaccessible", error.strerror or "runtime cannot be listed")
        )
        return result

    registry, registry_issues, parseable = _private_json(selected / "workspace.json")
    registry_state = registry.pop("state")
    registry_data = registry.pop("data", {})
    agents: dict[str, Any] = {"state": registry_state, "issues": registry_issues}
    endpoint_ids: list[str] = ["desktop-v6"]
    if parseable:
        agent_observations, agent_issues, agent_endpoint_ids = _agent_report(
            registry_data, selected
        )
        agents["observations"] = agent_observations
        agents["issues"].extend(agent_issues)
        if any(
            code.startswith("registry-")
            for code in (item["code"] for item in agent_issues)
        ):
            agents["state"] = "invalid"
        endpoint_ids.extend(agent_endpoint_ids)
    agents["issues"].extend(registry_issues)
    result["registry"] = agents
    if agents["issues"] or agents["state"] in ("invalid", "corrupt", "inaccessible"):
        if agents["state"] == "ok":
            agents["state"] = "invalid"
        result["state"] = agents["state"]

    terminals, terminal_issues, terminal_parseable = _private_json(
        selected / "terminals.json"
    )
    terminal_state = terminals.pop("state")
    terminal_data = terminals.pop("data", {})
    terminal_result: dict[str, Any] = {
        "state": terminal_state,
        "issues": terminal_issues,
    }
    if terminal_parseable:
        terminal_observations, terminal_record_issues, terminal_ids = _terminal_report(
            terminal_data, selected
        )
        terminal_result["observations"] = terminal_observations
        terminal_result["issues"].extend(terminal_record_issues)
        if terminal_record_issues:
            terminal_result["state"] = "invalid"
        endpoint_ids.extend(terminal_ids)
    result["terminals"] = terminal_result
    if terminal_result["issues"] or terminal_state in (
        "invalid",
        "corrupt",
        "inaccessible",
    ):
        if terminal_result["state"] == "ok":
            terminal_result["state"] = "invalid"
        result["state"] = terminal_result["state"]

    reachability, descriptors = _probe_endpoints(selected, endpoint_ids, samples)
    result["reachability"] = reachability
    result["descriptors"] = descriptors
    return result


def diagnose(app: Path, runtime: Path, samples: int = 1) -> dict[str, Any]:
    """Collect the complete diagnostic result and its meaningful exit status."""
    application = diagnose_app(app)
    selected_runtime = diagnose_runtime(runtime, samples)
    input_ready = application["state"] == "ok" and selected_runtime["state"] == "ok"
    return {
        "schema_version": SCHEMA_VERSION,
        "diagnostic_status": "ready" if input_ready else "attention",
        "exit_status": 0 if input_ready else 1,
        "scope": "read-only structural diagnosis; file and socket evidence is not health",
        "health": "unknown",
        "app": application,
        "runtime": selected_runtime,
    }


SUPPORT_SCHEMA_VERSION = 1


def _support_issues(issues: list[dict[str, str]]) -> list[dict[str, str]]:
    """Copy only fixed details; runtime contract errors never carry a path."""
    return [
        {
            "code": item["code"],
            "detail": (
                "Runtime directory does not satisfy the private runtime rules"
                if item["code"] == "runtime-invalid"
                else item["detail"]
            ),
        }
        for item in issues
    ]


def _support_subdict(value: dict[str, Any], names: tuple[str, ...]) -> dict[str, Any]:
    return {name: value[name] for name in names if name in value}


def support_export(report: dict[str, Any]) -> dict[str, Any]:
    """Reduce a diagnostic to counts and metadata presence, never identities."""
    application = report["app"]
    runtime = report["runtime"]
    metadata = application.get("metadata", {})
    app = {
        "state": application["state"],
        "kind": application["kind"],
        "issues": _support_issues(application["issues"]),
        "metadata": {
            "source": metadata.get("source"),
            "state": metadata.get("state"),
            "short_version": metadata.get("short_version"),
            "bundle_version": metadata.get("bundle_version"),
            "minimum_system": metadata.get("minimum_system"),
            "bundle_identifier_present": metadata.get(
                "bundle_identifier_present", False
            ),
            "executable_name_present": metadata.get("executable_name_present", False),
        },
        "executable": _support_subdict(
            application.get("executable", {}), ("present", "executable_bit")
        ),
        "protocol": _support_subdict(
            application.get("protocol", {}),
            (
                "source_wire_version",
                "runtime_wire_version",
                "negotiation_attempted",
            ),
        ),
    }
    registry = _support_subdict(
        runtime.get("registry", {}), ("state", "observations", "issues")
    )
    registry["issues"] = _support_issues(registry.get("issues", []))
    terminals = _support_subdict(
        runtime.get("terminals", {}), ("state", "observations", "issues")
    )
    terminals["issues"] = _support_issues(terminals.get("issues", []))
    reachability = _support_subdict(
        runtime.get("reachability", {}),
        (
            "method",
            "limit",
            "probed",
            "connectable",
            "not_listening",
            "timeout",
            "missing",
            "unknown",
            "unexamined",
            "samples_requested",
            "sample_count",
            "success_count",
            "timeout_count",
            "health",
            "authoritative",
        ),
    )
    reachability["measurement"] = _support_subdict(
        runtime.get("reachability", {}).get("measurement", {}),
        (
            "method",
            "unit",
            "claim",
            "samples_requested",
            "sample_count",
            "success_count",
            "timeout_count",
            "aggregate",
            "by_classification",
        ),
    )
    selected_runtime = {
        "state": runtime["state"],
        "issues": _support_issues(runtime["issues"]),
        "inventory": runtime.get("inventory", {}),
        "registry": registry,
        "terminals": terminals,
        "reachability": reachability,
        "descriptors": runtime.get("descriptors", {}),
    }
    return {
        "schema_version": SUPPORT_SCHEMA_VERSION,
        "export": "support",
        "platform": sys.platform,
        "diagnostic_status": report["diagnostic_status"],
        "exit_status": report["exit_status"],
        "scope": (
            "redacted structural diagnosis; no selected paths, session identity, "
            "launch data, terminal content, protocol bytes, or health claim; "
            "connect timing is aggregate only"
        ),
        "health": "unknown",
        "app": app,
        "runtime": selected_runtime,
    }


def _text_report(report: dict[str, Any]) -> str:
    app = report["app"]
    runtime = report["runtime"]
    registry = runtime["registry"]
    reachability = runtime["reachability"]
    version = app.get("metadata", {}).get("short_version") or "unknown version"
    agents = registry.get("observations", {}).get("listed", 0)
    categories = registry.get("observations", {}).get("categories_listed", 0)
    connectable = reachability.get("connectable", 0)
    not_listening = reachability.get("not_listening", 0)
    unknown = reachability.get("unknown", 0)
    unexamined = reachability.get("unexamined", 0)
    distribution = reachability.get("measurement", {}).get("aggregate", {})
    if distribution.get("sample_count"):
        timing = (
            f"Connect timings: p50 {distribution['p50_ms']} ms, "
            f"p95 {distribution['p95_ms']} ms, p99 {distribution['p99_ms']} ms, "
            f"max {distribution['max_ms']} ms (local connect diagnostics only).\n"
        )
    else:
        timing = ""
    return (
        f"App: {app['state']} ({version}; package metadata only)\n"
        f"Runtime: {runtime['state']} ({agents} agents, {categories} categories recorded)\n"
        f"Endpoints: {connectable} connectable, "
        f"{not_listening} not listening, {unknown} unknown; "
        f"{unexamined} not examined\n"
        f"{timing}"
        "Health: unknown. Read-only: no GUI, session restore, terminal content, or protocol handshake; "
        "file/socket evidence is not authoritative health."
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--json", action="store_true", help="write the machine-readable report"
    )
    parser.add_argument(
        "--export",
        action="store_true",
        help="write the bounded, redacted support export instead of the full report",
    )
    parser.add_argument(
        "--app", type=Path, help="application bundle or executable to inspect"
    )
    parser.add_argument(
        "--runtime", type=Path, help="private lapis runtime directory to inspect"
    )
    parser.add_argument(
        "--samples",
        type=_bounded_sample_count,
        default=1,
        help=(
            "bounded connect-only samples per endpoint, 1-8; default 1. "
            "The worst case adds about 10 seconds"
        ),
    )
    return parser


def main(arguments: list[str] | None = None) -> int:
    parser = build_parser()
    options = parser.parse_args(arguments)
    if options.export and options.json:
        parser.error("--export and --json are mutually exclusive")
    app = options.app.expanduser() if options.app else default_app()
    runtime = options.runtime.expanduser() if options.runtime else default_runtime()
    report = diagnose(app, runtime, options.samples)
    if options.export:
        print(json.dumps(support_export(report), indent=2, sort_keys=True))
    elif options.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(_text_report(report))
    return int(report["exit_status"])


if __name__ == "__main__":
    raise SystemExit(main())
