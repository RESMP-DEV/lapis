#!/usr/bin/env python3
"""Manage the Claude Code and Codex plans lapis gives its sessions.

lapis reads the plans from the "accounts" section of its config
(~/.lapis/lapis.json) and chooses one for each Claude Code and Codex session:
a machine's own sign-in first, then, once a plan is at the switch point, the
plan with the most room that the machine can use. A plan is usable on its
home machine (that machine's own sign-in) and on every machine where lapis
keeps a credential for it:

  Claude Code  a setup token in ~/.lapis/accounts/claude/NAME.token (0600),
               from `claude setup-token`, passed as CLAUDE_CODE_OAUTH_TOKEN
  Codex        a home ~/.lapis/accounts/codex/NAME with its own auth.json,
               from `codex login` there, sharing everything else with ~/.codex

Usage:
  lapis_accounts.py list
  lapis_accounts.py homes [HOST ...]
  lapis_accounts.py sign-in [--to HOST ...]
  lapis_accounts.py add-claude NAME --email EMAIL [--to HOST ...]
  lapis_accounts.py add-codex NAME --email EMAIL [--on local|HOST ...]

`sign-in` goes through every listed Claude Code plan that has no token on this
Mac yet, asking before each: one browser approval per plan, and the token is
kept on every machine the plans name.

Credentials never appear on a command line or in this script's output.
"""

from __future__ import annotations

import argparse
import json
import os
import pty
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

CONFIG = Path.home() / ".lapis" / "lapis.json"
ACCOUNTS = Path.home() / ".lapis" / "accounts"
NAME = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
HOST = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")
SETUP_TOKEN = re.compile(rb"sk-ant-oat01-[A-Za-z0-9_-]{20,}")
LOCAL = "local"

# Reads a machine's own sign-ins: Claude Code's email and Codex's, from its
# ID token's claims. Prints JSON; no credential leaves the machine.
SIGN_INS = r"""
import base64, json, os
out = {}
try:
    account = json.load(open(os.path.expanduser("~/.claude.json"))).get("oauthAccount") or {}
    out["claude"] = account.get("emailAddress", "")
except Exception:
    pass
try:
    home = os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")
    token = (json.load(open(os.path.join(home, "auth.json"))).get("tokens") or {}).get("id_token", "")
    claims = json.loads(base64.urlsafe_b64decode(token.split(".")[1] + "=="))
    out["codex"] = claims.get("email", "")
except Exception:
    pass
print(json.dumps(out))
"""


class Failure(RuntimeError):
    """A step that cannot go on; the message says why."""


def load_config(path: Path = CONFIG) -> dict:
    if not path.exists():
        return {}
    config = json.loads(path.read_text())
    if not isinstance(config, dict):
        raise Failure(f"{path} is not a JSON object")
    return config


