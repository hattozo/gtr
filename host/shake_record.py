"""Records GTA's own camera shakes from a running game, for the guest to play back on the place's camera.

The host's camera follows the place's camera, so a shake has to be made in the guest; and it should be GTA's shake, not
one made up. The script shakes a camera of its own the way GTA shakes one (SHAKE_CAM, at full strength) and writes down
what that does to the camera each tick; this turns those into evenly spaced samples the guest reads.

    python host/shake_record.py

Writes GtrShakes.txt beside the guest and has the guest read it. The game must be running unpaused with the guest
connected. The recordings are GTA's and stay on this machine: build/ is not part of the project.
"""
import os
import sys
import time
from pathlib import Path

import numpy as np

from capture import running
from gtrframe import Link

# The shakes asked for by name; GTA ignores a name it doesn't know, and nothing is recorded for it
NAMES = ["ROCKET_EXPLOSION_SHAKE", "GRENADE_EXPLOSION_SHAKE", "LARGE_EXPLOSION_SHAKE", "MEDIUM_EXPLOSION_SHAKE", "SMALL_EXPLOSION_SHAKE"]
SECONDS = 3.0
RATE = 100.0
# A camera that moves less than this (metres, degrees) isn't shaking
STILL = 0.002
FOLDER = Path(os.environ["LOCALAPPDATA"]) / "Gtr"


def main():
    root = Path(__file__).resolve().parent.parent
    if not running():
        print("FAIL: the host's script isn't ticking (is the game running, unpaused?)")
        return 1
    link = Link()
    link.receive()
    link.send(t="host", op="bench", on=1)
    # Level and looking north, so that what the shake adds comes out in the camera's own terms
    link.send(t="host", op="benchcam", offset=[0, 0, 3.0], rot=[0, 0, 0], fov=50.0)
    time.sleep(2.0)
    text = "# GTA V's camera shakes at full strength, recorded by host/shake_record.py: metres right, forward, up; degrees pitch, roll, yaw\n"
    recorded = 0
    try:
        for name in NAMES:
            path = FOLDER / f"shake_{name}.txt"
            path.unlink(missing_ok=True)
            link.send(t="host", op="shaketrace", name=name, amp=1.0, seconds=SECONDS)
            time.sleep(SECONDS + 1.0)
            if not path.exists():
                print(f"{name}: the script wrote nothing")
                continue
            ticks = np.loadtxt(path, ndmin=2)
            # The first tick or two are the camera before the shake has reached it
            if len(ticks) < 10 or np.abs(ticks[:, 1:]).max() < STILL:
                print(f"{name}: GTA has no such shake")
                continue
            # Yaw comes back as a heading and can wrap
            ticks[:, 6] = (ticks[:, 6] + 180.0) % 360.0 - 180.0
            moving = np.flatnonzero(np.abs(ticks[:, 1:]).max(axis=1) >= STILL)
            end = ticks[min(moving[-1] + 1, len(ticks) - 1), 0] / 1000.0
            times = np.arange(0.0, end + 1.0 / RATE, 1.0 / RATE)
            samples = np.stack([np.interp(times, ticks[:, 0] / 1000.0, ticks[:, i]) for i in range(1, 7)], axis=1)
            samples[-1] = 0.0
            text += f"shake {name} {RATE:g}\n" + "".join(" ".join(f"{v:.5f}" for v in sample) + "\n" for sample in samples)
            recorded += 1
            print(f"{name}: {end:.2f} s, up to {np.abs(samples[:, :3]).max() * 100:.1f} cm and {np.abs(samples[:, 3:]).max():.2f} degrees")
    finally:
        link.send(t="host", op="bench", on=0)
        time.sleep(0.3)
    if recorded == 0:
        print("FAIL: nothing recorded; the guest keeps its own shake")
        return 1
    (root / "build" / "vanadium" / "GtrGuest" / "GtrShakes.txt").write_text(text)
    link.send(t="shake", reload=True)
    time.sleep(0.3)
    link.close()
    print(f"{recorded} shakes written, and read by the running guest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
