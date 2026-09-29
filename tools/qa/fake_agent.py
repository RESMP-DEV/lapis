#!/usr/bin/env python3
"""A line-based stand-in for an agent CLI, for lapis GUI and soak tests.

Installed under harness names (kimi, grok, opencode, ...) on a QA-only PATH.
Commands typed at its prompt exercise terminal and lifecycle behavior:

  slow    print progress for 20 seconds, then return to the prompt
  flood   print 20000 long lines as fast as possible
  count   print 120 numbered lines (scrollback for phone history tests)
  size ID report the actual PTY grid with a caller-provided observation ID
  wide    print wide, combining and right-to-left text
  alt     draw on the alternate screen for 5 seconds
  mouse   a full-screen program that reports the mouse (SGR), as Claude Code's
          full-screen mode does, until the wheel turns; then says what came
  title   set an unusual window title
  bell    ring the terminal bell
  hang    ignore SIGHUP and SIGTERM and sleep (tests forced termination)
  crash   exit with SIGSEGV
  exit    exit normally
  other   echo the line after half a second

LAPIS_FAKE_BURST=N prints a 3-second burst every N seconds in the background,
so a soak can keep many agents moderately busy without typing.

At start it reports a conversation the way agent session hooks do for terminal
restore tools (OSC 1337 SetUserVar=agent_checkpoint), reusing the one passed
with --session, --resume, -r or --conversation so a restored agent can say what
it resumed.
"""

import base64
import json
import os
import select
import signal
import sys
import termios
import threading
import time
import tty
import uuid

NAME = os.path.basename(sys.argv[0])
OUTPUT_LOCK = threading.Lock()


def write(text, *, flush=False):
    with OUTPUT_LOCK:
        sys.stdout.write(text)
        if flush:
            sys.stdout.flush()


def say(text=""):
    write(text + "\n", flush=True)


def bursts(period, stop):
    count = 0
    while not stop.wait(period):
        count += 1
        for step in range(30):
            say(f"\x1b[36mburst {count}.{step}\x1b[0m " + "·" * (step % 40))
            if stop.wait(0.1):
                return


def wheel_events(seconds):
    """Raw input until mouse events arrive (and a moment after, for the rest
    of a gesture), or `seconds` pass."""
    descriptor = sys.stdin.fileno()
    saved = termios.tcgetattr(descriptor)
    received = b""
    try:
        tty.setraw(descriptor)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([descriptor], [], [], 0.1)
            if ready:
                received += os.read(descriptor, 4096)
                if received.count(b"M") and deadline - time.monotonic() > 0.6:
                    deadline = time.monotonic() + 0.6
    finally:
        termios.tcsetattr(descriptor, termios.TCSADRAIN, saved)
    return received


def run(command, line):
    if command.startswith("size "):
        size = os.get_terminal_size()
        say(f"grid-{line.partition(' ')[2]} {size.columns} {size.lines}")
    elif command == "count":
        for number in range(1, 121):
            say(f"count {number}")
    elif command == "slow":
        for step in range(20):
            say(f"working {step + 1}/20")
            time.sleep(1)
    elif command == "flood":
        for number in range(20000):
            write(f"flood {number:05d} " + "x" * 90 + "\n")
        write("", flush=True)
    elif command == "wide":
        say("wide: 漢字かなカナ 한국어 😀👍🏽 é ä مرحبا بالعالم שלום")
        say("box: ┌──┬──┐ │▓▓│░░│ └──┴──┘")
    elif command == "alt":
        write("\x1b[?1049h\x1b[2J\x1b[H")
        for row in range(10):
            write(f"\x1b[{row + 2};4Halternate screen row {row}")
        write("", flush=True)
        time.sleep(5)
        write("\x1b[?1049l")
        say("left the alternate screen")
    elif command == "mouse":
        write("\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[2J\x1b[Hmouse ready", flush=True)
        received = wheel_events(30)
        write("\x1b[?1006l\x1b[?1000l\x1b[?1049l")
        events = received.split(b"\x1b")[1:]
        first = events[0].decode("ascii", "replace") if events else "nothing"
        say(f"mouse got {len(events)} events, first ESC{first}")
    elif command == "title":
        write("\x1b]0;‮eltit desrever \x07")
        say("title set")
    elif command == "bell":
        write("\a")
        say("bell rung")
    elif command == "hang":
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        say("hanging; SIGHUP and SIGTERM are ignored")
        while True:
            time.sleep(60)
    elif command == "crash":
        os.kill(os.getpid(), signal.SIGSEGV)
    elif command == "exit":
        sys.exit(0)
    else:
        time.sleep(0.5)
        say(f"echo: {line}")


def checkpoint():
    """Emit a session-start checkpoint; return its identity and resumed state."""
    arguments = sys.argv[1:]
    resumed = None
    for flag in ("--session", "--resume", "-r", "--conversation"):
        if flag in arguments and arguments.index(flag) + 1 < len(arguments):
            resumed = arguments[arguments.index(flag) + 1]
    conversation = resumed or str(uuid.uuid4())
    value = json.dumps({"agent": NAME, "session_id": conversation}).encode()
    sys.stdout.write(
        "\x1b]1337;SetUserVar=agent_checkpoint="
        + base64.b64encode(value).decode()
        + "\x07"
    )
    return conversation, resumed is not None


def main():
    conversation, resumed = checkpoint()
    say(f"\x1b[1m{NAME}\x1b[0m (lapis fake agent) pid {os.getpid()} cwd {os.getcwd()}")
    say(("resumed conversation " if resumed else "new conversation ") + conversation)
    period = float(os.environ.get("LAPIS_FAKE_BURST", "0") or 0)
    stop = threading.Event()
    worker = None
    if period > 0:
        worker = threading.Thread(target=bursts, args=(period, stop))
        worker.start()
    try:
        while True:
            try:
                write("\x1b[32m›\x1b[0m ", flush=True)
                line = input()
            except EOFError:
                return 0
            run(line.strip().lower(), line)
    finally:
        stop.set()
        if worker is not None:
            worker.join()


if __name__ == "__main__":
    sys.exit(main())
