"""Photographs the guest's character through the passthrough: asks where it stands, looks at it from a few metres away and saves
the exported frame over a plain backdrop.

    python host/snap.py [out.png] [distance_m] [azimuth_degrees] [height_m]
"""
import math
import sys
import time

import numpy as np
from PIL import Image

from fakehost import look_at
from gtrframe import FrameReader, Link


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out/snap.png"
    distance = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    azimuth = math.radians(float(sys.argv[3]) if len(sys.argv) > 3 else -90.0)
    height = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    width, height_px = 1280, 720

    reader = FrameReader()
    link = Link()
    print("guest:", link.receive())
    link.send(t="origin", pos=[0.0, 0.0, 0.0])
    character = None
    deadline = time.monotonic() + 30
    while character is None and time.monotonic() < deadline:
        link.send(t="where")
        character = link.receive()["character"]
        if character is None:
            time.sleep(0.5)
    if character is None:
        print("the guest has no character")
        return 1
    print("character at (host metres):", [round(c, 2) for c in character])

    target = np.array(character)
    position = target + [distance * math.cos(azimuth), distance * math.sin(azimuth), height]
    forward, right, up = look_at(position, target)
    camera_id = int(time.time())
    link.send(t="cam", id=camera_id, pos=position.tolist(), right=right.tolist(), fwd=forward.tolist(), up=up.tolist(),
              fov=50.0, w=width, h=height_px)
    reader.wait_for(camera_id)
    time.sleep(1.0)
    frame = reader.wait_for(camera_id)

    covered = frame.color[..., 3:] > 127
    backdrop = np.full((height_px, width, 3), 96, np.uint8)
    Image.fromarray(np.where(covered, frame.color[..., :3], backdrop)).save(out)
    finite = frame.depth[covered[..., 0]]
    print(f"saved {out}: {int(covered.sum())} guest pixels, depth {finite.min():.2f}..{finite.max():.2f} m" if finite.size else
          f"saved {out}: the guest drew nothing")
    link.close()
    reader.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
