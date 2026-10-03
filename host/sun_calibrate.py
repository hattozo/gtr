"""Measures, in a running game, where GTA's sun and moon are through its day and how dark their shadows are.

GTA has no native that says where its sun is, and the guest's shadows on GTA's world have to fall the way GTA's own do. So
GTA's player is stood on open level ground (the airport's runway), looked at from straight above, and photographed at each
half hour twice: as it is, and faded out, which takes its shadow away too. What differs between the two pictures, away from
the player itself, is the shadow: its direction is the sun's, its length gives the sun's height, and how much darker it is
than the ground gives the shadow's strength.

    python host/sun_calibrate.py [outdir]           measure in the running game
    python host/sun_calibrate.py --again [outdir]   work the table out again from the pictures of the last measurement

Writes host/gta/GtrSun.txt (which install.ps1 installs) and the copy a running script reads, and has the script read it.
To measure, the game must be running unpaused with the guest connected. It leaves GTA at noon under a clear sky.
"""
import math
import os
import re
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

from capture import capture, running
from gtrframe import Link

# Los Santos International's long runway
PLACE = [-1336.0, -3044.0, 13.9]
HOURS = np.arange(0.0, 24.0, 0.5)
# The script's bench camera: straight above the player, north at the top of the picture
CAMERA_HEIGHT = 30.0
CAMERA_FOV = 50.0
# GTA's player, about: his height, and the circle on the ground he covers seen from above
PLAYER_HEIGHT = 1.8
PLAYER_RADIUS = 0.6
# A shadow starts at the player's feet, so it is looked for in a ring round him, by direction: it is there when the ground
# one way is this much darker with the player than without, and that many times darker than it is round the ring at large
RING = (0.7, 1.6)
SECTORS = 72
MIN_DARKER = 0.03
MIN_STANDING_OUT = 5.0
# The shadow is then followed outwards along a strip this wide, until it has faded to this much of what it is by the feet
STRIP_HALF_WIDTH = 0.3
STEP = 0.1
FADED = 0.4
# A sun lower than this isn't believed: the shadow is longer than can be followed over the ground's own markings
MIN_ELEVATION = 8.0
LOG = Path(os.environ["LOCALAPPDATA"]) / "Gtr" / "GtrHost.log"


def luminance(picture):
    return picture.astype(float) @ np.array([0.2126, 0.7152, 0.0722]) / 255.0


def smooth(image, radius=2):
    """The mean over a square round each pixel, which takes the game's grain out."""
    padded = np.pad(image, radius, mode="edge")
    total = np.zeros_like(image)
    for dy in range(2 * radius + 1):
        for dx in range(2 * radius + 1):
            total += padded[dy:dy + image.shape[0], dx:dx + image.shape[1]]
    return total / (2 * radius + 1) ** 2


def measure(with_player, without):
    """The shadow in a pair of pictures: (direction along the ground the shadow points, east and north; its length in
    metres; how much darker it is, 0 to 1), or None when there is none to see."""
    height, width = with_player.shape[:2]
    metres = 2.0 * CAMERA_HEIGHT * math.tan(math.radians(CAMERA_FOV) / 2) / height
    ratio = (smooth(luminance(with_player)) + 0.01) / (smooth(luminance(without)) + 0.01)
    # The game's exposure drifts between the two pictures; most of the ground is the same in both
    darker = 1.0 - ratio / np.median(ratio)
    ys, xs = np.mgrid[0:height, 0:width]
    east, north = (xs - width / 2 + 0.5) * metres, -(ys - height / 2 + 0.5) * metres
    distance = np.hypot(east, north)

    ring = (distance > RING[0]) & (distance < RING[1])
    sector = (np.arctan2(east[ring], north[ring]) / (2 * math.pi) * SECTORS).astype(int) % SECTORS
    by_sector = np.bincount(sector, weights=darker[ring], minlength=SECTORS) / np.maximum(np.bincount(sector, minlength=SECTORS), 1)
    # Three sectors together, since a shadow is wider than one
    spread = (by_sector + np.roll(by_sector, 1) + np.roll(by_sector, -1)) / 3
    best = int(np.argmax(spread))
    noise = np.median(np.abs(spread - np.median(spread))) + 1e-4
    if spread[best] < MIN_DARKER or (spread[best] - np.median(spread)) / noise < MIN_STANDING_OUT:
        return None
    near = [(best + offset) % SECTORS for offset in (-2, -1, 0, 1, 2)]
    weights = np.maximum(by_sector[near], 0.0)
    angles = (best + np.array([-2, -1, 0, 1, 2]) + 0.5) * 2 * math.pi / SECTORS
    direction = np.array([(weights * np.sin(angles)).sum(), (weights * np.cos(angles)).sum()])
    direction /= np.linalg.norm(direction)

    along = east * direction[0] + north * direction[1]
    off = np.abs(east * direction[1] - north * direction[0])
    strip = (off < STRIP_HALF_WIDTH) & (along > PLAYER_RADIUS)
    steps = (along[strip] / STEP).astype(int)
    profile = np.bincount(steps, weights=darker[strip]) / np.maximum(np.bincount(steps), 1)
    first = int(RING[0] / STEP)
    by_feet = float(np.median(profile[first:int(RING[1] / STEP)]))
    faded = np.flatnonzero(profile[first:] < by_feet * FADED)
    length = (first + (int(faded[0]) if faded.size else len(profile) - first)) * STEP
    # How dark the shadow is where it is whole: the strip is wider than the shadow, and takes in its soft edges
    whole = strip & (along > RING[0]) & (along < length)
    return direction, float(length), float(np.percentile(darker[whole], 90)) if whole.any() else by_feet


def log_matches(pattern):
    return list(re.finditer(pattern, LOG.read_text(errors="replace")))


