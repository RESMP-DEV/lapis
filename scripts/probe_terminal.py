"""Build pinned headless engine experiments and retain replay results, including failures."""

import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time
import urllib.request
import uuid
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBES = ROOT / "tools" / "terminal_probe"
ARCHIVE_MARKER = ".lapis-archive.json"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_source_tree(bundle, members, prefix, destination):
    """Check the complete pinned tree; generated build files belong elsewhere."""
    expected_paths = {Path(ARCHIVE_MARKER)}
    for member in members:
        relative = Path(member.name).relative_to(prefix)
        if relative == Path(ARCHIVE_MARKER):
            raise RuntimeError("Archive contains the reserved ownership marker")
        expected_paths.add(relative)
        expected_paths.update(relative.parents)
        path = destination / relative
        resolved_destination = destination.resolve()
        candidate = path
        while candidate != destination:
            if candidate.is_symlink():
                resolved_candidate = candidate.resolve()
                if not resolved_candidate.is_relative_to(resolved_destination):
                    raise RuntimeError(f"Source path escapes archive: {path}")
                break
            candidate = candidate.parent
        if not path.resolve().is_relative_to(resolved_destination):
            raise RuntimeError(f"Source path escapes archive: {path}")
        if member.isfile() or member.islnk():
            with bundle.extractfile(member) as stream:
                expected = hashlib.file_digest(stream, "sha256").hexdigest()
            if path.is_symlink() or not path.is_file() or digest(path) != expected:
                raise RuntimeError(f"Pinned source changed: {path}")
        elif member.issym():
            if not path.is_symlink() or str(path.readlink()) != member.linkname:
                raise RuntimeError(f"Pinned source symlink changed: {path}")
        elif member.isdir() and (path.is_symlink() or not path.is_dir()):
            raise RuntimeError(f"Pinned source directory changed: {path}")
    for root, directories, files in os.walk(destination, followlinks=False):
        for name in directories + files:
            path = Path(root) / name
            if path.relative_to(destination) not in expected_paths:
                raise RuntimeError(f"Unexpected source path: {path}")


def prepare_archive(entry, directory):
    """Reuse only owned, unchanged trees; leave rejected destinations untouched."""
    relative = Path(entry["destination"])
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise RuntimeError("Archive destination must be inside the build directory")
    destination = directory / relative
    if destination.is_symlink():
        raise RuntimeError(f"Refusing symlinked source root: {destination}")
    if not destination.resolve().is_relative_to(directory.resolve()):
        raise RuntimeError("Archive destination must be inside the build directory")
    marker = destination / ARCHIVE_MARKER
    ownership = {"schema_version": 1, "archive_sha256": entry["sha256"]}
    if destination.exists() and (
        not destination.is_dir()
        or marker.is_symlink()
        or not marker.is_file()
        or marker.read_text() != json.dumps(ownership) + "\n"
    ):
        raise RuntimeError(
            f"Refusing unowned source tree: {destination}; use a fresh --build-root"
        )
    downloads = directory / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / f"{entry['name']}.tar"
    if not archive.exists():
        partial = archive.with_suffix(".part")
        request = urllib.request.Request(
            entry["url"], headers={"User-Agent": "lapis-probe"}
        )
        with (
            urllib.request.urlopen(request, timeout=60) as response,
            partial.open("wb") as output,
        ):
            shutil.copyfileobj(response, output)
        partial.replace(archive)
    if digest(archive) != entry["sha256"]:
        raise RuntimeError(f"Archive checksum mismatch: {archive}")
    with tarfile.open(archive) as bundle:
        members = bundle.getmembers()
        if any(
            not Path(member.name).parts
            or Path(member.name).is_absolute()
            or ".." in Path(member.name).parts
            for member in members
        ):
            raise RuntimeError("Archive contains an unsafe path")
        prefixes = {Path(member.name).parts[0] for member in members}
        if len(prefixes) != 1:
            raise RuntimeError(f"Expected one archive root: {archive}")
        prefix = prefixes.pop()
        if any(
            Path(member.name).relative_to(prefix) == Path(ARCHIVE_MARKER)
            for member in members
        ):
            raise RuntimeError("Archive contains the reserved ownership marker")
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=directory) as temporary:
                bundle.extractall(temporary, filter="data")
                extracted = Path(temporary) / prefix
                if extracted.is_symlink() or not extracted.is_dir():
                    raise RuntimeError("Archive root must be a directory")
                (extracted / ARCHIVE_MARKER).write_text(json.dumps(ownership) + "\n")
                verify_source_tree(bundle, members, prefix, extracted)
                extracted.rename(destination)
        else:
            verify_source_tree(bundle, members, prefix, destination)
    return destination


