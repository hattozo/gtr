"""Sends messages to the guest's link: one JSON object per line on standard input, or each argument as one.

    python host/send.py '{"t":"host","op":"time","clock":[12,0]}'

A {"t":"host", ...} message is passed on to the host's script; see handle() in host/gta/src/script.cpp for its ops.
"""
import json
import sys
import time

from gtrframe import Link


def main():
    # PowerShell puts a byte order mark in front of what it pipes
    lines = sys.argv[1:] or [line for line in sys.stdin.buffer.read().decode("utf-8-sig").splitlines() if line.strip()]
    link = Link()
    link.receive()
    for line in lines:
        link.send(**json.loads(line))
    # Long enough for the guest to read them before the connection closes
    time.sleep(0.3)
    link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
