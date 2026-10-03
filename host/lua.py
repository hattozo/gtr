"""Runs Luau in the guest, like a line in Studio's command bar, and prints what it returns.

    python host/lua.py "return workspace.Gravity"
    python host/lua.py path/to/file.lua
"""
import sys
import time
from pathlib import Path

from gtrframe import Link


def run(link, source, timeout=10.0):
    command_id = time.monotonic_ns()
    link.send(t="lua", id=command_id, src=source)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        answer = link.receive(timeout)
        if answer.get("t") == "lua" and answer.get("id") == command_id:
            if answer["error"] is not None:
                raise RuntimeError(answer["error"])
            return answer["result"]
    raise TimeoutError("the guest did not answer")


def main():
    argument = sys.argv[1]
    source = Path(argument).read_text() if argument.endswith(".lua") else argument
    link = Link()
    link.receive()
    # The reply's holder lives under the host camera, which exists from the start
    print(run(link, source))
    link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