def wait_for_log(pattern, already, timeout=40.0):
    """The next time the script's log says something, given how many times it had said it before."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = log_matches(pattern)
        if len(found) > already:
            return found[-1]
        time.sleep(0.3)
    raise TimeoutError(f"the script's log never said: {pattern}")


def row(hour, found):
    """A row of the table for what was measured at an hour, said aloud."""
    if found is not None:
        direction, length, darkness = found
        elevation = math.atan2(PLAYER_HEIGHT, max(length, 0.05))
        if math.degrees(elevation) >= MIN_ELEVATION:
            print(f"{hour:4.1f} h: shadow {length:5.2f} m towards {math.degrees(math.atan2(direction[0], direction[1])) % 360:5.1f} degrees from north, "
                  f"sun {math.degrees(elevation):4.1f} degrees up, {darkness * 100:3.0f}% darker")
            return hour, (-direction[0] * math.cos(elevation), -direction[1] * math.cos(elevation), math.sin(elevation)), darkness
    print(f"{hour:4.1f} h: no shadow")
    return hour, (0.0, 0.0, 1.0), 0.0


def write(root, rows):
    if sum(1 for _, _, darkness in rows if darkness > 0) < 8:
        print("FAIL: hardly any shadows were found; nothing written")
        return False
    text = ("# Where GTA V's sun and moon are through its day, measured in the game by host/sun_calibrate.py.\n"
            "# hour, the direction towards the light (east, north, up), how much darker its shadows are\n")
    text += "".join(f"{hour:.2f} {sun[0]:.4f} {sun[1]:.4f} {sun[2]:.4f} {darkness:.3f}\n" for hour, sun, darkness in rows)
    for path in (root / "host" / "gta" / "GtrSun.txt", LOG.parent / "GtrSun.txt"):
        path.write_text(text)
    return True


def main():
    again = "--again" in sys.argv[1:]
    arguments = [a for a in sys.argv[1:] if a != "--again"]
    root = Path(__file__).resolve().parent.parent
    outdir = Path(arguments[0] if arguments else root / "out" / "sun").resolve()
    outdir.mkdir(parents=True, exist_ok=True)

    def load(name):
        return np.asarray(Image.open(outdir / f"{name}.png").convert("RGB"))

    if again:
        rows = [row(hour, measure(load(f"{hour:04.1f}_player"), load(f"{hour:04.1f}_empty"))) for hour in HOURS]
        if not write(root, rows):
            return 1
        try:
            link = Link()
            link.receive()
            link.send(t="host", op="sunreload")
            time.sleep(0.3)
            link.close()
        except OSError:
            pass
        print("written")
        return 0

    if not running():
        print("FAIL: the host's script isn't ticking (is the game running, unpaused?)")
        return 1

    def picture(name):
        capture(outdir / f"{name}.png")
        return load(name)

    link = Link()
    link.receive()
    grounded = r"bench ground ([-\d.]+), within 12 m ([-\d.]+)\.\.([-\d.]+)"
    already = len(log_matches(grounded))
    link.send(t="host", op="weather", name="EXTRASUNNY")
    link.send(t="host", op="bench", on=1, pos=PLACE)
    rows = []
    try:
        ground = wait_for_log(grounded, already)
        low, high = float(ground.group(2)), float(ground.group(3))
        print(f"on the runway; the ground within 12 m is level to {high - low:.2f} m")
        link.send(t="host", op="benchcam", offset=[0, 0, CAMERA_HEIGHT], rot=[-90, 0, 0], fov=CAMERA_FOV)
        # The map around the place streams in
        time.sleep(6.0)

        def pair(name, hour):
            link.send(t="host", op="time", clock=[int(hour), int(round(hour % 1 * 60))])
            link.send(t="host", op="pedalpha", value=255)
            time.sleep(0.5)
            # GTA keeps the shadows it has drawn while the camera is still, whatever the clock does: seen from somewhere
            # else for a moment, it draws them again from where the sun now is
            for offset in ([8, 8, CAMERA_HEIGHT + 10], [-8, 5, CAMERA_HEIGHT - 8], [0, 0, CAMERA_HEIGHT]):
                link.send(t="host", op="benchcam", offset=offset, rot=[-90, 0, 0], fov=CAMERA_FOV)
                time.sleep(0.3)
            time.sleep(1.0)
            shown = picture(f"{name}_player")
            link.send(t="host", op="pedalpha", value=0)
            time.sleep(0.45)
            return shown, picture(f"{name}_empty")

        for hour in HOURS:
            rows.append(row(hour, measure(*pair(f"{hour:04.1f}", hour))))

        # Whether a player faded almost out still casts a shadow: GTA's own shadow for the character, if so
        shown, empty = pair("fade", 12.0)
        for alpha in (1, 16, 64, 128):
            link.send(t="host", op="pedalpha", value=alpha)
            time.sleep(0.45)
            faded = picture(f"fade_{alpha}")
            height, width = faded.shape[:2]
            centre = (slice(height // 2 - 8, height // 2 + 8), slice(width // 2 - 8, width // 2 + 8))
            seen = float(np.abs(luminance(faded)[centre] - luminance(empty)[centre]).mean())
            print(f"player at alpha {alpha}: {'a shadow' if measure(faded, empty) else 'no shadow'}; the player itself differs from the ground by {seen:.3f}")
    finally:
        link.send(t="host", op="pedalpha", value=255)
        link.send(t="host", op="time", clock=[12, 0])
        link.send(t="host", op="bench", on=0)
        time.sleep(0.3)

    if not write(root, rows):
        return 1
    link.send(t="host", op="sunreload")
    time.sleep(0.3)
    link.close()
    print("written, and read by the running script; pictures in", outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