def save_config(config: dict, path: Path = CONFIG) -> None:
    """Replace the file whole, so lapis never reads half of it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=path.parent, prefix=".lapis.json.")
    with os.fdopen(handle, "w") as out:
        json.dump(config, out, indent=2)
        out.write("\n")
    os.replace(temporary, path)


def machines(config: dict) -> list[str]:
    """This Mac and the machines the usage dashboard asks."""
    usage = config.get("usage") if isinstance(config.get("usage"), dict) else {}
    hosts = [
        h for h in usage.get("machines", []) if isinstance(h, str) and HOST.match(h)
    ]
    return [LOCAL, *hosts]


def plan_name(email: str) -> str:
    """someone@gmail.com -> someone-gmail."""
    user, _, domain = email.partition("@")
    name = re.sub(
        r"[^A-Za-z0-9._-]", "-", f"{user}-{domain.split('.')[0]}" if domain else user
    )
    return name[:64] or "plan"


def merge_plan(
    config: dict, cli: str, name: str, email: str, home: str | None, kept: list[str]
) -> dict:
    """Add or update one plan, matched by name or email, and return it."""
    section = config.setdefault("accounts", {})
    plans = section.setdefault(cli, [])
    email = email.strip().lower()
    plan = next(
        (
            p
            for p in plans
            if p.get("name") == name or (email and p.get("email", "").lower() == email)
        ),
        None,
    )
    if plan is None:
        plan = {"name": name}
        plans.append(plan)
    if email:
        plan["email"] = email
    if home is not None and "home" not in plan:
        plan["home"] = home
    held = [m for m in plan.get("machines", []) if isinstance(m, str)]
    for machine in kept:
        if machine not in held:
            held.append(machine)
    if held:
        plan["machines"] = held
    return plan


def ssh(host: str, command: str, stdin: bytes | None = None, tty: bool = False):
    arguments = ["ssh", "-o", "ConnectTimeout=10"] + (["-t"] if tty else ["-T"])
    return subprocess.run(
        arguments + [host, command],
        input=stdin,
        capture_output=not tty,
        check=False,
    )


def sign_ins(machine: str) -> dict:
    if machine == LOCAL:
        result = subprocess.run(
            [sys.executable, "-c", SIGN_INS], capture_output=True, check=False
        )
    else:
        result = ssh(machine, "python3 -c " + shlex.quote(SIGN_INS))
    try:
        return json.loads(result.stdout.decode().strip().splitlines()[-1])
    except (IndexError, ValueError):
        return {}


def command_homes(config: dict, hosts: list[str]) -> None:
    """Record each machine's own sign-ins as home plans: this Mac and `hosts`,
    else the usage machines."""
    for machine in [LOCAL, *hosts] if hosts else machines(config):
        found = sign_ins(machine)
        for cli in ("claude", "codex"):
            email = found.get(cli, "")
            if not email:
                continue
            section = config.get("accounts", {}).get(cli, [])
            known = next(
                (p for p in section if p.get("email", "").lower() == email.lower()),
                None,
            )
            if known is not None and "home" in known and known["home"] != machine:
                # One plan, signed in on two machines: the second holds it too.
                merge_plan(config, cli, known["name"], email, None, [machine])
            else:
                merge_plan(config, cli, plan_name(email), email, machine, [])
            print(f"{machine}: {cli} signs in as {email}")
    save_config(config)


TOKEN_PREFIX = b"sk-ant-oat01-"


def masked_output(pending: bytes) -> tuple[bytes, bytes]:
    """Split pending terminal output into what can be shown now, with any
    setup token hidden, and what to hold: a token whose line has not ended,
    or an ending that could be the start of one."""
    start = pending.rfind(TOKEN_PREFIX[:1])
    hold = len(pending)
    while start >= 0:
        rest = pending[start:]
        if TOKEN_PREFIX.startswith(rest[: len(TOKEN_PREFIX)]) and (
            len(rest) < len(TOKEN_PREFIX) or b"\n" not in rest
        ):
            hold = start
        start = pending.rfind(TOKEN_PREFIX[:1], 0, start)
    show, keep = pending[:hold], pending[hold:]
    return SETUP_TOKEN.sub(b"sk-ant-oat01-[kept by lapis]", show), keep


def read_setup_token() -> bytes:
    """Run `claude setup-token` on a wide terminal of its own, pass keys to it,
    show its output with the token hidden, and return the token."""
    import fcntl
    import select
    import struct
    import termios
    import tty

    error_read, error_write = os.pipe()  # close-on-exec: EOF means exec succeeded
    try:
        pid, fd = pty.fork()
    except OSError as error:
        os.close(error_read)
        os.close(error_write)
        raise Failure(f"could not create setup-token terminal: {error}") from error
    if pid == 0:
        os.close(error_read)
        try:
            # Wide enough that the token is never wrapped.
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 400, 0, 0))
            os.execvp("claude", ["claude", "setup-token"])
        except OSError as error:
            try:
                os.write(error_write, str(error).encode()[:4096])
            finally:
                os._exit(127)
    os.close(error_write)
    seen = bytearray()
    pending = b""
    saved = None
    status = 0
    try:
        try:
            launch_error = os.read(error_read, 4096)
        finally:
            os.close(error_read)
        if launch_error:
            raise Failure(
                "could not start claude setup-token: "
                + launch_error.decode(errors="replace")
            )
        saved = termios.tcgetattr(0) if os.isatty(0) else None
        if saved is not None:
            tty.setraw(0)
        while True:
            ready, _, _ = select.select([0, fd] if saved is not None else [fd], [], [])
            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    data = b""
                if not data:
                    break
                seen.extend(data)
                show, pending = masked_output(pending + data)
                os.write(1, show)
            if 0 in ready:
                os.write(fd, os.read(0, 1024))
    finally:
        try:
            if saved is not None:
                termios.tcsetattr(0, termios.TCSAFLUSH, saved)
            os.write(1, masked_output(pending + b"\n")[0])
        finally:
            os.close(fd)
            _, status = os.waitpid(pid, 0)
    if status != 0:
        raise Failure(
            f"claude setup-token exited with status {os.waitstatus_to_exitcode(status)}"
        )
    matches = SETUP_TOKEN.findall(bytes(seen))
    if not matches:
        raise Failure("claude setup-token printed no token")
    return matches[-1]


def keep_claude_token(name: str, token: bytes, hosts: list[str]) -> list[str]:
    folder = ACCOUNTS / "claude"
    folder.mkdir(parents=True, exist_ok=True)
    folder.chmod(0o700)
    path = folder / f"{name}.token"
    handle = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(handle, "wb") as out:
        out.write(token + b"\n")
    kept = [LOCAL]
    target = f"~/.lapis/accounts/claude/{name}.token"
    for host in hosts:
        result = ssh(
            host,
            f"umask 077 && mkdir -p ~/.lapis/accounts/claude && cat > {target}",
            stdin=token + b"\n",
        )
        if result.returncode == 0:
            kept.append(host)
            print(f"{host}: kept the token")
        else:
            print(f"{host}: could not keep the token (ssh exited {result.returncode})")
    return kept


def command_add_claude(config: dict, name: str, email: str, hosts: list[str]) -> None:
    print(f"Sign in as {email} in the browser `claude setup-token` opens.")
    token = read_setup_token()
    kept = keep_claude_token(name, token, hosts)
    merge_plan(config, "claude", name, email, None, kept)
    save_config(config)
    print(f"lapis can give Claude Code sessions {name} on: {', '.join(kept)}")


def command_add_codex(config: dict, name: str, email: str, where: list[str]) -> None:
    kept = []
    home = f"~/.lapis/accounts/codex/{name}"
    login = (
        f'mkdir -p {home} && chmod 700 {home} && CODEX_HOME="$HOME/.lapis/accounts/codex/{name}" '
        'exec "${SHELL:-/bin/sh}" -lic "codex login --device-auth"'
    )
    check = f'test -s "$HOME/.lapis/accounts/codex/{name}/auth.json"'
    for machine in where:
        print(f"{machine}: sign in as {email} with the code Codex shows")
        if machine == LOCAL:
            subprocess.run(["sh", "-c", login], check=False)
            done = subprocess.run(["sh", "-c", check], check=False).returncode == 0
        else:
            ssh(machine, login, tty=True)
            done = ssh(machine, check).returncode == 0
        if done:
            kept.append(machine)
        else:
            print(f"{machine}: no login was saved")
    merge_plan(config, "codex", name, email, None, kept)
    save_config(config)
    print(
        f"lapis can give Codex sessions {name} on: {', '.join(kept) or 'no machine yet'}"
    )


def plan_machines(config: dict, plan: dict) -> list[str]:
    """Usage hosts plus this Claude plan's own credential destinations."""
    hosts = machines(config)[1:]
    selected = plan.get("machines")
    selected = selected if isinstance(selected, list) else []
    for host in [plan.get("home"), *selected]:
        if (
            isinstance(host, str)
            and host != LOCAL
            and HOST.fullmatch(host)
            and host not in hosts
        ):
            hosts.append(host)
    return hosts


