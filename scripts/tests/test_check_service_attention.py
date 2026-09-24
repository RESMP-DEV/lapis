"""Behavioral regressions for the managed-service attention stream."""

import contextlib
import io
import json
import socket
import tempfile
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from check_cli_launch import ATTENTION_RETRY, CheckError, WireClient, frame
from check_service_attention import (
    View,
    cleanup_service,
    exercise,
    fixture_options,
    fixture_trust_prompt,
    initialize_fixture_owner,
    parse_args,
    question_turn,
    verify_runtime_model,
    selected_question_answers,
)


class AuthenticationPreflightTests(unittest.IsolatedAsyncioTestCase):
    async def test_account_is_checked_without_refresh_before_input(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            owner = SimpleNamespace(
                rpc=AsyncMock(return_value={"account": {"type": "chatgpt"}})
            )
            with patch(
                "check_service_attention.initialize",
                AsyncMock(return_value={"codexHome": str(home)}),
            ):
                await initialize_fixture_owner(owner, home, True)
            owner.rpc.assert_awaited_once_with("account/read", {"refreshToken": False})

    async def test_wrong_home_and_missing_account_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            owner = SimpleNamespace(rpc=AsyncMock(return_value={"account": None}))
            with patch(
                "check_service_attention.initialize",
                AsyncMock(return_value={"codexHome": str(home)}),
            ):
                with self.assertRaisesRegex(CheckError, "will not start a login flow"):
                    await initialize_fixture_owner(owner, home, True)
            owner.rpc.reset_mock()
            with patch(
                "check_service_attention.initialize",
                AsyncMock(return_value={"codexHome": str(home / "wrong")}),
            ):
                with self.assertRaisesRegex(CheckError, "private fixture home"):
                    await initialize_fixture_owner(owner, home, True)
            owner.rpc.assert_not_called()


class OnboardingTests(unittest.TestCase):
    def test_only_disposable_directory_trust_is_acknowledged(self):
        directory = Path("/private/fixture/cwd")
        prompt = f"{directory}\nDo you trust the contents of this directory?\nYes, continue\nPress enter to continue"
        self.assertTrue(fixture_trust_prompt(prompt, directory))
        self.assertFalse(fixture_trust_prompt(prompt, Path("/other/project")))
        self.assertFalse(
            fixture_trust_prompt("Welcome to Codex\nPress enter to continue", directory)
        )

    def test_login_menu_never_receives_trust_confirmation(self):
        for label in (
            "Sign in with ChatGPT",
            "Finish signing in",
            "Sign in with Device Code",
        ):
            with (
                self.subTest(label=label),
                self.assertRaisesRegex(CheckError, "will not start a login flow"),
            ):
                fixture_trust_prompt(
                    label + "\nPress enter to continue", Path("/private/fixture/cwd")
                )


class ProviderOptionsTests(unittest.TestCase):
    def test_live_modes_are_explicit_and_exclusive(self):
        defaults = parse_args([])
        self.assertFalse(defaults.live_openai or defaults.live_glm)
        self.assertEqual(parse_args(["--live-openai"]).model, "gpt-6-astra")
        self.assertEqual(parse_args(["--live-glm"]).model, "zai,glm-5.3")
        custom = parse_args(["--live-openai", "--model", "candidate", "--desktop"])
        self.assertEqual(custom.model, "candidate")
        self.assertTrue(custom.desktop)
        for arguments in [
            ["--live-openai", "--live-glm"],
            ["--desktop"],
            ["--model", "candidate"],
            ["--live-glm", "--model", "candidate"],
            ["--live-openai", "--model", " "],
        ]:
            with (
                self.subTest(arguments=arguments),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                with self.assertRaises(SystemExit):
                    parse_args(arguments)

    def test_openai_uses_private_auth_link_without_reading_credentials(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            user = root / "user"
            (user / ".codex").mkdir(parents=True)
            auth = user / ".codex" / "auth.json"
            auth.write_text("not a real credential")
            runtime = root / "private"
            (runtime / "home").mkdir(parents=True)
            args = parse_args(["--live-openai"])
            with (
                patch("check_service_attention.Path.home", return_value=user),
                patch.object(
                    Path,
                    "read_bytes",
                    side_effect=AssertionError("credentials must not be read"),
                ),
                patch.object(
                    Path,
                    "read_text",
                    side_effect=AssertionError("credentials must not be read"),
                ),
            ):
                options = fixture_options(args, runtime)
            link = runtime / "home" / "auth.json"
            self.assertTrue(link.is_symlink())
            self.assertEqual(link.readlink(), auth)
            self.assertEqual(auth.read_text(), "not a real credential")
            config = dict(option.split("=", 1) for option in options[1::2])
            self.assertEqual(json.loads(config["model_provider"]), "openai")
            self.assertEqual(json.loads(config["model"]), "gpt-6-astra")
            self.assertEqual(json.loads(config["cli_auth_credentials_store"]), "file")
            self.assertEqual(json.loads(config["forced_login_method"]), "chatgpt")
            self.assertNotIn("model_providers.lapis_probe.base_url", config)
            for key in [
                "features.apps",
                "features.plugins",
                "agents.enabled",
                "features.multi_agent",
                "features.multi_agent_v2",
                "analytics.enabled",
            ]:
                self.assertFalse(json.loads(config[key]))

    def test_missing_auth_fails_before_service_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "home").mkdir()
            with patch("check_service_attention.Path.home", return_value=root):
                with self.assertRaisesRegex(CheckError, "existing.*auth.json login"):
                    fixture_options(parse_args(["--live-openai"]), root)
            self.assertFalse((root / "home" / "auth.json").exists())

    def test_glm_and_no_turn_do_not_access_auth(self):
        for arguments in [[], ["--live-glm"]]:
            with (
                self.subTest(arguments=arguments),
                patch(
                    "check_service_attention.Path.home",
                    side_effect=AssertionError("unexpected auth access"),
                ),
            ):
                options = fixture_options(parse_args(arguments), Path("fixture"))
                self.assertIn('model_provider="lapis_probe"', options)

    def test_runtime_provider_and_model_must_match(self):
        args = parse_args(["--live-openai"])
        verify_runtime_model({"model": "gpt-6-astra", "modelProvider": "openai"}, args)
        for runtime in [
            {"model": "other", "modelProvider": "openai"},
            {"model": "gpt-6-astra", "modelProvider": "lapis_probe"},
            {},
        ]:
            with self.subTest(runtime=runtime):
                with self.assertRaisesRegex(
                    CheckError, "Unexpected runtime model/provider"
                ):
                    verify_runtime_model(runtime, args)


class ModelPropagationTests(unittest.IsolatedAsyncioTestCase):
    async def test_question_fixture_uses_selected_model(self):
        owner = SimpleNamespace(rpc=AsyncMock(return_value={}))
        await question_turn(owner, "fixture-thread", "color", "selected-model")
        method, params = owner.rpc.call_args.args
        self.assertEqual(method, "turn/start")
        self.assertEqual(
            params["collaborationMode"]["settings"]["model"], "selected-model"
        )


class StreamSocket:
    def __init__(self, chunks, *, expire_first=False):
        self.chunks = list(chunks)
        self.now = 0
        self.expire_first = expire_first
        self.closed = False

    def settimeout(self, timeout):
        pass

    def recv(self, size):
        if not self.chunks:
            raise socket.timeout
        if self.expire_first:
            self.expire_first = False
            self.now += 1
        return self.chunks.pop(0)

    def close(self):
        self.closed = True


def client_for(stream):
    client = WireClient.__new__(WireClient)
    client.socket = stream
    client.buffer = bytearray()
    client.attachment = b"a" * 40
    client.cached_snapshot = None
    return client


class ViewTests(unittest.IsolatedAsyncioTestCase):
    async def test_partial_frame_deadline_preserves_and_completes_frame(self):
        payload = b"a" * 40 + b"exact-retry-token"
        encoded = frame(ATTENTION_RETRY, payload)
        stream = StreamSocket([encoded[:2], encoded[2:]], expire_first=True)
        client = client_for(stream)
        # Only the wire reader's clock advances past its first deadline. The
        # second receive must resume the same buffered frame, without data loss.
        with patch(
            "check_cli_launch.time", SimpleNamespace(monotonic=lambda: stream.now)
        ):
            view = View(client)
            try:
                await view.wait(lambda: view.retry is not None, 2)
                self.assertEqual(view.retry, b"exact-retry-token")
                self.assertEqual(client.buffer, b"")
            finally:
                await view.close()
        self.assertTrue(stream.closed)

    async def test_malformed_frame_and_eof_remain_fatal(self):
        for data, error, message in [
            (bytes(4), CheckError, "Invalid frame size"),
            (b"", EOFError, "Service disconnected"),
        ]:
            with self.subTest(message=message):
                stream = StreamSocket([data])
                view = View(client_for(stream))
                with self.assertRaisesRegex(error, message):
                    await view.wait(lambda: False, 2)
                with self.assertRaises(error):
                    await view.close()
                self.assertTrue(stream.closed)


class DiagnosticTests(unittest.IsolatedAsyncioTestCase):
    async def test_missing_codex_has_clear_diagnostic(self):
        with patch("check_service_attention.shutil.which", return_value=None):
            with self.assertRaisesRegex(CheckError, "codex is not installed"):
                await exercise(object(), {})


class CleanupTests(unittest.IsolatedAsyncioTestCase):
    async def test_primary_failure_survives_cleanup_failures(self):
        for stop_fails, groups_fail in [(True, False), (False, True), (True, True)]:
            with self.subTest(stop_fails=stop_fails, groups_fail=groups_fail):
                service = Mock()
                service.stop.side_effect = (
                    RuntimeError("stop failed") if stop_fails else None
                )
                wait = AsyncMock(side_effect=TimeoutError() if groups_fail else None)
                receipt = {}
                original = CheckError("qualification failed")
                with patch("check_service_attention.wait_groups_gone", wait):
                    with self.assertRaises(CheckError) as raised:
                        try:
                            raise original
                        finally:
                            await cleanup_service(
                                service, {42}, receipt, sys.exception()
                            )
                self.assertIs(raised.exception, original)
                expected = []
                if stop_fails:
                    expected.append("service cleanup: stop failed")
                if groups_fail:
                    expected.append("process-group cleanup: TimeoutError")
                self.assertEqual(receipt["cleanup_errors"], expected)
                self.assertEqual(original.__notes__, expected)
                service.stop.assert_called_once_with()
                wait.assert_awaited_once_with({42})

    async def test_cleanup_failure_after_success_fails_qualification(self):
        for stage in ("stop", "groups"):
            with self.subTest(stage=stage):
                service = Mock()
                service.stop.side_effect = (
                    RuntimeError("stop failed") if stage == "stop" else None
                )
                wait = AsyncMock(
                    side_effect=TimeoutError() if stage == "groups" else None
                )
                receipt = {}
                with patch("check_service_attention.wait_groups_gone", wait):
                    with self.assertRaises(CheckError) as raised:
                        await cleanup_service(service, {42}, receipt, None)
                self.assertEqual(str(raised.exception), receipt["cleanup_errors"][0])
                service.stop.assert_called_once_with()
                wait.assert_awaited_once_with({42})

    async def test_successful_cleanup_records_reaped_processes(self):
        for groups in (set(), {42}):
            with self.subTest(groups=groups):
                service = Mock()
                service.process.poll.return_value = 0
                receipt = {}
                with patch(
                    "check_service_attention.wait_groups_gone", new_callable=AsyncMock
                ) as wait:
                    await cleanup_service(service, groups, receipt, None)
                self.assertTrue(receipt["service_reaped"])
                self.assertNotIn("cleanup_errors", receipt)
                service.stop.assert_called_once_with()
                if groups:
                    wait.assert_awaited_once_with(groups)
                    self.assertTrue(receipt["owned_process_groups_cleaned"])
                else:
                    wait.assert_not_awaited()
                    self.assertNotIn("owned_process_groups_cleaned", receipt)


class QuestionFixtureTests(unittest.TestCase):
    def test_expected_answers_follow_question_and_option_order(self):
        questions = [
            {
                "id": "color_second",
                "options": [{"label": "Yellow"}, {"label": "Red (Recommended)"}],
            },
            {
                "id": "color_first",
                "options": [{"label": "Green"}, {"label": "Blue"}],
            },
        ]
        self.assertEqual(
            selected_question_answers(questions),
            {"color_first": "Blue", "color_second": "Red (Recommended)"},
        )

    def test_rejects_duplicate_ids_and_ambiguous_or_missing_colors(self):
        duplicate_ids = [
            {"id": "color_first", "options": [{"label": "Blue"}]},
            {"id": "color_first", "options": [{"label": "Red"}]},
        ]
        ambiguous = [
            {
                "id": "color_first",
                "options": [{"label": "Blue"}, {"label": "Blue (Recommended)"}],
            },
            {"id": "color_second", "options": [{"label": "Red"}]},
        ]
        missing = [
            {"id": "color_first", "options": [{"label": "Green"}]},
            {"id": "color_second", "options": [{"label": "Red"}]},
        ]
        for questions in (duplicate_ids, ambiguous, missing):
            with self.subTest(questions=questions):
                with self.assertRaisesRegex(CheckError, "Unexpected question"):
                    selected_question_answers(questions)


if __name__ == "__main__":
    unittest.main()
