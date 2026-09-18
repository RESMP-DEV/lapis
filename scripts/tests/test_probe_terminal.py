"""Exercise pinned-source rejection and failure receipts without upstream downloads."""

import argparse
import io
import json
import os
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from scripts import probe_terminal as probe


class ProbeTests(unittest.TestCase):
    def setUp(self):
        (probe.ROOT / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=probe.ROOT / "build")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "source.tar"
        with tarfile.open(self.archive, "w") as archive:
            member = tarfile.TarInfo("package/source.cpp")
            member.size = 6
            archive.addfile(member, io.BytesIO(b"source"))
        self.entry = {
            "name": "source",
            "url": self.archive.as_uri(),
            "sha256": probe.digest(self.archive),
            "destination": "source",
        }
        self.cache = self.root / "cache"

    def test_extract_reuse_and_reject_modified_source(self):
        source = probe.prepare_archive(self.entry, self.cache)
        self.assertEqual((source / "source.cpp").read_bytes(), b"source")
        self.assertEqual(probe.prepare_archive(self.entry, self.cache), source)
        (source / "source.cpp").write_text("modified")
        with self.assertRaisesRegex(RuntimeError, "Pinned source changed"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertEqual((source / "source.cpp").read_text(), "modified")

    def test_derived_copy_isolation_preserves_verified_cache(self):
        pristine = probe.prepare_archive(self.entry, self.cache)
        run_one = self.cache / "runs" / "one"
        verified_one, derived_one = probe.prepare_derived_archive(
            self.entry, self.cache, run_one
        )

        (derived_one / "source.cpp").write_text("upstream generated output")

        run_two = self.cache / "runs" / "two"
        verified_two, derived_two = probe.prepare_derived_archive(
            self.entry, self.cache, run_two
        )
        self.assertEqual(verified_one, pristine)
        self.assertEqual(verified_two, pristine)
        self.assertNotEqual(derived_one, derived_two)
        self.assertEqual((pristine / "source.cpp").read_bytes(), b"source")
        self.assertEqual(
            (derived_one / "source.cpp").read_text(), "upstream generated output"
        )
        self.assertEqual((derived_two / "source.cpp").read_bytes(), b"source")

    def test_unknown_source_tree_is_preserved(self):
        source = self.cache / "source"
        source.mkdir(parents=True)
        (source / "source.cpp").write_bytes(b"source")
        (source / "notes.txt").write_text("keep my work")
        before = {path.name: path.read_bytes() for path in source.iterdir()}
        with self.assertRaisesRegex(RuntimeError, "unowned.*fresh --build-root"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertEqual(
            {path.name: path.read_bytes() for path in source.iterdir()}, before
        )
        self.assertFalse((self.cache / "downloads").exists())

    def test_extra_header_is_rejected_without_mutation(self):
        source = probe.prepare_archive(self.entry, self.cache)
        header = source / "format"
        header.write_text("unexpected standard-header override")
        before = {path.name: path.read_bytes() for path in source.iterdir()}
        with self.assertRaisesRegex(RuntimeError, "Unexpected source path"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertEqual(
            {path.name: path.read_bytes() for path in source.iterdir()}, before
        )

    def test_symlinked_root_is_rejected_without_mutation(self):
        source = probe.prepare_archive(self.entry, self.cache)
        link = self.cache / "source-link"
        link.symlink_to(source, target_is_directory=True)
        self.entry["destination"] = link.name
        with self.assertRaisesRegex(RuntimeError, "symlinked source root"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertTrue(link.is_symlink())
        self.assertEqual((source / "source.cpp").read_bytes(), b"source")

    def test_changed_ownership_marker_is_preserved(self):
        source = probe.prepare_archive(self.entry, self.cache)
        marker = source / probe.ARCHIVE_MARKER
        marker.write_text('{"archive_sha256": "different archive"}\n')
        before = marker.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "unowned source tree"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertEqual(marker.read_bytes(), before)

    def test_reject_archive_checksum(self):
        self.entry["sha256"] = "0" * 64
        with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertFalse((self.cache / "source").exists())

    def test_reject_escape_destination(self):
        self.entry["destination"] = "../outside"
        with self.assertRaisesRegex(RuntimeError, "inside the build directory"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertFalse((self.root / "outside").exists())

    def test_reject_traversal_member(self):
        with tarfile.open(self.archive, "w") as archive:
            member = tarfile.TarInfo("package/../../outside")
            member.size = 1
            archive.addfile(member, io.BytesIO(b"x"))
        self.entry["sha256"] = probe.digest(self.archive)
        with self.assertRaisesRegex(RuntimeError, "unsafe path"):
            probe.prepare_archive(self.entry, self.cache)
        self.assertFalse((self.root / "outside").exists())

    def test_timeout_preserves_command_and_log(self):
        steps = []
        with (
            patch.object(
                probe,
                "run_process",
                side_effect=subprocess.TimeoutExpired(["build"], 1800),
            ),
            self.assertRaises(subprocess.TimeoutExpired),
        ):
            probe.run_step("build", ["build"], self.root, steps)
        self.assertTrue(steps[0]["timed_out"])
        self.assertIsNone(steps[0]["exit_code"])
        self.assertEqual(steps[0]["command"], ["build"])
        self.assertTrue(Path(steps[0]["log"]).exists())

    def test_timeout_stops_descendant_that_ignores_sigterm(self):
        child_code = (
            "import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
            "print('ready',flush=True); time.sleep(30)"
        )
        parent_code = (
            "import subprocess,sys,time; "
            f"child=subprocess.Popen([sys.executable,'-c',{child_code!r}],"
            "stdout=subprocess.PIPE,text=True); child.stdout.readline(); "
            "print(child.pid,flush=True); time.sleep(30)"
        )
        steps = []
        with self.assertRaises(subprocess.TimeoutExpired):
            probe.run_step(
                "descendant-timeout",
                [sys.executable, "-c", parent_code],
                self.root,
                steps,
                timeout=1,
            )
        child_pid = int(Path(steps[0]["log"]).read_text().strip())

        def running():
            # An orphan can briefly remain a zombie awaiting init; it cannot run.
            result = subprocess.run(
                ["ps", "-p", str(child_pid), "-o", "stat="],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            return result.returncode == 0 and not result.stdout.strip().startswith("Z")

        try:
            deadline = time.monotonic() + 2
            while running() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(running(), "timed-out build left its child running")
        finally:
            if running():
                os.kill(child_pid, signal.SIGKILL)
        self.assertTrue(steps[0]["timed_out"])
        self.assertIsNone(steps[0]["exit_code"])

    def configure_probe_fixture(self, engine="contour", mode="dev"):
        sources = self.root / "probes" / engine
        sources.mkdir(parents=True)
        (sources / "sources.json").write_text('{"archives": []}')
        args = argparse.Namespace(
            build_root=self.cache,
            mode=mode,
            cxx="clang++",
            cc="clang",
            cxx_flags="",
            jobs=1,
            invocation="fixture",
        )
        binary = (
            self.cache
            / engine
            / "runs"
            / args.invocation
            / "build"
            / f"lapis_{engine}_probe"
        )
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"fixture")
        return sources, binary, args

    def read_probe_receipt(self, engine, mode):
        return json.loads(
            (self.cache / engine / "reports" / mode / "receipt.json").read_text()
        )

    def test_crash_preserves_status_without_json(self):
        sources, binary, args = self.configure_probe_fixture()
        crashed = subprocess.CompletedProcess(
            [str(binary)], -11, "", "crash diagnostic"
        )

        def crash(command, **kwargs):
            kwargs["stderr"].write(crashed.stderr)
            return crashed

        with (
            redirect_stdout(io.StringIO()),
            patch.object(probe, "PROBES", sources.parent),
            patch.object(probe, "run_step", return_value=0),
            patch.object(probe, "run_process", side_effect=crash),
        ):
            self.assertFalse(probe.probe("contour", args))
        receipt = self.read_probe_receipt("contour", "dev")
        self.assertEqual(receipt["replay_exit_code"], -11)
        self.assertEqual(Path(receipt["replay_stderr"]).read_text(), "crash diagnostic")
        self.assertFalse(receipt["passed"])

    def test_replay_timeout_keeps_partial_output_and_failure_receipt(self):
        sources, binary, args = self.configure_probe_fixture()
        binary.write_text(
            f"#!{sys.executable}\nimport sys,time\n"
            "print('partial JSON',flush=True)\n"
            "print('partial diagnostic',file=sys.stderr,flush=True)\n"
            "time.sleep(30)\n"
        )
        binary.chmod(0o700)
        run_replay = probe.run_replay
        with (
            redirect_stdout(io.StringIO()),
            patch.object(probe, "PROBES", sources.parent),
            patch.object(probe, "run_step", return_value=0),
            patch.object(
                probe,
                "run_replay",
                side_effect=lambda *args: run_replay(*args, timeout=1),
            ),
        ):
            self.assertFalse(probe.probe("contour", args))
        receipt = self.read_probe_receipt("contour", "dev")
        self.assertFalse(receipt["passed"])
        self.assertTrue(receipt["replay_timed_out"])
        self.assertIsNone(receipt["replay_exit_code"])
        self.assertEqual(Path(receipt["replay_stdout"]).read_text(), "partial JSON\n")
        self.assertEqual(
            Path(receipt["replay_stderr"]).read_text(), "partial diagnostic\n"
        )
        self.assertEqual(receipt["binary_sha256"], probe.digest(binary))

    def test_sanitizer_labels_survive_failure_for_each_mode_and_engine(self):
        expected = {
            ("ghostty", "dev"): "none; Ghostty upstream uses ReleaseSafe",
            ("contour", "dev"): "none",
            (
                "ghostty",
                "asan",
            ): "C++ adapter/runner only; Zig library uses ReleaseSafe",
            ("contour", "asan"): "C++ consumer, engine and compiled dependency graph",
        }
        for (engine, mode), label in expected.items():
            with self.subTest(engine=engine, mode=mode):
                sources = self.root / f"labels-{engine}-{mode}" / engine
                sources.mkdir(parents=True)
                (sources / "sources.json").write_text('{"archives": [{}]}')
                args = argparse.Namespace(build_root=self.cache, mode=mode)
                with (
                    redirect_stdout(io.StringIO()),
                    patch.object(probe, "PROBES", sources.parent),
                    patch.object(
                        probe, "prepare_archive", side_effect=RuntimeError("fixture")
                    ),
                ):
                    self.assertFalse(probe.probe(engine, args))
                receipt = json.loads(
                    (
                        self.cache / engine / "reports" / mode / "receipt.json"
                    ).read_text()
                )
                self.assertEqual(receipt["sanitizer_scope"], label)
                self.assertFalse(receipt["passed"])


class CompilerDiscoveryTests(unittest.TestCase):
    def test_homebrew_failures_are_bounded_and_actionable(self):
        cases = [
            (subprocess.TimeoutExpired(["brew"], 15), "timed out after 15 seconds"),
            (
                subprocess.CalledProcessError(1, ["brew"], stderr="LLVM is missing"),
                "Homebrew LLVM discovery failed: LLVM is missing",
            ),
            (OSError("cannot execute"), "Cannot run Homebrew LLVM discovery"),
        ]
        for error, message in cases:
            with (
                self.subTest(error=type(error).__name__),
                patch.dict(os.environ, {"LAPIS_LLVM_BIN": ""}),
                patch.object(probe.platform, "system", return_value="Darwin"),
                patch.object(probe.shutil, "which", return_value="/tools/brew"),
                patch.object(probe, "run_process", side_effect=error) as run,
            ):
                with self.assertRaisesRegex(RuntimeError, message):
                    probe.default_llvm()
                self.assertEqual(run.call_args.kwargs["timeout"], 15)

    def test_explicit_compilers_bypass_discovery(self):
        with (
            patch.object(
                sys,
                "argv",
                [
                    "probe",
                    "--engine",
                    "ghostty",
                    "--cxx",
                    "/tools/c++",
                    "--cc",
                    "/tools/cc",
                ],
            ),
            patch.object(
                probe, "default_llvm", side_effect=AssertionError("discovery")
            ),
            patch.object(probe, "probe", return_value=True) as run,
        ):
            self.assertEqual(probe.main(), 0)
        args = run.call_args.args[1]
        self.assertEqual((args.cxx, args.cc), ("/tools/c++", "/tools/cc"))

    def test_partial_compiler_override_is_preserved(self):
        with (
            patch.object(
                sys, "argv", ["probe", "--engine", "ghostty", "--cxx", "/tools/c++"]
            ),
            patch.object(probe, "default_llvm", return_value=Path("/defaults/bin")),
            patch.object(probe, "probe", return_value=True) as run,
        ):
            self.assertEqual(probe.main(), 0)
        args = run.call_args.args[1]
        self.assertEqual((args.cxx, args.cc), ("/tools/c++", "/defaults/bin/clang"))

    def test_discovery_failure_exits_before_engine_work(self):
        diagnostic = io.StringIO()
        with (
            patch.object(sys, "argv", ["probe"]),
            patch.object(
                probe, "default_llvm", side_effect=RuntimeError("discovery failed")
            ),
            patch.object(probe, "probe") as run,
            redirect_stderr(diagnostic),
            self.assertRaises(SystemExit) as result,
        ):
            probe.main()
        self.assertEqual(result.exception.code, 2)
        self.assertIn("discovery failed", diagnostic.getvalue())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
