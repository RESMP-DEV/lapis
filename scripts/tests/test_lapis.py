"""Behavioral regressions for the lapis launcher wrapper."""

import io
import json
import os
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
            patch.object(lapis, "DEFAULT_SOCKET", self.runtime / "desktop-v4.sock"),
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

    def test_explicit_socket_option_is_preserved(self):
        socket = self.temporary_root / "explicit.sock"
        _, run = self.launch(["--socket", str(socket)])

        command = run.call_args.args[0]
        self.assertIn("--socket", command)
        self.assertIn(str(socket), command)
        self.assertEqual(
            command.count("--socket")
            + sum(str(part).startswith("--socket=") for part in command),
            1,
        )
        self.assertFalse(self.runtime.exists())

    def test_explicit_socket_assignment_is_preserved(self):
        socket = self.temporary_root / "explicit.sock"
        _, run = self.launch([f"--socket={socket}"])

        command = run.call_args.args[0]
        self.assertIn(f"--socket={socket}", command)
        self.assertNotIn("--socket", command)
        self.assertFalse(self.runtime.exists())

    def test_ui_preview_does_not_inject_a_socket(self):
        _, run = self.launch(["--ui-preview"])

        command = run.call_args.args[0]
        self.assertNotIn("--socket", command)
        self.assertFalse(any(str(part).startswith("--socket=") for part in command))
        self.assertFalse(self.runtime.exists())

    def test_default_live_shell_injects_private_socket(self):
        _, run = self.launch([])

        command = run.call_args.args[0]
        self.assertIn("--socket", command)
        self.assertIn(str(self.runtime / "desktop-v4.sock"), command)
        self.assertEqual(self.runtime.stat().st_mode & 0o777, 0o700)

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
