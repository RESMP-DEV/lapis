"""Bring lapis agents back at login, without opening a window (a macOS LaunchAgent).

    uv run --no-project python scripts/restore_at_login.py install|uninstall|status|run

At login it runs `lapis_desktop --restore-agents`: agents whose services died
with the Mac (a restart, a crash, a power cut) start again in their folders,
resuming their conversations, and the helper exits. Opening lapis later
reattaches to them; the iPhone app can reach them before that. A window opened
while the helper runs waits for it.

Agents inherit the helper's environment, so install records this shell's PATH
(agents started from an open lapis window get the environment lapis was
opened with). `run` starts the helper now. Output goes to
~/Library/Logs/lapis-restore.log.
"""

import argparse
import os
import plistlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESKTOP = (
    ROOT / "build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop"
)
LABEL = "dev.lapis.restore"
PLIST = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
LOG = Path.home() / "Library" / "Logs" / "lapis-restore.log"
DOMAIN = f"gui/{os.getuid()}"
KEPT = ("PATH", "LANG", "SHELL")


def launchctl(*arguments, check=False):
    return subprocess.run(
        ["launchctl", *arguments], capture_output=True, text=True, check=check
    )


def environment():
    """This shell's PATH and locale, without the Python `uv run` puts first."""
    kept = {name: os.environ[name] for name in KEPT if name in os.environ}
    if "PATH" in kept:
        kept["PATH"] = os.pathsep.join(
            entry
            for entry in kept["PATH"].split(os.pathsep)
            if "/uv/python/" not in entry and ".venv" not in entry
        )
    return kept


def install():
    if not DESKTOP.exists():
        raise SystemExit(f"missing {DESKTOP}; build the desktop first")
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    PLIST.write_bytes(
        plistlib.dumps(
            {
                "Label": LABEL,
                "ProgramArguments": [str(DESKTOP), "--restore-agents"],
                "EnvironmentVariables": environment(),
                "RunAtLoad": True,
                "ProcessType": "Interactive",
                "StandardOutPath": str(LOG),
                "StandardErrorPath": str(LOG),
            }
        )
    )
    # Registering runs it once now (RunAtLoad), which restarts nothing that is
    # already running.
    launchctl("bootout", f"{DOMAIN}/{LABEL}")
    launchctl("bootstrap", DOMAIN, str(PLIST), check=True)
    print(f"installed {PLIST}; log {LOG}")


def uninstall():
    launchctl("bootout", f"{DOMAIN}/{LABEL}")
    PLIST.unlink(missing_ok=True)
    print("removed")


def status():
    if not PLIST.exists():
        print("not installed")
        return
    print(f"installed {PLIST}")
    if LOG.exists():
        lines = [
            line for line in LOG.read_text().splitlines() if "lapis restore:" in line
        ]
        print("\n".join(lines[-8:]) or "no restore yet")


def run():
    launchctl("kickstart", f"{DOMAIN}/{LABEL}", check=True)
    print(f"started; see {LOG}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("action", choices=["install", "uninstall", "status", "run"])
    action = parser.parse_args().action
    {"install": install, "uninstall": uninstall, "status": status, "run": run}[action]()


if __name__ == "__main__":
    main()
