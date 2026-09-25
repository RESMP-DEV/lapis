"""Check that agents come back after a power loss, resuming their conversations.

Uses the real desktop restore helper (`lapis_desktop --restore-agents`) on a
disposable workspace in /tmp, with:
- real Codex (managed mode) and Claude Code against scripts/fake_models.py, so
  no model usage is spent;
- tools/qa/fake_agent.py installed as grok, kimi, opencode and omp, which
  report a conversation the way the agent-checkpoint hooks do and say which
  conversation they resumed.

It starts every agent with the helper (a first boot) and holds a conversation
with each; Codex and Claude then start a second one with /new and /clear.
Twice it simulates a power loss (SIGKILL on every process at once, leaving
stale sockets) and boots again with the helper, the second time as a launchd
job like the login LaunchAgent. After each boot Codex and Claude must retain their native context: they show
the exchange after /new and /clear, and the model receives it with the next
prompt. OSC-only stand-ins instead report
starting fresh without an unverified resume argument. Finally the helper runs
with everything alive and must leave it untouched. macOS; nothing is left
running.

    uv run --no-project python scripts/check_restore.py
"""

import json
import os
import plistlib
import shutil
import signal
import socket
import subprocess
import struct
import sys
import tempfile
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESKTOP = (
    ROOT / "build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop"
)
SERVICE_NAME = "lapis_session_service"
STAND_INS = ("grok", "kimi", "opencode", "omp")
LAUNCHD_KEPT = (
    "PATH",
    "LANG",
    "SHELL",
    "CODEX_HOME",
    "CLAUDE_CONFIG_DIR",
    "ANTHROPIC_BASE_URL",
    "ANTHROPIC_AUTH_TOKEN",
    "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC",
    "LAPIS_HISTORY_ROOT",
)

if __package__:
    from . import check_cli_launch as wire
else:
    import check_cli_launch as wire

CODEX_CONFIG = """model = "lapis-fake"
model_provider = "lapis_fake"
approval_policy = "on-request"

[model_providers.lapis_fake]
name = "lapis fake"
base_url = "http://127.0.0.1:{port}/v1"
wire_api = "responses"
requires_openai_auth = false
supports_websockets = false
request_max_retries = 0
stream_max_retries = 0

[features]
apps = false
plugins = false
multi_agent = false

[analytics]
enabled = false

[projects.{directory}]
trust_level = "trusted"
"""


class Failure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Failure(message)


def screen(session, timeout=2.0):
    """Consume the existing test wire client's terminal frames without OS input."""
    text = (session.cached_snapshot or {}).get("text", "")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            kind, data = session.receive(
                min(0.2, max(0.001, deadline - time.monotonic()))
            )
        except socket.timeout:
            continue
        except wire.CheckError as error:
            if str(error) != "Frame deadline expired":
                raise
            continue
        require(
            kind != wire.STATUS,
            "Service status: " + data[1:].decode("utf-8", errors="replace"),
        )
        if kind == wire.SNAPSHOT:
            require(
                len(data) >= 72 and data[:40] == session.attachment,
                "Snapshot attachment mismatch",
            )
            sequence = struct.unpack_from(">Q", data, 40)[0]
            require(sequence > session.sequence, "Snapshot sequence did not advance")
            session.sequence = sequence
            session.cached_snapshot = wire.decode_snapshot(data[72:])
            text = session.cached_snapshot["text"]
        else:
            require(
                kind in (wire.ATTENTION_SNAPSHOT, wire.ATTENTION_RETRY),
                "Unexpected frame while reading a restore fixture",
            )
    return text


def wait_screen(session, needle, timeout=40):
    deadline = time.monotonic() + timeout
    seen = ""
    while time.monotonic() < deadline:
        seen = screen(session, 1.0)
        if needle in seen:
            return seen
    raise Failure(f"never showed {needle!r}; last screen:\n{seen}")


def record(endpoint):
    try:
        return json.loads(Path(endpoint + ".resume").read_text())
    except (OSError, ValueError):
        return None