def prepare_derived_archive(entry, directory, run_directory):
    """Copy a verified pristine tree to a path only this invocation owns."""
    pristine = prepare_archive(entry, directory)
    relative = Path(entry["destination"])
    derived = run_directory / relative
    if derived.exists() or derived.is_symlink():
        raise RuntimeError(f"Derived source path is already occupied: {derived}")
    derived.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(pristine, derived, symlinks=True)
    with tarfile.open(directory / "downloads" / f"{entry['name']}.tar") as bundle:
        members = bundle.getmembers()
        prefix = Path(members[0].name).parts[0]
        verify_source_tree(bundle, members, prefix, derived)
    return pristine, derived


def stop_process_group(process):
    """Bound timeout cleanup, including children whose parent already exited."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass
    # Waiting only for the leader is insufficient: compilers can still be alive.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def run_process(command, *, timeout, **kwargs):
    """Run a POSIX process group and preserve partial output on timeout."""
    command = [str(arg) for arg in command]
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    try:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            stop_process_group(process)
            try:
                stdout, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired as cleanup_error:
                stdout, stderr = cleanup_error.output, cleanup_error.stderr
                for stream in (process.stdout, process.stderr):
                    if stream is not None:
                        stream.close()
            if stdout is not None:
                error.output = stdout
            if stderr is not None:
                error.stderr = stderr
            raise
        except BaseException:
            stop_process_group(process)
            raise
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    finally:
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream is not None:
                stream.close()


def run_step(
    name,
    command,
    directory,
    steps,
    *,
    cwd=None,
    env=None,
    allow_failure=False,
    timeout=1800,
):
    log = directory / f"{name}.log"
    start = time.monotonic()
    step = {
        "name": name,
        "command": [str(arg) for arg in command],
        "log": str(log),
        "cwd": str(cwd or Path.cwd()),
    }
    steps.append(step)
    try:
        with log.open("w") as output:
            result = run_process(
                command,
                cwd=cwd,
                env=env,
                stdout=output,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
        step["exit_code"] = result.returncode
    except subprocess.TimeoutExpired:
        step.update(exit_code=None, timed_out=True)
        raise
    finally:
        step["elapsed_seconds"] = round(time.monotonic() - start, 3)
    print(f"{'PASS' if result.returncode == 0 else 'FAIL'} {name}: {log}", flush=True)
    if result.returncode and not allow_failure:
        raise RuntimeError(log.read_text()[-5000:])
    return result.returncode


def run_replay(executable, reports, receipt, *, timeout=60):
    """Keep replay output and status even when it crashes or times out."""
    stdout = reports / "replay.json"
    stderr = reports / "replay.stderr"
    receipt.update(
        replay_stdout=str(stdout),
        replay_stderr=str(stderr),
        binary_sha256=digest(executable),
    )
    start = time.monotonic()
    try:
        with stdout.open("w") as output, stderr.open("w") as errors:
            result = run_process(
                [executable], stdout=output, stderr=errors, timeout=timeout
            )
        receipt["replay_exit_code"] = result.returncode
    except subprocess.TimeoutExpired:
        receipt.update(replay_exit_code=None, replay_timed_out=True)
        raise
    finally:
        receipt["replay_elapsed_seconds"] = round(time.monotonic() - start, 3)
    receipt["replay"] = json.loads(stdout.read_text())
    return result.returncode


def sanitizer_scope(engine, mode):
    if mode == "dev":
        return (
            "none; Ghostty upstream uses ReleaseSafe" if engine == "ghostty" else "none"
        )
    return (
        "C++ adapter/runner only; Zig library uses ReleaseSafe"
        if engine == "ghostty"
        else "C++ consumer, engine and compiled dependency graph"
    )


def default_llvm():
    if os.environ.get("LAPIS_LLVM_BIN"):
        return Path(os.environ["LAPIS_LLVM_BIN"])
    if platform.system() == "Darwin" and shutil.which("brew"):
        result = subprocess.run(
            ["brew", "--prefix", "llvm"], capture_output=True, text=True, check=True
        )
        return Path(result.stdout.strip()) / "bin"
    return Path("/usr/bin")


def probe(engine, args):
    directory = args.build_root.resolve() / engine
    invocation = getattr(args, "invocation", None) or uuid.uuid4().hex
    args.invocation = invocation
    run_directory = directory / "runs" / invocation
    directory.mkdir(parents=True, exist_ok=True)
    reports = directory / "reports" / args.mode
    reports.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((PROBES / engine / "sources.json").read_text())
    receipt = {
        "schema_version": 1,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "engine": engine,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "mode": args.mode,
        "sanitizer_scope": sanitizer_scope(engine, args.mode),
        "sources": manifest,
        "derived_sources": [],
        "steps": [],
        "passed": False,
        "scope": "Headless replay, not PTY, GUI, GPU or latency acceptance",
    }
    steps = receipt["steps"]
    try:
        for entry in manifest["archives"]:
            pristine, derived = prepare_derived_archive(entry, directory, run_directory)
            receipt["derived_sources"].append(
                {
                    "archive": entry["name"],
                    "archive_sha256": entry["sha256"],
                    "pristine_source": str(pristine),
                    "derived_source": str(derived),
                }
            )
        build = run_directory / "build"
        configure = [
            "cmake",
            "-S",
            PROBES / engine,
            "-B",
            build,
            "-G",
            "Ninja",
            f"-DCMAKE_CXX_COMPILER={args.cxx}",
            f"-DCMAKE_C_COMPILER={args.cc}",
            f"-DCMAKE_CXX_FLAGS={args.cxx_flags}",
            "-DCMAKE_BUILD_TYPE=Debug",
            f"-DLAPIS_SANITIZER={'address' if args.mode == 'asan' else 'none'}",
        ]
        environment = dict(os.environ)
        if engine == "ghostty":
            host = f"{platform.system()}-{platform.machine()}"
            compiler = manifest["zig"][host]
            zig_entry = dict(compiler, name="zig", destination="zig")
            zig = prepare_archive(zig_entry, directory) / "zig"
            environment["ZIG_GLOBAL_CACHE_DIR"] = str(directory / "zig-cache")
            options = [
                arg for arg in manifest["build_args"] if not arg.startswith("-j")
            ]
            run_step(
                "zig-build",
                [
                    zig,
                    "build",
                    *options,
                    f"-j{args.jobs}",
                    "--prefix",
                    run_directory / "prefix",
                    "--cache-dir",
                    directory / "zig-local-cache",
                ],
                reports,
                steps,
                cwd=run_directory / "ghostty-source",
                env=environment,
            )
            configure.append(f"-DGHOSTTY_PREFIX={run_directory / 'prefix'}")
        else:
            # A fresh CMake cache keeps libunicode defaults inside this source copy.
            configure.extend(
                [
                    f"-DCONTOUR_SOURCE_DIR={run_directory / 'contour-source'}",
                    f"-DCONTOUR_DEPS_DIR={run_directory / 'contour-deps'}",
                    "-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=FALSE",
                ]
            )
            empty = directory / "empty-pkgconfig"
            empty.mkdir(exist_ok=True)
            environment["PKG_CONFIG_LIBDIR"] = str(empty)
            environment["PKG_CONFIG_PATH"] = ""
        run_step("configure", configure, reports, steps, env=environment)
        run_step(
            "build",
            ["cmake", "--build", build, "--parallel", args.jobs],
            reports,
            steps,
        )
        executable = build / f"lapis_{engine}_probe"
        replay_exit_code = run_replay(executable, reports, receipt)
        receipt["source_file_sha256"] = {
            str(path.relative_to(ROOT)): digest(path)
            for path in sorted(
                [
                    *PROBES.rglob("*"),
                    ROOT / "cmake/ProjectOptions.cmake",
                    Path(__file__),
                ]
            )
            if path.is_file()
        }
        receipt["compiler"] = subprocess.run(
            [args.cxx, "--version"],
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.strip()
        ctest = run_step(
            "ctest",
            ["ctest", "--test-dir", build, "--output-on-failure"],
            reports,
            steps,
            allow_failure=True,
        )
        receipt["passed"] = (
            replay_exit_code == 0 and receipt["replay"]["passed"] and ctest == 0
        )
    except (
        OSError,
        RuntimeError,
        ValueError,
        KeyError,
        tarfile.TarError,
        subprocess.SubprocessError,
    ) as error:
        receipt["error"] = str(error)
        print(f"FAIL {engine}: {error}", flush=True)
    finally:
        path = reports / "receipt.json"
        path.write_text(json.dumps(receipt, indent=2) + "\n")
        print(f"Receipt: {path}", flush=True)
    return receipt["passed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--engine", choices=("ghostty", "contour", "all"), default="all"
    )
    parser.add_argument("--mode", choices=("dev", "asan"), default="dev")
    parser.add_argument(
        "--build-root", type=Path, default=ROOT / "build/terminal-probe/reproduce"
    )
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--cxx")
    parser.add_argument("--cc")
    parser.add_argument("--cxx-flags", default="")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    llvm = default_llvm()
    args.cxx = args.cxx or str(llvm / "clang++")
    args.cc = args.cc or str(llvm / "clang")
    engines = ("ghostty", "contour") if args.engine == "all" else (args.engine,)

    # Separate trees, with one shared CPU budget. Failure does not cancel the peer.
    def run_engine(engine):
        bounded = argparse.Namespace(**vars(args))
        bounded.jobs = max(1, args.jobs // len(engines))
        return probe(engine, bounded)

    with ThreadPoolExecutor(max_workers=min(len(engines), args.jobs)) as pool:
        results = list(pool.map(run_engine, engines))
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
