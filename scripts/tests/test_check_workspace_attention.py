import asyncio
import tempfile
import sys
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from check_cli_launch import CheckError, Service
import check_workspace_attention
from check_workspace_attention import request_by_role


class FixtureResponseTests(unittest.TestCase):
    def test_request_by_role_rejects_unsafe_approval_and_wrong_context(self):
        request = {
            "thread": "thread-a",
            "details": {"command": "python3 -c 'print(no)'"},
        }
        with self.assertRaisesRegex(CheckError, "non-fixture approval"):
            request_by_role([request], "approval", "thread-a")
        with self.assertRaisesRegex(CheckError, "Expected one live"):
            request_by_role([request], "approval", "other-thread")


class ResponseIdentityTests(unittest.TestCase):
    def test_response_requires_live_identity_typed_request_and_choice(self):
        expected = {
            "approval": {
                "sessionId": "session",
                "serviceEpoch": "epoch",
                "requestSuffix": "1:2:n7",
                "choice": "accept",
            }
        }
        response = {
            "sourceId": "approval",
            "sessionId": "session",
            "token": "session:epoch:3:1:2:n7",
            "choice": "accept",
        }
        check_workspace_attention.validate_responses([response], expected)
        for replacement in (
            {"sessionId": "neighbor"},
            {"token": "session:epoch:3:1:2:s37"},
            {"token": "session:epoch:3:1:3:n7"},
            {"choice": "submit"},
        ):
            with self.subTest(replacement=replacement), self.assertRaises(CheckError):
                check_workspace_attention.validate_responses(
                    [{**response, **replacement}], expected
                )


class CleanupTests(unittest.IsolatedAsyncioTestCase):
    async def test_start_source_success_retains_owner_view_and_service(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = Mock(build_dir=Path("/build"))
            view = Mock(screen="")
            owner = Mock()
            owner.close = AsyncMock()
            owner.rpc = AsyncMock(
                side_effect=[
                    {"data": ["thread"]},
                    {"thread": {"ephemeral": False}},
                ]
            )
            service = Mock()
            service.connect = Mock(return_value=view)
            service.stop = Mock()
            service.endpoint = Path(temporary) / "source.sock"
            service.process = Mock(pid=101)
            view.wait = AsyncMock()
            view.close = AsyncMock()
            with (
                patch("check_workspace_attention.Service", return_value=service),
                patch("check_workspace_attention.View", return_value=view),
                patch("check_workspace_attention.process_groups", return_value={201}),
                patch("check_workspace_attention.UnixWebSocketTransport") as transport,
                patch("check_workspace_attention.Client", return_value=owner) as client,
                patch("check_workspace_attention.initialize", new_callable=AsyncMock),
            ):
                transport.connect = AsyncMock()
                result = await check_workspace_attention.start_source(
                    args,
                    Path("/codex"),
                    Path(temporary),
                    Path(temporary) / "artifacts",
                    "approval",
                    "safe fixture",
                )
            self.assertIs(result.owner, owner)
            self.assertIs(result.service, service)
            self.assertEqual(result.process_groups, frozenset({201}))
            owner.close.assert_not_awaited()
            view.close.assert_awaited_once()
            service.stop.assert_not_called()
            client.assert_called_once_with(transport.connect.return_value)

    async def test_owner_cancellation_does_not_skip_owned_service_cleanup(self):
        source = Mock(role="input", process_groups=frozenset({301}))
        source.owner.close = AsyncMock(side_effect=asyncio.CancelledError())
        source.service.process.poll.return_value = 0
        receipt = {}
        original = asyncio.CancelledError("cancelled qualification")
        with patch(
            "check_workspace_attention.wait_groups_gone", new_callable=AsyncMock
        ) as wait:
            await check_workspace_attention.stop_source(source, receipt, original)
        source.service.stop.assert_called_once()
        wait.assert_awaited_once_with(frozenset({301}))
        self.assertIn("CancelledError", receipt["cleanup_errors"][0])
        self.assertTrue(original.__notes__)

    async def test_cancelled_monitor_does_not_skip_neighbor(self):
        monitors = check_workspace_attention.PendingMonitors()
        first = Mock(close=AsyncMock(side_effect=asyncio.CancelledError()))
        second = Mock(close=AsyncMock())
        monitors.monitors = {"approval": first, "input": second}
        original = asyncio.CancelledError("cancelled qualification")
        receipt = {}
        await monitors.close(receipt, original)
        second.close.assert_awaited_once()
        self.assertTrue(receipt["cleanup_errors"])

    async def test_cleanup_failure_preserves_primary_exception_and_is_reported(self):
        service = Mock()
        service.process.poll.return_value = 0
        service.stop.side_effect = RuntimeError("stop failed")
        source = Mock(
            role="approval", owner=Mock(), service=service, process_groups=set()
        )
        source.owner.close = AsyncMock()
        receipt = {}
        original = CheckError("qualification failed")
        with self.assertRaises(CheckError):
            try:
                raise original
            finally:
                await check_workspace_attention.stop_source(
                    source, receipt, sys.exception()
                )
        self.assertIn(
            "approval: service cleanup: stop failed", receipt["cleanup_errors"]
        )
        self.assertIn("approval: service cleanup: stop failed", original.__notes__)

    async def test_stop_source_waits_only_on_owned_groups(self):
        service = Mock()
        service.process.poll.return_value = 0
        source = Mock(
            role="input",
            owner=Mock(),
            service=service,
            process_groups=frozenset({201}),
        )
        source.owner.close = AsyncMock()
        receipt = {}
        with patch(
            "check_workspace_attention.wait_groups_gone", new_callable=AsyncMock
        ) as wait:
            await check_workspace_attention.stop_source(source, receipt)
        wait.assert_awaited_once_with(frozenset({201}))
        self.assertTrue(receipt["services_reaped"])
        self.assertTrue(receipt["process_groups_cleaned"])


class StartupFailureTests(unittest.TestCase):
    def test_concurrent_start_failure_retains_sibling_and_cleanup_notes(self):
        primary = RuntimeError("approval failed")
        primary.add_note("approval service cleanup: stop failed")
        sibling = TimeoutError("input failed")
        sibling.add_note("input service cleanup: stop failed")

        with self.assertRaises(RuntimeError) as raised:
            check_workspace_attention.raise_concurrent_start_failures(
                [primary, sibling]
            )

        self.assertIs(raised.exception, primary)
        self.assertIn(
            "approval service cleanup: stop failed", raised.exception.__notes__
        )
        self.assertIn(
            "concurrent source start also failed: input failed",
            raised.exception.__notes__,
        )
        self.assertIn(
            "concurrent source start cleanup: input service cleanup: stop failed",
            raised.exception.__notes__,
        )

    def test_conflicting_agent_modes_fail_before_service_spawn(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch("check_cli_launch.subprocess.Popen") as popen:
                with self.assertRaisesRegex(CheckError, "at most one agent mode"):
                    Service(
                        root / "service",
                        root / "runtime",
                        root / "artifacts",
                        "conflicted",
                        "program",
                        [],
                        root,
                        codex=True,
                        claude=True,
                    )

        popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