def wait_record(endpoint, agent, timeout=40, replacing=None, source=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = record(endpoint)
        if (
            value
            and value.get("agent") == agent
            and value["session_id"] != replacing
            and (source is None or value.get("source") == source)
        ):
            return value["session_id"]
        time.sleep(0.2)
    raise Failure(f"{agent} never recorded its conversation at {endpoint}")


def tree(roots):
    """PIDs of the given processes and all their descendants."""
    rows = subprocess.run(
        ["ps", "-axo", "pid=,ppid="], capture_output=True, text=True, check=True
    ).stdout.split("\n")
    children = {}
    for row in rows:
        parts = row.split()
        if len(parts) == 2:
            children.setdefault(int(parts[1]), []).append(int(parts[0]))
    found, stack = set(), list(roots)
    while stack:
        pid = stack.pop()
        if pid not in found:
            found.add(pid)
            stack.extend(children.get(pid, []))
    return found


def workspace_processes(runtime):
    """Services, guards and backends of this workspace, with their agents."""
    rows = subprocess.run(
        ["ps", "-axo", "pid=,command="], capture_output=True, text=True, check=True
    ).stdout.split("\n")
    roots = [
        int(row.split(None, 1)[0])
        for row in rows
        if row.strip() and str(runtime) in row and "check_restore" not in row
    ]
    return tree(roots)


def power_loss(runtime):
    """Every process at once, no shutdown: what a power cut leaves behind."""
    pids = workspace_processes(runtime)
    require(pids, "nothing was running to lose")
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 10
    while workspace_processes(runtime) and time.monotonic() < deadline:
        time.sleep(0.1)
    require(not workspace_processes(runtime), "processes survived the power loss")
    return len(pids)


def service_pids(runtime):
    rows = subprocess.run(
        ["ps", "-axo", "pid=,command="], capture_output=True, text=True, check=True
    ).stdout.split("\n")
    return sorted(
        int(row.split(None, 1)[0])
        for row in rows
        if SERVICE_NAME in row and str(runtime) in row
    )


def boot(registry, environment, log):
    """The login helper: restart what is gone and exit."""
    result = subprocess.run(
        [str(DESKTOP), "--restore-agents", "--registry", str(registry)],
        env=environment,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=150,
    )
    log.write(result.stdout + result.stderr)
    require(result.returncode == 0, f"restore helper exited {result.returncode}")
    lines = [
        line
        for line in (result.stdout + result.stderr).splitlines()
        if "lapis restore:" in line
    ]
    return lines


def boot_launchd(registry, environment, log):
    """The login helper as launchd runs it: a job whose process group launchd
    kills when it exits, so the agents it starts must be outside that group."""
    label = f"dev.lapis.restore-check.{registry.parent.name}"
    target = f"gui/{os.getuid()}/{label}"
    plist = registry.parent / "restore-check.plist"
    output = ROOT / "build" / "restore-check" / "helper-launchd.log"
    output.unlink(missing_ok=True)
    plist.write_bytes(
        plistlib.dumps(
            {
                "Label": label,
                "ProgramArguments": [
                    str(DESKTOP),
                    "--restore-agents",
                    "--registry",
                    str(registry),
                ],
                # What the LaunchAgent records (PATH, LANG, SHELL) plus this
                # check's model and configuration; launchd supplies the rest.
                "EnvironmentVariables": {
                    name: value
                    for name, value in environment.items()
                    if name in LAUNCHD_KEPT
                },
                "RunAtLoad": True,
                "ProcessType": "Interactive",
                "StandardOutPath": str(output),
                "StandardErrorPath": str(output),
            }
        )
    )
    subprocess.run(
        ["launchctl", "bootstrap", f"gui/{os.getuid()}", str(plist)],
        check=True,
        capture_output=True,
    )
    try:
        deadline = time.monotonic() + 150
        state = ""
        while time.monotonic() < deadline:
            state = subprocess.run(
                ["launchctl", "print", target], capture_output=True, text=True
            ).stdout
            if "state = not running" in state and "last exit code = " in state:
                break
            time.sleep(0.5)
        require("last exit code = 0" in state, f"launchd helper: {state[-400:]}")
    finally:
        subprocess.run(["launchctl", "bootout", target], capture_output=True)
    text = output.read_text() if output.exists() else ""
    log.write(text)
    # Agents outlive the job: give launchd time to clean up after it.
    time.sleep(3)
    return [line for line in text.splitlines() if "lapis restore:" in line]


def fake_log(path):
    try:
        return [
            json.loads(line) for line in path.read_text().splitlines() if line.strip()
        ]
    except OSError:
        return []


def main():
    require(DESKTOP.exists(), f"missing {DESKTOP}; build the desktop first")
    for program in ("codex", "claude"):
        require(shutil.which(program), f"{program} is not installed")
    # Resolved: the desktop checks endpoints against the real folder
    # (/private/tmp on macOS).
    runtime = Path(tempfile.mkdtemp(prefix="lapis-r-", dir="/tmp")).resolve()
    logs = ROOT / "build" / "restore-check"
    logs.mkdir(parents=True, exist_ok=True)
    log = (logs / "helper.log").open("w")
    models = None
    try:
        bin_dir = runtime / "bin"
        bin_dir.mkdir()
        for name in STAND_INS:
            target = bin_dir / name
            shutil.copy2(ROOT / "tools/qa/fake_agent.py", target)
            target.chmod(0o755)
        codex_home = runtime / "codex-home"
        claude_config = runtime / "claude-config"
        for directory in (codex_home, claude_config):
            directory.mkdir(mode=0o700)
        work = {}
        for name in ("codex", "claude", *STAND_INS):
            work[name] = runtime / f"work-{name}"
            work[name].mkdir()
        version = subprocess.run(
            ["claude", "--version"], capture_output=True, text=True
        ).stdout.split()[0]
        (claude_config / ".claude.json").write_text(
            json.dumps(
                {
                    "hasCompletedOnboarding": True,
                    "theme": "dark",
                    "lastOnboardingVersion": version,
                    "projects": {
                        str(work["claude"].resolve()): {"hasTrustDialogAccepted": True}
                    },
                }
            )
        )
        # Outside the workspace folder, so a power loss spares the fake model.
        fake_models_log = logs / "fake-models.jsonl"
        fake_models_log.unlink(missing_ok=True)
        ready = logs / "fake-models-ready.json"
        ready.unlink(missing_ok=True)
        models = subprocess.Popen(
            [
                sys.executable,
                str(ROOT / "scripts/fake_models.py"),
                "--port",
                "0",
                "--ready-file",
                str(ready),
                "--log",
                str(fake_models_log),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        deadline = time.monotonic() + 10
        while (
            not ready.exists() and models.poll() is None and time.monotonic() < deadline
        ):
            time.sleep(0.05)
        require(ready.exists() and models.poll() is None, "fake model did not listen")
        port = json.loads(ready.read_text())["port"]
        (codex_home / "config.toml").write_text(
            CODEX_CONFIG.format(
                port=port, directory=json.dumps(str(work["codex"].resolve()))
            )
        )
        environment = {
            **os.environ,
            "PATH": f"{bin_dir}:{os.environ.get('PATH', '')}",
            "CODEX_HOME": str(codex_home),
            "CLAUDE_CONFIG_DIR": str(claude_config),
            "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{port}",
            "ANTHROPIC_AUTH_TOKEN": "lapis-fake",
            "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
            "LAPIS_HISTORY_ROOT": str(runtime / "history"),
        }

        # The registry as the desktop writes it for new agents.
        agents, ids = [], {}
        for name in ("codex", "claude", *STAND_INS):
            identifier = str(uuid.uuid4())
            ids[name] = identifier
            program = (
                shutil.which(name)
                if name in ("codex", "claude")
                else str(bin_dir / name)
            )
            agents.append(
                {
                    "id": identifier,
                    "title": f"{name} agent",
                    "category": "general",
                    "endpoint": str(runtime / (identifier + ".sock")),
                    "program": program,
                    "harness": name,
                    "mode": "claude" if name == "claude" else "",
                    "resumeThread": "",
                    "arguments": ["-c", "check_for_update_on_startup=false"]
                    if name == "codex"
                    else [],
                    "directory": str(work[name].resolve()),
                }
            )
        registry = runtime / "workspace.json"
        registry.write_text(
            json.dumps(
                {
                    "version": 2,
                    "activeCategory": "general",
                    "categories": [
                        {"id": "general", "name": "General", "selected": ids["codex"]}
                    ],
                    "agents": agents,
                }
            )
        )
        endpoint = {name: str(runtime / (ids[name] + ".sock")) for name in ids}

        def attach(name):
            agent = next(
                a
                for a in json.loads(registry.read_text())["agents"]
                if a["id"] == ids[name]
            )
            client = wire.WireClient(agent["endpoint"])
            try:
                client.attach(
                    agent["program"],
                    agent["arguments"],
                    agent["directory"],
                    codex=agent["harness"] == "codex",
                    claude=agent.get("mode") == "claude",
                )
                return client
            except BaseException:
                client.close()
                raise

        print("first boot: the helper starts every agent")
        lines = boot(registry, environment, log)
        require(
            any("6 agents restarted" in line for line in lines), f"first boot: {lines}"
        )

        conversations = {}
        prompt = {
            "codex": "hello codex from before",
            "claude": "hello claude from before",
        }
        # Each starts a fresh conversation in the same agent first; the one to
        # come back is the one in use at the power loss.
        fresh = {"codex": "/new", "claude": "/clear"}

        def say(session, text):
            session.send(wire.PASTE, text.encode())
            time.sleep(0.3)
            session.send(wire.KEY, bytes([10, 0]))
            wait_screen(session, f"Fake model reply to: {text}")

        for name in ("codex", "claude"):
            session = attach(name)
            wait_screen(session, "codex" if name == "codex" else "Claude Code")
            time.sleep(2)
            say(session, f"{name} before {fresh[name]}")
            earlier = wait_record(endpoint[name], name, source="observer")
            session.send(wire.PASTE, fresh[name].encode())
            time.sleep(0.5)
            session.send(wire.KEY, bytes([10, 0]))
            time.sleep(3)
            say(session, prompt[name])
            # Codex builds the observer has not qualified are followed by the
            # service's rollout scan, once a minute after the first find.
            conversations[name] = wait_record(
                endpoint[name], name, timeout=90, replacing=earlier, source="observer"
            )
            session.close()
            print(
                f"  {name}: conversation {earlier}, then {fresh[name]} "
                f"{conversations[name]}"
            )
        for name in STAND_INS:
            conversations[name] = wait_record(endpoint[name], name, source="terminal")
            session = attach(name)
            session.send(wire.TEXT, f"note from {name}\r".encode())
            wait_screen(session, f"echo: note from {name}")
            session.close()
            print(f"  {name}: conversation {conversations[name]}")
        first_context = {
            "codex": max(
                (
                    len(e.get("input_types", []))
                    for e in fake_log(fake_models_log)
                    if e.get("path") == "/v1/responses"
                ),
                default=0,
            ),
            "claude": max(
                (e.get("messages", 0) for e in fake_log(fake_models_log)), default=0
            ),
        }

        for round_number in (1, 2):
            lost = power_loss(runtime)
            print(
                f"power loss {round_number}: {lost} processes killed, sockets left behind"
            )
            require(
                all(Path(path).exists() for path in endpoint.values()),
                "stale sockets are part of the test",
            )
            # The second boot runs as the login LaunchAgent does.
            helper = boot if round_number == 1 else boot_launchd
            began = time.monotonic()
            lines = helper(registry, environment, log)
            took = time.monotonic() - began
            require(
                any("6 agents restarted" in line for line in lines),
                f"boot {round_number}: {lines}",
            )
            require(
                all("running" in line for line in lines if " agent " in line),
                f"boot {round_number}: {lines}",
            )
            saved = {a["id"]: a for a in json.loads(registry.read_text())["agents"]}
            for name in ("codex", "claude"):
                session = attach(name)
                shown = wait_screen(session, prompt[name])
                require(
                    f"Fake model reply to: {prompt[name]}" in shown,
                    f"{name} lost the earlier reply",
                )
                follow_up = f"{name} after power loss {round_number}"
                session.send(wire.PASTE, follow_up.encode())
                time.sleep(0.3)
                session.send(wire.KEY, bytes([10, 0]))
                wait_screen(session, f"Fake model reply to: {follow_up}")
                session.close()
                require(
                    record(endpoint[name])["session_id"] == conversations[name],
                    f"{name} changed conversation",
                )
                arguments = saved[ids[name]]["arguments"]
                require(
                    arguments.count(conversations[name]) == 1,
                    f"{name} resume arguments: {arguments}",
                )
            for name in STAND_INS:
                session = attach(name)
                previous = conversations[name]
                current = wait_record(
                    endpoint[name], name, replacing=previous, source="terminal"
                )
                wait_screen(session, f"new conversation {current}")
                session.close()
                require(
                    previous not in saved[ids[name]]["arguments"],
                    f"{name} injected an advisory identity into argv",
                )
                conversations[name] = current
            entries = fake_log(fake_models_log)
            codex_context = max(
                (
                    len(e.get("input_types", []))
                    for e in entries
                    if e.get("path") == "/v1/responses"
                ),
                default=0,
            )
            claude_context = max((e.get("messages", 0) for e in entries), default=0)
            require(
                codex_context > first_context["codex"],
                "Codex's next request lacked the earlier conversation",
            )
            require(
                claude_context > first_context["claude"],
                "Claude's next request lacked the earlier conversation",
            )
            print(
                f"boot {round_number}{' (launchd)' if round_number == 2 else ''}: "
                f"helper {took:.1f} s, native context resumed and 4 advisory agents restarted fresh; "
                f"Codex context {first_context['codex']} -> {codex_context} items, "
                f"Claude {first_context['claude']} -> {claude_context} messages"
            )

        before = service_pids(runtime)
        lines = boot(registry, environment, log)
        require(
            any("0 agents restarted" in line for line in lines), f"live boot: {lines}"
        )
        require(service_pids(runtime) == before, "the helper disturbed running agents")
        print("helper with everything running: nothing restarted, same processes")
        print("PASS")
        return 0
    except (
        Failure,
        wire.CheckError,
        OSError,
        EOFError,
        subprocess.SubprocessError,
    ) as error:
        print(f"FAIL: {error}")
        for service_log in runtime.glob("*.sock.log"):
            shutil.copy2(service_log, logs / service_log.name)
        print(f"service logs copied to {logs}")
        return 1
    finally:
        log.close()
        for pid in workspace_processes(runtime):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if models is not None:
            try:
                os.killpg(models.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            models.wait(timeout=5)
        shutil.rmtree(runtime, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
