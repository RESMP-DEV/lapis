"""Behavioral regressions for the lapis launcher wrapper."""

import io
import json
import os
import stat
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from scripts import check_ui_preview, lapis


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

    def launch(self, arguments, result_code=0, platform="darwin", environment=None):
        result = SimpleNamespace(returncode=result_code)
        with (
            patch.object(lapis.sys, "platform", platform),
            patch.dict(
                "os.environ",
                {"LAPIS_GHOSTTY_PREFIX": str(self.ghostty), **(environment or {})},
                clear=True,
            ),
            patch.object(lapis, "desktop_binary_path", return_value=self.desktop),
            patch.object(lapis, "RUNTIME_DIR", self.runtime),
            patch.object(lapis, "DEFAULT_SOCKET", self.runtime / "desktop-v6.sock"),
            patch.object(lapis, "_run", return_value=result) as run,
        ):
            code = lapis.launch(arguments)
        return code, run

    def test_platform_binary_paths(self):
        for platform, suffix in (
            ("darwin", "lapis_desktop.app/Contents/MacOS/lapis_desktop"),
            ("linux", "lapis_desktop"),
        ):
            with (
                self.subTest(platform=platform),
                patch.object(lapis.sys, "platform", platform),
            ):
                expected = lapis.DESKTOP_BUILD / "apps/desktop" / suffix
                self.assertEqual(lapis.desktop_binary_path(), expected)
                self.assertEqual(check_ui_preview.desktop_binary_path(), expected)

    def test_platform_screen_defaults_and_overrides(self):
        for platform, expected in (("darwin", None), ("linux", None)):
            for environment, arguments, selected in (
                ({}, ["--ui-preview"], expected),
                ({"LAPIS_SCREEN": "DP-1"}, ["--ui-preview"], "DP-1"),
                ({"LAPIS_SCREEN": ""}, ["--ui-preview"], None),
                (
                    {"LAPIS_SCREEN": "DP-1"},
                    ["--ui-preview", "--screen", "HDMI-1"],
                    "HDMI-1",
                ),
            ):
                with self.subTest(
                    platform=platform, arguments=arguments, environment=environment
                ):
                    _, run = self.launch(
                        arguments, platform=platform, environment=environment
                    )
                    command = run.call_args.args[0]
                    if selected is None:
                        self.assertNotIn("--screen", command)
                    else:
                        self.assertEqual(command.count("--screen"), 1)
                        self.assertEqual(
                            command[command.index("--screen") + 1], selected
                        )

    def test_program_arguments_do_not_override_app_options(self):
        args = ["--", "program", "--socket=child.sock", "--screen=child"]
        _, run = self.launch(args)
        command = run.call_args.args[0]
        self.assertEqual(command[-len(args) :], args)
        self.assertNotIn("--screen", command[: command.index("--")])
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

    def test_explicit_development_shell_injects_private_socket(self):
        _, run = self.launch(["--development-shell"])

        command = run.call_args.args[0]
        self.assertIn("--socket", command)
        self.assertIn(str(self.runtime / "desktop-v6.sock"), command)
        self.assertEqual(self.runtime.stat().st_mode & 0o777, 0o700)

    def test_normal_workspace_does_not_inject_shell_socket_or_display(self):
        _, run = self.launch([])
        command = run.call_args.args[0]
        self.assertNotIn("--socket", command)
        self.assertNotIn("--screen", command)
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
            patch.object(lapis.sys, "platform", "darwin"),
            patch.dict(
                "os.environ", {"LAPIS_GHOSTTY_PREFIX": str(self.ghostty)}, clear=True
            ),
            patch.object(lapis, "desktop_binary_path", return_value=self.desktop),
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
            patch.object(lapis, "desktop_binary_path", return_value=self.desktop),
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
            patch.object(
                lapis, "desktop_binary_path", return_value=self.root / "missing-desktop"
            ),
            patch.object(lapis, "RUNTIME_DIR", self.root / "missing-runtime"),
            patch.object(lapis.subprocess, "run", return_value=qmake),
            redirect_stdout(output),
        ):
            code = lapis.command_doctor()

        self.assertEqual(code, 1)
        self.assertIn("one-line failure", output.getvalue())


