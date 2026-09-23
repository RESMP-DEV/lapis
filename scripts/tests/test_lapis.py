"""Behavioral regressions for the lapis launcher wrapper."""

import io
import json
import os
import stat
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from scripts import lapis


def write_receipt(prefix):
    library = prefix / "lib/libghostty-vt.a"
    library.parent.mkdir(parents=True)
    library.touch()
    receipt = prefix.parent / "reports/receipt.json"
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(
        json.dumps(
            {
                "engine": "ghostty",
                "passed": True,
                "sources": json.loads(
                    (
                        lapis.ROOT / "tools/terminal_probe/ghostty/sources.json"
                    ).read_text()
                ),
            }
        )
    )


class LauncherTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.temporary_root = Path(temporary.name)
        self.runtime = self.temporary_root / "runtime"
        self.ghostty = self.temporary_root / "ghostty-prefix"
        self.ghostty_header = self.ghostty / "include" / "ghostty" / "vt.h"
        self.ghostty_header.parent.mkdir(parents=True)
        self.ghostty_header.touch()
        write_receipt(self.ghostty)
        self.desktop = self.temporary_root / "lapis_desktop"
        self.desktop.touch()

    def launch(self, arguments, result_code=0):
        result = SimpleNamespace(returncode=result_code)
        with (
            patch.object(lapis.sys, "platform", "darwin"),
            patch.dict(
                "os.environ", {"LAPIS_GHOSTTY_PREFIX": str(self.ghostty)}, clear=True
            ),
            patch.object(lapis, "DESKTOP_BINARY", self.desktop),
            patch.object(lapis, "RUNTIME_DIR", self.runtime),
            patch.object(lapis, "DEFAULT_SOCKET", self.runtime / "desktop-v6.sock"),
            patch.object(lapis, "_run", return_value=result) as run,
        ):
            code = lapis.launch(arguments)
        return code, run

    def test_program_arguments_do_not_override_app_options(self):
        args = ["--", "program", "--socket=child.sock", "--screen=child"]
        _, run = self.launch(args)
        command = run.call_args.args[0]
        self.assertEqual(command[-len(args) :], args)
        self.assertIn("--screen", command[: command.index("--")])
        self.assertNotIn("--socket", command[: command.index("--")])
        self.assertFalse(self.runtime.exists())

    def test_bootstrap_does_not_require_existing_ghostty(self):
        with (
            patch.object(
                lapis, "ghostty_prefix", side_effect=lapis.SetupError("missing")
            ),
            patch.object(
                lapis.subprocess, "run", return_value=SimpleNamespace(returncode=9)
            ) as run,
        ):
            self.assertEqual(lapis.command_bootstrap(), 9)
        self.assertIn("probe_terminal.py", run.call_args.args[0][1])

    def test_explicit_socket_argument_is_preserved(self):
        socket = self.temporary_root / "explicit.sock"
        cases = [
            ["--socket", str(socket)],
            [f"--socket={socket}"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                _, run = self.launch(arguments)

                command = run.call_args.args[0]
                self.assertEqual(command[-len(arguments) :], arguments)
                self.assertEqual(
                    command.count("--socket")
                    + sum(str(part).startswith("--socket=") for part in command),
                    1,
                )
                self.assertFalse(self.runtime.exists())

    def test_ui_preview_does_not_inject_a_socket(self):
        _, run = self.launch(["--ui-preview"])

        command = run.call_args.args[0]
        self.assertNotIn("--socket", command)
        self.assertFalse(any(str(part).startswith("--socket=") for part in command))
        self.assertFalse(self.runtime.exists())

    def test_default_live_shell_injects_private_workspace(self):
        _, run = self.launch([])

        command = run.call_args.args[0]
        workspace = self.runtime / "workspace-v1.json"
        self.assertIn("--workspace", command)
        self.assertIn(str(workspace), command)
        self.assertEqual(self.runtime.stat().st_mode & 0o777, 0o700)

    def test_explicit_session_arguments_inject_private_socket(self):
        cases = [
            ["--new-session"],
            ["--discover"],
            ["--cwd", str(self.temporary_root)],
            [f"--cwd={self.temporary_root}"],
            ["--codex"],
            ["--claude"],
            ["--smoke-input"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                _, run = self.launch(arguments)

                command = run.call_args.args[0]
                self.assertIn("--socket", command)
                self.assertIn(str(self.runtime / "desktop-v6.sock"), command)
                self.assertNotIn("--workspace", command)
                self.assertEqual(self.runtime.stat().st_mode & 0o777, 0o700)

    def test_bare_positional_program_injects_private_socket(self):
        cases = [
            ["zsh"],
            ["--screen", "built-in", "zsh"],
            ["--screen=built-in", "zsh"],
            ["--capture", str(self.temporary_root / "capture.png"), "zsh"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                _, run = self.launch(arguments)

                command = run.call_args.args[0]
                self.assertIn("--socket", command)
                self.assertIn(str(self.runtime / "desktop-v6.sock"), command)
                self.assertNotIn("--workspace", command)
                self.assertEqual(self.runtime.stat().st_mode & 0o777, 0o700)

    def test_value_taking_options_do_not_look_like_programs(self):
        screen = str(self.temporary_root / "screen")
        cases = [
            ["--screen", screen],
            ["--qml", str(self.temporary_root / "Main.qml")],
            ["--scenario", "arrival"],
            ["--capture", str(self.temporary_root / "capture.png")],
            ["--capture-delay", "500"],
            ["--trace", str(self.temporary_root / "trace.json")],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                _, run = self.launch(arguments)

                command = run.call_args.args[0]
                self.assertIn("--workspace", command)
                self.assertNotIn("--socket", command)

    def test_explicit_workspace_argument_is_preserved(self):
        workspace = self.temporary_root / "explicit-workspace.json"
        cases = [
            ["--workspace", str(workspace)],
            [f"--workspace={workspace}"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                _, run = self.launch(arguments)

                command = run.call_args.args[0]
                self.assertEqual(command[-len(arguments) :], arguments)
                self.assertEqual(
                    command.count("--workspace")
                    + sum(str(part).startswith("--workspace=") for part in command),
                    1,
                )
                self.assertFalse(self.runtime.exists())

    def test_new_runtime_directory_is_private(self):
        self.assertFalse(self.runtime.exists())

        with patch.object(lapis, "RUNTIME_DIR", self.runtime):
            self.assertEqual(lapis.private_runtime_dir(), self.runtime)

        runtime_stat = os.lstat(self.runtime)
        self.assertTrue(stat.S_ISDIR(runtime_stat.st_mode))
        self.assertEqual(runtime_stat.st_uid, os.geteuid())
        self.assertEqual(stat.S_IMODE(runtime_stat.st_mode), 0o700)

    def test_private_existing_runtime_directory_is_preserved(self):
        self.runtime.mkdir(mode=0o700)
        marker = self.runtime / ".private"
        marker.write_text("kept")
        before = os.lstat(self.runtime)

        with patch.object(lapis, "RUNTIME_DIR", self.runtime):
            self.assertEqual(lapis.private_runtime_dir(), self.runtime)

        self.assertEqual(os.lstat(self.runtime), before)
        self.assertEqual(marker.read_text(), "kept")

    def test_unsafe_runtime_permissions_are_rejected_without_repair(self):
        for mode in (0o750, 0o701, 0o777, 0o500, 0o1700):
            with self.subTest(mode=oct(mode)):
                self.runtime.mkdir(mode=mode)
                os.chmod(self.runtime, mode)

                with (
                    patch.object(lapis, "RUNTIME_DIR", self.runtime),
                    patch.object(lapis.os, "chmod") as chmod,
                    self.assertRaisesRegex(lapis.SetupError, "must have mode 0700"),
                ):
                    lapis.private_runtime_dir()

                self.assertEqual(stat.S_IMODE(os.lstat(self.runtime).st_mode), mode)
                chmod.assert_not_called()
                self.runtime.rmdir()

    def test_runtime_symlink_is_rejected_without_changing_target(self):
        target = self.temporary_root / "runtime-target"
        target.mkdir(mode=0o750)
        os.chmod(target, 0o750)
        self.runtime.symlink_to(target, target_is_directory=True)

        with (
            patch.object(lapis, "RUNTIME_DIR", self.runtime),
            patch.object(lapis.os, "chmod") as chmod,
            self.assertRaisesRegex(lapis.SetupError, "not a directory"),
        ):
            lapis.private_runtime_dir()

        self.assertTrue(self.runtime.is_symlink())
        self.assertEqual(stat.S_IMODE(os.lstat(target).st_mode), 0o750)
        chmod.assert_not_called()

    def test_runtime_non_directory_and_wrong_owner_are_rejected(self):
        self.runtime.write_text("not a directory")
        with (
            patch.object(lapis, "RUNTIME_DIR", self.runtime),
            self.assertRaisesRegex(lapis.SetupError, "not a directory"),
        ):
            lapis.private_runtime_dir()

        owner_mode = stat.S_IFDIR | 0o700
        wrong_owner = os.stat_result(
            (owner_mode, 1, 2, 3, os.geteuid() + 1, os.getegid(), 7, 8, 9, 10)
        )
        with (
            patch.object(lapis, "RUNTIME_DIR", self.runtime),
            patch.object(lapis.os, "lstat", return_value=wrong_owner),
            self.assertRaisesRegex(lapis.SetupError, "owned by UID"),
        ):
            lapis.private_runtime_dir()

    def test_runtime_creation_failure_becomes_setup_error(self):
        blocked_parent = self.temporary_root / "blocked-parent"
        blocked_parent.write_text("not a directory")
        blocked_runtime = blocked_parent / "runtime"
        with (
            patch.object(lapis, "RUNTIME_DIR", blocked_runtime),
            self.assertRaisesRegex(
                lapis.SetupError, "Cannot create or inspect private runtime directory"
            ),
        ):
            lapis.private_runtime_dir()

    def test_child_exit_code_propagates_without_checking(self):
        code, run = self.launch(["--ui-preview"], result_code=42)

        self.assertEqual(code, 42)
        self.assertFalse(run.call_args.kwargs["check"])

    def test_debugger_child_exit_code_propagates_without_checking(self):
        result = SimpleNamespace(returncode=7)
        with (
            patch.dict(
                "os.environ", {"LAPIS_GHOSTTY_PREFIX": str(self.ghostty)}, clear=True
            ),
            patch.object(lapis, "DESKTOP_BINARY", self.desktop),
            patch.object(lapis, "_run", return_value=result) as run,
        ):
            code = lapis.command_ui_debug([])

        self.assertEqual(code, 7)
        self.assertFalse(run.call_args.kwargs["check"])

    def test_quality_dispatcher_does_not_resolve_ghostty(self):
        with patch.object(lapis, "run_python_script", return_value=0) as run:
            self.assertEqual(lapis.COMMANDS["quality"](["--fast"]), 0)

        run.assert_called_once_with("check_quality.py", ["--fast"], needs_ghostty=False)

    def test_doctor_checks_runtime_without_creating_or_repairing_it(self):
        with (
            patch.object(lapis, "RUNTIME_DIR", self.runtime),
            patch.object(lapis, "DESKTOP_BINARY", self.desktop),
            patch.object(lapis, "ghostty_prefix", return_value=self.ghostty),
            patch.object(lapis.shutil, "which", return_value="qmake"),
            patch.object(
                lapis.subprocess, "run", return_value=SimpleNamespace(stdout="6.11.2")
            ),
            redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(lapis.command_doctor(), 0)
            self.assertFalse(self.runtime.exists())
            self.runtime.mkdir(mode=0o700)
            self.assertEqual(lapis.command_doctor(), 0)
            with patch.object(lapis.os, "geteuid", return_value=os.geteuid() + 1):
                self.assertEqual(lapis.command_doctor(), 1)
            os.chmod(self.runtime, 0o1700)
            self.assertEqual(lapis.command_doctor(), 1)
            self.assertEqual(stat.S_IMODE(os.lstat(self.runtime).st_mode), 0o1700)
            self.runtime.rmdir()
            self.runtime.symlink_to(self.ghostty, target_is_directory=True)
            self.assertEqual(lapis.command_doctor(), 1)
            self.assertTrue(self.runtime.is_symlink())
            self.assertIn("not a directory", output.getvalue())


class GhosttyPrefixTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.runs = self.root / "runs"

    def prefix(self, name, mtime=None):
        prefix = self.runs / name / "prefix"
        header = prefix / "include" / "ghostty" / "vt.h"
        header.parent.mkdir(parents=True)
        header.touch()
        write_receipt(prefix)
        if mtime is not None:
            os.utime(prefix.parent / "reports/receipt.json", (mtime, mtime))
        return prefix

    def test_selects_newest_prefix_with_verified_header(self):
        self.prefix("lexically-newer", mtime=100)
        current = self.prefix("lexically-earlier", mtime=200)
        self.runs.joinpath("newest-without-header", "prefix").mkdir(parents=True)

        with (
            patch.dict("os.environ", {}, clear=True),
            patch.object(lapis, "GHOSTTY_RUNS", self.runs),
        ):
            self.assertEqual(lapis.ghostty_prefix(), current)

    def test_skips_failed_or_incomplete_builds(self):
        good = self.prefix("good", 100)
        failed = self.prefix("failed", 200)
        (failed.parent / "reports/receipt.json").write_text(
            '{"engine":"ghostty","passed":false}'
        )
        incomplete = self.prefix("incomplete", 300)
        (incomplete / "lib/libghostty-vt.a").unlink()
        with (
            patch.dict("os.environ", {}, clear=True),
            patch.object(lapis, "GHOSTTY_RUNS", self.runs),
        ):
            self.assertEqual(lapis.ghostty_prefix(), good)
        with (
            patch.dict(
                "os.environ", {"LAPIS_GHOSTTY_PREFIX": str(incomplete)}, clear=True
            ),
            self.assertRaises(lapis.SetupError),
        ):
            lapis.ghostty_prefix()

    def test_doctor_reports_single_line_setup_errors(self):
        output = io.StringIO()
        qmake = SimpleNamespace(stdout="")
        with (
            patch.object(
                lapis,
                "ghostty_prefix",
                side_effect=lapis.SetupError("one-line failure"),
            ),
            patch.object(lapis, "DESKTOP_BINARY", self.root / "missing-desktop"),
            patch.object(lapis, "RUNTIME_DIR", self.root / "missing-runtime"),
            patch.object(lapis.subprocess, "run", return_value=qmake),
            redirect_stdout(output),
        ):
            code = lapis.command_doctor()

        self.assertEqual(code, 1)
        self.assertIn("one-line failure", output.getvalue())


if __name__ == "__main__":
    unittest.main()
