"""Measures, in a running game, whether the guest's frames are shown with the host's pictures they belong to.

The host's script puts a probe beyond the character: a magenta box GTA draws, inside a brick the guest draws. Shown together
the brick hides the box whatever the camera does. A frame apart, the box shows at the brick's edge while the camera turns.
This zooms the place's camera into first person (the character is otherwise in the way, and takes the box's colour from the
host's light around it), turns it left and right, and counts the magenta in the pictures, for each frame offset the script can apply
(its F6), and says whether the script's mark is in the pictures at all.

    python host/calibrate.py [outdir]

The game must be running unpaused with the guest connected, and nobody at its mouse meanwhile.
"""
import sys
import threading
import time
from pathlib import Path

import numpy as np
from PIL import Image

from capture import capture, running, set_debug
from gtrframe import Link
from lua import run

GUEST_AS_DRAWN = 4
CAMERA_DISTANCE = "local c = workspace.CurrentCamera return (c.CFrame.Position - c.Focus.Position).Magnitude"
CAMERA_YAW = "local l = workspace.CurrentCamera.CFrame.LookVector return math.deg(math.atan2(-l.X, -l.Z))"
FIRST_PERSON_STUDS = 1.0
# The probe's brick has to be in plain view, not in a wall of the host's: this much of the picture, or the camera is turned
# a quarter round and the probe put down again
MIN_BRICK_PIXELS = 4000
MARK_PIXELS = 8
OFFSETS = (0, 1, -1)
PICTURES = 10
SWING_PIXELS = 6.0
SWING_SECONDS = 0.5
RATE = 60.0


def magenta(picture):
    r, g, b = (picture[..., i].astype(int) for i in range(3))
    found = (r > 200) & (b > 200) & (g < 60)
    # The mark itself is magenta for two of its numbers
    found[:MARK_PIXELS, :2 * MARK_PIXELS] = False
    return int(found.sum())


def brick(picture):
    """How much of a picture is the probe's green brick."""
    r, g, b = (picture[..., i].astype(float) for i in range(3))
    return int(((g > 110) & (g > 1.6 * r) & (g > 1.6 * b)).sum())


def mark(picture):
    """The tick number the mark in a picture reads as, or None when there is no mark."""
    first = picture[MARK_PIXELS // 2, MARK_PIXELS // 2].astype(float) / 255
    second = picture[MARK_PIXELS // 2, MARK_PIXELS + MARK_PIXELS // 2].astype(float) / 255
    if (np.abs(first + second - 1.0) > 0.2).any() or (np.abs(first - 0.5) < 0.3).any():
        return None
    return int(first[0] > 0.5) + 2 * int(first[1] > 0.5) + 4 * int(first[2] > 0.5)


class Swing(threading.Thread):
    """Turns the place's camera left and right, as the mouse does in first person."""

    def __init__(self):
        super().__init__(daemon=True)
        self.link = Link()
        self.link.receive()
        self.stop = False

    def run(self):
        start = time.monotonic()
        while not self.stop:
            # Half a swing first, so the camera goes as far to one side of where it started as to the other
            direction = 1.0 if int((time.monotonic() - start) / SWING_SECONDS + 0.5) % 2 == 0 else -1.0
            self.link.send(t="mouse", pos=[0.5, 0.5], delta=[direction * SWING_PIXELS, 0])
            self.link.drain(1.0 / RATE)
        time.sleep(0.2)
        self.link.close()


def main():
    root = Path(__file__).resolve().parent.parent
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else root / "out" / "calibrate").resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    if not running():
        print("FAIL: the host's script isn't ticking (is the game running, unpaused?)")
        return 1

    def picture(name):
        capture(outdir / f"{name}.png")
        return np.asarray(Image.open(outdir / f"{name}.png").convert("RGB"))

    link = Link()
    link.receive()
    link.send(t="host", op="markoffset", value=0)
    # The guest as it drew itself: the host's light around the box would colour the brick with it
    set_debug(view=GUEST_AS_DRAWN, show_mark=True)
    clicks = 0
    while float(run(link, CAMERA_DISTANCE)) > FIRST_PERSON_STUDS and clicks < 40:
        link.send(t="wheel", delta=1.0)
        clicks += 1
        time.sleep(0.15)
    time.sleep(1.0)

    def turn(degrees):
        start = float(run(link, CAMERA_YAW))
        for _ in range(600):
            if abs((float(run(link, CAMERA_YAW)) - start + 180.0) % 360.0 - 180.0) >= degrees:
                break
            link.send(t="mouse", pos=[0.5, 0.5], delta=[12, 0])
            time.sleep(1.0 / RATE)

    try:
        for attempt in range(4):
            link.send(t="host", op="probe", on=1)
            time.sleep(1.0)
            still = picture("still")
            if brick(still) >= MIN_BRICK_PIXELS:
                break
            link.send(t="host", op="probe", on=0)
            turn(90.0)
        else:
            print("FAIL: the probe's brick is out of sight whichever way the camera faces; stand somewhere more open")
            return 1
        print(f"the probe's brick covers {brick(still)} pixels")
        print(f"at rest: {magenta(still)} magenta pixels (the brick should hide the box: 0), mark reads {mark(still)}")

        results = {}
        for offset in OFFSETS:
            link.send(t="host", op="markoffset", value=offset)
            swing = Swing()
            swing.start()
            time.sleep(0.6)
            counts, marks, bricks = [], [], []
            for i in range(PICTURES):
                taken = picture(f"offset{offset:+d}_{i}")
                counts.append(magenta(taken))
                marks.append(mark(taken))
                bricks.append(brick(taken))
                time.sleep(0.13)
            swing.stop = True
            swing.join()
            time.sleep(0.5)
            results[offset] = counts
            print(f"offset {offset:+d}: magenta {counts} (mean {int(np.mean(counts))}), marks read {marks}, brick in view {min(bricks)}..{max(bricks)} pixels")

        best = min(results, key=lambda offset: np.mean(results[offset]))
        link.send(t="host", op="markoffset", value=best)
        print(f"least magenta at offset {best:+d}, which the script now uses; pictures in {outdir}")
    finally:
        set_debug(view=0, show_mark=False)
        link.send(t="host", op="probe", on=0)
        for _ in range(clicks):
            link.send(t="wheel", delta=-1.0)
            time.sleep(0.15)
        time.sleep(0.3)
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
