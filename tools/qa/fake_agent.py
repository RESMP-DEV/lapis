#!/usr/bin/env python3
"""A line-based stand-in for an agent CLI, for lapis GUI and soak tests.

Installed under harness names (kimi, grok, opencode, ...) on a QA-only PATH.
Commands typed at its prompt exercise terminal and lifecycle behavior:

  slow    print progress for 20 seconds, then return to the prompt
  flood   print 20000 long lines as fast as possible
  wide    print wide, combining and right-to-left text
  alt     draw on the alternate screen for 5 seconds
  title   set an unusual window title
  bell    ring the terminal bell
  hang    ignore SIGHUP and SIGTERM and sleep (tests forced termination)
  crash   exit with SIGSEGV
  exit    exit normally
  other   echo the line after half a second

LAPIS_FAKE_BURST=N prints a 3-second burst every N seconds in the background,
so a soak can keep many agents moderately busy without typing.
"""

import os
import signal
import sys
import threading
import time

NAME = os.path.basename(sys.argv[0])


def say(text=""):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def bursts(period):
    count = 0
    while True:
        time.sleep(period)
        count += 1
        for step in range(30):
            say(f"\x1b[36mburst {count}.{step}\x1b[0m " + "·" * (step % 40))
            time.sleep(0.1)


def run(command, line):
    if command == "slow":
        for step in range(20):
            say(f"working {step + 1}/20")
            time.sleep(1)
    elif command == "flood":
        for number in range(20000):
            sys.stdout.write(f"flood {number:05d} " + "x" * 90 + "\n")
        sys.stdout.flush()
    elif command == "wide":
        say("wide: 漢字かなカナ 한국어 😀👍🏽 é ä مرحبا بالعالم שלום")
        say("box: ┌──┬──┐ │▓▓│░░│ └──┴──┘")
    elif command == "alt":
        sys.stdout.write("\x1b[?1049h\x1b[2J\x1b[H")
        for row in range(10):
            sys.stdout.write(f"\x1b[{row + 2};4Halternate screen row {row}")
        sys.stdout.flush()
        time.sleep(5)
        sys.stdout.write("\x1b[?1049l")
        say("left the alternate screen")
    elif command == "title":
        sys.stdout.write("\x1b]0;‮eltit desrever \x07")
        say("title set")
    elif command == "bell":
        sys.stdout.write("\a")
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


def main():
    say(f"\x1b[1m{NAME}\x1b[0m (lapis fake agent) pid {os.getpid()} cwd {os.getcwd()}")
    period = float(os.environ.get("LAPIS_FAKE_BURST", "0") or 0)
    if period > 0:
        threading.Thread(target=bursts, args=(period,), daemon=True).start()
    while True:
        try:
            line = input("\x1b[32m›\x1b[0m ")
        except EOFError:
            return 0
        run(line.strip().lower(), line)


if __name__ == "__main__":
    sys.exit(main())
