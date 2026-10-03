"""Checks, without GTA, that a see-through part of the guest's comes out as partly covering, at its own depth, where nothing
opaque of the guest's is behind it: a brick at Transparency 0.5 in front of the camera, against nothing.

The guest must be running, empty:  tools\run-guest.ps1 -Empty

    python host/test_transparency.py
"""
import sys
import time

import numpy as np

from fakehost import ORIGIN, look_at
from gtrframe import FrameReader, Link
from lua import run

WIDTH, HEIGHT = 640, 360


def main():
    link = Link()
    link.receive()
    reader = FrameReader()
    link.send(t="origin", pos=ORIGIN.tolist())
    link.send(t="clear")
    centre = ORIGIN + [0.0, 6.0, 1.0]
    link.send(t="brick", id="glass", pos=centre.tolist(), size=[2.0, 2.0, 2.0], color=[0.2, 0.4, 0.9])
    eye = ORIGIN + [0.0, 0.0, 1.0]
    forward, right, up = look_at(eye, centre)

    def frame(identity):
        link.send(t="cam", id=identity, pos=eye.tolist(), right=right.tolist(), fwd=forward.tolist(), up=up.tolist(), fov=50.0, w=WIDTH, h=HEIGHT)
        return reader.wait_for(identity)

    frame(1)
    run(link, "workspace.HostBricks.glass.Transparency = 0.5 return 1")
    time.sleep(0.3)
    seen = frame(2)
    middle = (HEIGHT // 2, WIDTH // 2)
    alpha = seen.color[middle][3] / 255
    depth = float(seen.depth[middle])
    corner = seen.color[5, 5][3] / 255
    expected = 6.0 - 1.0
    ok = 0.35 < alpha < 0.65 and abs(depth - expected) < 0.2 and corner == 0
    print(f"{'ok  ' if ok else 'FAIL'} a half see-through brick covers half the pixel at its own depth: alpha {alpha:.2f}, depth {depth:.2f} m "
          f"(its face is {expected:.2f} m off), empty beside it: {corner:.2f}")
    link.send(t="clear")
    time.sleep(0.2)
    reader.close()
    link.close()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