class LinuxGuiTests(unittest.TestCase):
    def test_rejects_non_linux_missing_tools_and_missing_software_driver(self):
        for platform, tools, diagnostic in (
            ("darwin", {}, "requires Linux"),
            ("linux", {"xvfb-run": "/bin/xvfb-run"}, "requires xvfb-run and openbox"),
            ("linux", {"openbox": "/bin/openbox"}, "requires xvfb-run and openbox"),
            (
                "linux",
                {"xvfb-run": "/bin/xvfb-run", "openbox": "/bin/openbox"},
                "plus xprop",
            ),
            (
                "linux",
                {
                    "xvfb-run": "/bin/xvfb-run",
                    "openbox": "/bin/openbox",
                    "xprop": "/bin/xprop",
                },
                "requires the Mesa lavapipe",
            ),
        ):
            with (
                self.subTest(platform=platform, tools=tools),
                patch.object(lapis.sys, "platform", platform),
                patch.object(lapis.shutil, "which", side_effect=tools.get),
                patch.object(lapis, "LAVAPIPE_ICDS", ()),
                patch.object(lapis, "_run") as run,
                self.assertRaisesRegex(lapis.SetupError, diagnostic),
            ):
                lapis.command_linux_gui([])
            run.assert_not_called()

    def test_isolation_literal_argv_exit_status_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            icd = Path(directory) / "lvp_icd.json"
            icd.touch()
            for arguments in (
                [],
                ["ctest", "--test-dir", "path with spaces", "$(false)"],
            ):
                observed = []

                def run(command, **kwargs):
                    env = kwargs["env"]
                    runtime = Path(env["XDG_RUNTIME_DIR"])
                    observed.append(runtime)
                    self.assertEqual(stat.S_IMODE(runtime.stat().st_mode), 0o700)
                    self.assertEqual(env["QT_QPA_PLATFORM"], "xcb")
                    self.assertEqual(env["QT_IM_MODULE"], "compose")
                    self.assertEqual(env["LAPIS_SCREEN"], "")
                    self.assertEqual(env["VK_DRIVER_FILES"], str(icd))
                    self.assertEqual(env["VK_ICD_FILENAMES"], str(icd))
                    self.assertEqual(env["LIBGL_ALWAYS_SOFTWARE"], "1")
                    self.assertEqual(env["GALLIUM_DRIVER"], "llvmpipe")
                    self.assertFalse(kwargs["check"])
                    expected = arguments or [
                        lapis.sys.executable,
                        str(Path(lapis.__file__).resolve()),
                        "build",
                    ]
                    self.assertEqual(command[-len(expected) :], expected)
                    self.assertNotIn("$(false)", command[5])
                    return SimpleNamespace(returncode=37)

                with (
                    self.subTest(arguments=arguments),
                    patch.object(lapis.sys, "platform", "linux"),
                    patch.object(
                        lapis.shutil, "which", side_effect=lambda x: f"/bin/{x}"
                    ),
                    patch.object(lapis, "LAVAPIPE_ICDS", (icd,)),
                    patch.object(lapis, "environment", return_value={}),
                    patch.dict(
                        os.environ,
                        {
                            "VK_DRIVER_FILES": "/hardware.json",
                            "QT_QPA_PLATFORM": "wayland",
                            "LAPIS_SCREEN": "built-in" if not arguments else "DP-1",
                        },
                        clear=True,
                    ),
                    patch.object(lapis, "_run", side_effect=run),
                ):
                    self.assertEqual(lapis.command_linux_gui(arguments), 37)
                    self.assertEqual(os.environ["VK_DRIVER_FILES"], "/hardware.json")
                self.assertFalse(observed[0].exists())

    def test_runtime_cleanup_on_launch_failure(self):
        observed = []

        def failed_run(_command, **kwargs):
            observed.append(Path(kwargs["env"]["XDG_RUNTIME_DIR"]))
            raise OSError("failed launch")

        with (
            patch.object(lapis.sys, "platform", "linux"),
            patch.object(lapis.shutil, "which", side_effect=lambda x: f"/bin/{x}"),
            patch.object(lapis, "LAVAPIPE_ICDS", (Path(__file__),)),
            patch.object(lapis, "environment", return_value={}),
            patch.object(lapis, "_run", side_effect=failed_run),
            self.assertRaisesRegex(OSError, "failed launch"),
        ):
            lapis.command_linux_gui(["ctest"])
        self.assertFalse(observed[0].exists())

    def test_window_manager_readiness_and_failure_cleanup_without_a_display(self):
        for scenario in ("ready", "early-exit", "timeout"):
            with (
                self.subTest(scenario=scenario),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                wm = root / "openbox"
                wm.write_text(
                    '#!/bin/sh\necho $$ > "$XDG_RUNTIME_DIR/wm.pid"\n'
                    + (
                        "exit 9\n"
                        if scenario == "early-exit"
                        else "exec /bin/sleep 60\n"
                    )
                )
                wm.chmod(0o700)
                xprop = root / "xprop"
                xprop.write_text(
                    '#!/bin/sh\n[ "$1" = -root ] && [ "$2" = _NET_SUPPORTING_WM_CHECK ] || exit 2\n'
                    'echo call >> "$XDG_RUNTIME_DIR/xprop-calls"\n'
                    + (
                        '[ -f "$XDG_RUNTIME_DIR/wm.pid" ] || exit 1\n'
                        "echo '_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x123'\n"
                        if scenario == "ready"
                        else "exit 1\n"
                    )
                )
                xprop.chmod(0o700)
                sleep = root / "sleep"
                sleep.write_text("#!/bin/sh\nexit 0\n")
                sleep.chmod(0o700)
                marker = root / "command-argv.json"
                results = []

                def run(command, **kwargs):
                    # Replace only Xvfb with the real shell and fake local tools.
                    result = subprocess.run(
                        command[3:],
                        env={
                            **kwargs["env"],
                            "PATH": str(root) + os.pathsep + os.environ.get("PATH", ""),
                        },
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=8,
                    )
                    results.append(result)
                    pid = int(
                        (Path(kwargs["env"]["XDG_RUNTIME_DIR"]) / "wm.pid").read_text()
                    )
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                    if scenario == "timeout":
                        calls = Path(kwargs["env"]["XDG_RUNTIME_DIR"]) / "xprop-calls"
                        self.assertEqual(len(calls.read_text().splitlines()), 50)
                    return result

                tools = {
                    "xvfb-run": "/unused/xvfb-run",
                    "openbox": str(wm),
                    "xprop": str(xprop),
                }
                with (
                    patch.object(lapis.sys, "platform", "linux"),
                    patch.object(lapis.shutil, "which", side_effect=tools.get),
                    patch.object(lapis, "LAVAPIPE_ICDS", (Path(__file__),)),
                    patch.object(lapis, "environment", return_value={}),
                    patch.object(lapis, "_run", side_effect=run),
                ):
                    code = lapis.command_linux_gui(
                        [
                            lapis.sys.executable,
                            "-c",
                            "import json,sys; from pathlib import Path; Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:])); sys.exit(37)",
                            str(marker),
                            "space argument",
                            "$(false)",
                        ]
                    )
                if scenario == "ready":
                    self.assertEqual(code, 37)
                    self.assertEqual(
                        json.loads(marker.read_text()), ["space argument", "$(false)"]
                    )
                else:
                    self.assertEqual(code, 1)
                    self.assertFalse(marker.exists())
                    self.assertIn(
                        "exited before becoming ready"
                        if scenario == "early-exit"
                        else "did not become ready",
                        results[0].stderr,
                    )


class LinuxLlvmTests(unittest.TestCase):
    def test_complete_linux_bundle_and_caller_override(self):
        for platform, configured, missing, expected in (
            ("linux", {}, None, {"LAPIS_LLVM_BIN": "/usr/lib/llvm-22/bin"}),
            ("linux", {}, "clangd", {}),
            ("linux", {"LAPIS_LLVM_BIN": "/custom/llvm"}, None, {}),
            ("darwin", {}, None, {}),
        ):
            with (
                self.subTest(platform=platform, configured=configured, missing=missing),
                patch.object(lapis.sys, "platform", platform),
                patch.dict(os.environ, configured, clear=True),
                patch.object(Path, "is_file", return_value=True),
                patch.object(
                    lapis.os,
                    "access",
                    side_effect=lambda path, _mode: path.name != missing,
                ),
            ):
                self.assertEqual(lapis.llvm_environment(), expected)

    def test_bootstrap_child_gets_compiler_without_resolving_ghostty(self):
        with (
            patch.object(lapis.sys, "platform", "linux"),
            patch.dict(os.environ, {}, clear=True),
            patch.object(Path, "is_file", return_value=True),
            patch.object(lapis.os, "access", return_value=True),
            patch.object(
                lapis,
                "ghostty_prefix",
                side_effect=AssertionError("must not resolve Ghostty"),
            ),
            patch.object(
                lapis.subprocess, "run", return_value=SimpleNamespace(returncode=9)
            ) as run,
        ):
            self.assertEqual(lapis.command_bootstrap(), 9)
            self.assertEqual(
                run.call_args.kwargs["env"]["LAPIS_LLVM_BIN"], "/usr/lib/llvm-22/bin"
            )
            self.assertNotIn("LAPIS_LLVM_BIN", os.environ)

    def test_environment_selects_bundle_and_child_preserves_explicit_compiler(self):
        with (
            patch.object(lapis.sys, "platform", "linux"),
            patch.object(lapis, "ghostty_prefix", return_value=Path("/vt")),
            patch.object(Path, "is_file", return_value=True),
            patch.object(lapis.os, "access", return_value=True),
            patch.dict(os.environ, {}, clear=True),
        ):
            self.assertEqual(
                lapis.environment()["LAPIS_LLVM_BIN"], "/usr/lib/llvm-22/bin"
            )
            with (
                patch.dict(os.environ, {"LAPIS_LLVM_BIN": "/custom/llvm"}),
                patch.object(
                    lapis.subprocess, "run", return_value=SimpleNamespace(returncode=0)
                ) as run,
            ):
                lapis.run_python_script("check_quality.py", [], needs_ghostty=False)
                self.assertEqual(
                    run.call_args.kwargs["env"]["LAPIS_LLVM_BIN"], "/custom/llvm"
                )


class LinuxQtTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sdk = self.root / "build/deps/qt/6.11.2/gcc_64"
        config = self.sdk / "lib/cmake/Qt6/Qt6Config.cmake"
        config.parent.mkdir(parents=True)
        config.touch()
        self.qmake = self.sdk / "bin/qmake"
        self.qmake.parent.mkdir()
        self.qmake.touch(mode=0o700)

    def test_build_child_discovers_sdk_preserving_explicit_precedence(self):
        for configured in ("", "/custom/qt:/other/sdk", str(self.sdk)):
            with (
                self.subTest(configured=configured),
                patch.object(lapis, "ROOT", self.root),
                patch.object(lapis.sys, "platform", "linux"),
                patch.object(lapis, "ghostty_prefix", return_value=self.root / "vt"),
                patch.dict(os.environ, {"CMAKE_PREFIX_PATH": configured}, clear=True),
                patch.object(
                    lapis.subprocess, "run", return_value=SimpleNamespace(returncode=0)
                ) as run,
            ):
                self.assertEqual(lapis.COMMANDS["build"]([]), 0)
                prefixes = run.call_args.kwargs["env"]["CMAKE_PREFIX_PATH"].split(":")
                self.assertEqual(prefixes.count(str(self.sdk)), 1)
                self.assertEqual(prefixes[-1], str(self.sdk))
                if configured and configured != str(self.sdk):
                    self.assertEqual(prefixes[:-1], configured.split(":"))
                self.assertEqual(os.environ["CMAKE_PREFIX_PATH"], configured)

    def test_missing_sdk_and_macos_do_not_inject_prefix(self):
        for platform in ("darwin", "linux"):
            with (
                self.subTest(platform=platform),
                patch.object(lapis, "ROOT", self.root),
                patch.object(lapis.sys, "platform", platform),
                patch.object(lapis, "ghostty_prefix", return_value=self.root / "vt"),
            ):
                if platform == "linux":
                    (self.sdk / "lib/cmake/Qt6/Qt6Config.cmake").unlink()
                self.assertNotIn("CMAKE_PREFIX_PATH", lapis.environment())

    def test_doctor_queries_explicit_sdk_before_local_and_path(self):
        explicit = self.root / "explicit"
        explicit_qmake = explicit / "bin/qmake"
        explicit_qmake.parent.mkdir(parents=True)
        explicit_qmake.touch(mode=0o700)
        for configured, expected in (("", self.qmake), (str(explicit), explicit_qmake)):
            with (
                self.subTest(configured=configured),
                patch.object(lapis, "ROOT", self.root),
                patch.object(lapis.sys, "platform", "linux"),
                patch.dict(os.environ, {"CMAKE_PREFIX_PATH": configured}, clear=True),
                patch.object(lapis, "ghostty_prefix", return_value=self.root / "vt"),
                patch.object(lapis, "desktop_binary_path", return_value=self.qmake),
                patch.object(lapis, "RUNTIME_DIR", self.root / "runtime"),
                patch.object(lapis.shutil, "which", return_value="/usr/bin/qmake6"),
                patch.object(
                    lapis.subprocess,
                    "run",
                    return_value=SimpleNamespace(stdout="6.11.2"),
                ) as run,
                redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(lapis.command_doctor(), 0)
                self.assertEqual(
                    run.call_args.args[0], [str(expected), "-query", "QT_VERSION"]
                )

    def test_doctor_path_fallback_prefers_qmake6_only_on_linux(self):
        for platform, available, expected in (
            ("linux", {"qmake6": "/bin/qmake6", "qmake": "/bin/qmake"}, "/bin/qmake6"),
            ("linux", {"qmake": "/bin/qmake"}, "/bin/qmake"),
            ("darwin", {"qmake6": "/bin/qmake6", "qmake": "/bin/qmake"}, "/bin/qmake"),
        ):
            with (
                self.subTest(platform=platform, available=available),
                patch.object(lapis, "ROOT", self.root / "absent"),
                patch.object(lapis.sys, "platform", platform),
                patch.dict(os.environ, {}, clear=True),
                patch.object(lapis.shutil, "which", side_effect=available.get),
            ):
                self.assertEqual(lapis.qt_qmake(), expected)


if __name__ == "__main__":
    unittest.main()
