"""Checks that a frame shows the character at the same step as the camera it was drawn from.

The place's camera follows the character rigidly, so walking sideways must leave the character where it was in the picture.
A frame drawn after the physics step, with the camera from before it, shows the character a step ahead instead: a sixth
of a metre at walking speed, which in the host is the character sliding over the host's ground whenever it moves.

Holds D, then A to come back, and compares where the character's head is in the frames meanwhile with where it is at rest.

    python host/test_step.py          without GTA: the guest must be running a place (tools\\run-guest.ps1)
    python host/test_step.py --live   beside the host's script, in a running game
"""
import math
import sys
import time

import numpy as np

from fakehost import ORIGIN
from gtrframe import FrameReader, Link

CELL = 0.5
HALF = 20.0
KEY_A, KEY_D = 97, 100
WIDTH, HEIGHT = 640, 360
# The head is the top of the character: this much of its height in the picture
HEAD_FRACTION = 0.15
# The first of a walk is the camera and the character getting under way
SETTLE_SECONDS = 0.5
WALK_SECONDS = 1.3
# A step of physics at walking speed is some 15 cm; the head's centre is found to a pixel or so
MAX_SLIP_METRES = 0.03


def head(frame):
    """Where the character's head is in a frame: metres to the right of the picture's centre, at the head's depth."""
    covered = frame.color[..., 3] > 127
    rows = np.flatnonzero(covered.any(axis=1))
    if rows.size < 8:
        return None
    top = rows[0]
    band = covered[top:top + max(2, int((rows[-1] - top) * HEAD_FRACTION))]
    ys, xs = np.nonzero(band)
    depth = float(np.median(frame.depth[top:top + band.shape[0]][band]))
    focal = frame.height / 2 / math.tan(math.radians(frame.fov_y) / 2)
    return (float(xs.mean()) - frame.width / 2) * depth / focal


def main():
    live = "--live" in sys.argv[1:]
    link = Link()
    link.receive()
    reader = FrameReader()

    if not live:
        link.send(t="origin", pos=ORIGIN.tolist())
        link.send(t="clear")
        steps = int(HALF / CELL)
        tiles = []
        for i in range(-steps, steps):
            for j in range(-steps, steps):
                tiles += [ORIGIN[0] + (i + 0.5) * CELL, ORIGIN[1] + (j + 0.5) * CELL, ORIGIN[2]]
        for start in range(0, len(tiles), 600):
            link.send(t="ground", cell=CELL, depth=3.0, tiles=tiles[start:start + 600])
        link.send(t="spawn", pos=ORIGIN.tolist())

    def watch(seconds):
        """The head's place in each new frame for a while. Without the game, this is the host that keeps frames coming."""
        seen, last = [], None
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not live:
                link.send(t="view", w=WIDTH, h=HEIGHT)
            link.drain(0.004)
            frame = reader.latest()
            if frame is None or frame.camera_id == last:
                continue
            last = frame.camera_id
            at = head(frame)
            if at is not None:
                seen.append(at)
        return seen

    watch(2.5 if not live else 0.3)
    rest = watch(0.6)
    if len(rest) < 5:
        print("FAIL: no frames with a character in them (is a place running, and has the character spawned?)")
        return 1
    rest = float(np.median(rest))

    slips = {}
    for name, key in (("right", KEY_D), ("left", KEY_A)):
        link.send(t="key", code=key, down=True)
        watch(SETTLE_SECONDS)
        walking = watch(WALK_SECONDS - SETTLE_SECONDS)
        link.send(t="key", code=key, down=False)
        watch(0.6)
        slips[name] = float(np.median(walking)) - rest if walking else float("nan")

    passed = all(abs(slip) <= MAX_SLIP_METRES for slip in slips.values())
    for name, slip in slips.items():
        print(f"{'ok  ' if abs(slip) <= MAX_SLIP_METRES else 'FAIL'} walking {name}, the character is {slip * 100:+.1f} cm from where it rests in the picture")
    print("PASS" if passed else "FAIL")
    reader.close()
    link.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