def command_sign_in(config: dict, hosts: list[str] | None = None, ask=input) -> None:
    section = config.get("accounts")
    plans = section.get("claude") if isinstance(section, dict) else []
    plans = plans if isinstance(plans, list) else []
    waiting = []
    for plan in plans:
        if not isinstance(plan, dict):
            print("skipping a malformed Claude Code plan")
            continue
        name, email = plan.get("name"), plan.get("email")
        if not isinstance(name, str) or not NAME.fullmatch(name) or name in (".", ".."):
            print("skipping a plan with an unusable name")
            continue
        if not isinstance(email, str) or not email.strip():
            print(f"skipping {name}: plan has no usable email")
            continue
        if not (ACCOUNTS / "claude" / f"{name}.token").exists():
            waiting.append((plan, name, email.strip()))
    if not waiting:
        print("No valid Claude Code plan needs a local token.")
        return
    for plan, name, email in waiting:
        if ask(f"Sign in {name} ({email}) now? [Y/n] ").strip().lower() in ("n", "no"):
            continue
        destinations = hosts if hosts is not None else plan_machines(config, plan)
        command_add_claude(config, name, email, destinations)


def command_list(config: dict) -> None:
    section = config.get("accounts", {})
    print(f"Switch point: {section.get('switchAt', 95)}%")
    for cli in ("claude", "codex"):
        for plan in section.get(cli, []):
            home = plan.get("home", "-")
            kept = ", ".join(plan.get("machines", [])) or "-"
            print(
                f"{cli:6} {plan.get('name'):28} {plan.get('email', ''):32} home {home:8} kept {kept}"
            )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    homes = commands.add_parser("homes")
    homes.add_argument(
        "hosts", nargs="*", help="machines besides this Mac (default: usage machines)"
    )
    sign_in = commands.add_parser("sign-in")
    sign_in.add_argument(
        "--to",
        nargs="*",
        help="hosts for these tokens (default: each plan plus usage hosts; --to alone: local only)",
    )
    claude = commands.add_parser("add-claude")
    claude.add_argument("name")
    claude.add_argument("--email", required=True)
    claude.add_argument(
        "--to", nargs="*", help="hosts to keep the token on (default: usage machines)"
    )
    codex = commands.add_parser("add-codex")
    codex.add_argument("name")
    codex.add_argument("--email", required=True)
    codex.add_argument(
        "--on", nargs="*", help="machines to sign in on (default: local)"
    )
    arguments = parser.parse_args(argv)
    try:
        config = load_config()
        if hasattr(arguments, "name") and (
            not NAME.fullmatch(arguments.name) or arguments.name in (".", "..")
        ):
            raise Failure("a plan name is letters, digits, '.', '_' or '-', at most 64")
        named = (getattr(arguments, "to", None) or []) + (
            getattr(arguments, "on", None) or []
        )
        for host in named + (getattr(arguments, "hosts", None) or []):
            if host != LOCAL and not HOST.match(host):
                raise Failure(f"not a host name: {host}")
        if arguments.command == "list":
            command_list(config)
        elif arguments.command == "homes":
            command_homes(config, arguments.hosts)
        elif arguments.command == "sign-in":
            command_sign_in(config, arguments.to)
        elif arguments.command == "add-claude":
            hosts = arguments.to if arguments.to is not None else machines(config)[1:]
            command_add_claude(config, arguments.name, arguments.email, hosts)
        else:
            command_add_codex(
                config, arguments.name, arguments.email, arguments.on or [LOCAL]
            )
    except Failure as error:
        print(f"lapis accounts: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
