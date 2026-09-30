"""Keep the lapis iPhone gateway running for this user (a macOS LaunchAgent).

    uv run --no-project python apps/remote/launch_agent.py install|uninstall|status

The agent starts at login, restarts if it exits, and serves whichever of
Tailscale or ZeroTier answers.
It logs requests (paths and status only) to ~/Library/Logs/lapis-remote.log.
"""

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

LABEL = "dev.lapis.remote"
GATEWAY = Path(__file__).resolve().parent / "lapis_remote.py"
PLIST = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
LOG = Path.home() / "Library" / "Logs" / "lapis-remote.log"
DOMAIN = f"gui/{os.getuid()}"


def launchctl(*arguments, check=False):
    return subprocess.run(
        ["launchctl", *arguments], capture_output=True, text=True, check=check
    )


def install():
    """Install with whichever overlays the Mac has; the gateway refuses to
    serve without at least one of tailscale or zerotier-cli answering."""
    tailscale = shutil.which("tailscale")
    zerotier = shutil.which("zerotier-cli")
    if tailscale is None and zerotier is None:
        raise SystemExit("neither tailscale nor zerotier-cli is on PATH")
    arguments = [sys.executable, str(GATEWAY)]
    if tailscale is not None:
        arguments += ["--tailscale", tailscale]
    if zerotier is not None:
        arguments += ["--zerotier", zerotier]
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    PLIST.write_bytes(
        plistlib.dumps(
            {
                "Label": LABEL,
                "ProgramArguments": arguments,
                "RunAtLoad": True,
                "KeepAlive": True,
                "ThrottleInterval": 10,
                "ProcessType": "Background",
                "StandardOutPath": str(LOG),
                "StandardErrorPath": str(LOG),
            }
        )
    )
    launchctl("bootout", f"{DOMAIN}/{LABEL}")
    launchctl("bootstrap", DOMAIN, str(PLIST), check=True)
    print(f"installed {PLIST}; log {LOG}")


def uninstall():
    launchctl("bootout", f"{DOMAIN}/{LABEL}")
    PLIST.unlink(missing_ok=True)
    print("removed")


def status():
    result = launchctl("print", f"{DOMAIN}/{LABEL}")
    if result.returncode != 0:
        print("not installed")
        return
    for line in result.stdout.splitlines():
        if line.strip().startswith(("state =", "pid =", "last exit code")):
            print(line.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("action", choices=["install", "uninstall", "status"])
    action = parser.parse_args().action
    {"install": install, "uninstall": uninstall, "status": status}[action]()


if __name__ == "__main__":
    main()
