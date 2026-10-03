"""Checks the host's compositor (the ReShade add-on and effect) end to end, without GTA.

Runs build/gta/fakegta/fakegta.exe, a D3D11 window with ReShade that draws the host's scene and has the guest draw the test
bricks. The picture the add-on saves must show guest pixels where the bricks are in front of everything the host drew, and the
host's own colours everywhere else, including where its pillar stands in front of a brick. The guest must be running.

    python host/test_compositor.py [outdir]
"""
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from fakehost import BRICKS, GROUND_Z, HOST_PILLAR, ORIGIN, erode, look_at, pixel_rays, trace_box, trace_boxes

WIDTH, HEIGHT, FOV = 1280, 720, 50.0
SECONDS = 6.0
SKY = (0.60, 0.70, 0.85)
HOST_PALETTE = np.array([SKY, (0.42, 0.42, 0.44), (0.30, 0.30, 0.32), HOST_PILLAR[3]])
# One 8-bit step of rounding either way
COLOR_TOLERANCE = 2.5 / 255
EDGE_PIXELS = 3
MIN_FRACTION = 0.995
# Magenta pixels in a picture taken while the camera swings: at least this many when the guest lags, at most this many (the
# odd pixel on an edge) when it doesn't
MAGENTA_WHEN_LOOSE = 400
MAGENTA_WHEN_LOCKED = 30
# The stand-in's sun (kSun and kSunShadow in fakegta.cpp), and how much of the ground the bricks shade must come out that
# much darker: the shadows are found in the picture, so the far side of a thick brick can be missed
SUN = np.array([-0.48, 0.36, 0.80])
SUN_SHADOW = 0.5
SHADOW_TOLERANCE = 0.08
MIN_SHADOWED = 0.9


def shaded_by(points, box):
    """Whether the way from each point to the sun passes through a box."""
    centre, size, yaw, _ = box
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    axes = np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]])
    origin = (points - centre) @ axes.T
    direction = axes @ SUN
    half = np.asarray(size) / 2
    t1, t2 = (-half - origin) / direction, (half - origin) / direction
    near, far = np.minimum(t1, t2).max(axis=-1), np.maximum(t1, t2).min(axis=-1)
    return (far >= near) & (far > 0)


def is_host_color(picture):
    distance = np.abs(picture[..., None, :] - HOST_PALETTE).max(axis=-1)
    return distance.min(axis=-1) <= COLOR_TOLERANCE


def main():
    root = Path(__file__).resolve().parent.parent
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else root / "out" / "compositor").resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    fake = root / "build" / "gta" / "fakegta"

    target = ORIGIN + [0.0, 6.5, 1.0]
    azimuth = math.radians(-55)
    eye = target + [10.0 * math.cos(azimuth), 10.0 * math.sin(azimuth), 1.5]
    capture = outdir / "capture.bmp"
    capture.unlink(missing_ok=True)
    arguments = [f"{v:.6f}" for v in (*eye, *target, FOV, SECONDS)] + [str(capture)]
    result = subprocess.run([str(fake / "fakegta.exe"), *arguments], cwd=fake, capture_output=True, text=True, timeout=120)
    print(result.stdout.strip())
    if result.returncode != 0 or not capture.exists():
        print("FAIL: fakegta saved no picture (is ReShade's dxgi.dll beside it, and did the add-on load? see ReShade.log)")
        return 1

    picture = np.asarray(Image.open(capture).convert("RGB")).astype(float) / 255
    if picture.shape[:2] != (HEIGHT, WIDTH):
        print(f"FAIL: the picture is {picture.shape[1]}x{picture.shape[0]}")
        return 1
    Image.fromarray((picture * 255 + 0.5).astype(np.uint8)).save(outdir / "capture.png")

    forward, right, up = look_at(eye, target)
    rays = pixel_rays(forward, right, up, FOV, WIDTH, HEIGHT)
    guest_depth, _ = trace_boxes(eye, rays, list(BRICKS.values()))
    pillar_depth = trace_box(eye, rays, HOST_PILLAR)
    with np.errstate(divide="ignore", invalid="ignore"):
        ground = (GROUND_Z - eye[2]) / rays[..., 2]
    host_depth = np.minimum(np.where((rays[..., 2] < 0) & (ground > 0), ground, np.inf), pillar_depth)

    # Compared away from every outline, where a pixel may belong to either side
    guest_in_front = erode(guest_depth < host_depth - 0.05, EDGE_PIXELS)
    pillar_hides_guest = erode(np.isfinite(guest_depth) & (pillar_depth < guest_depth - 0.05), EDGE_PIXELS)
    host_only = erode(~np.isfinite(guest_depth), EDGE_PIXELS)

    host_colored = is_host_color(picture)
    pillar_colored = np.abs(picture - HOST_PILLAR[3]).max(axis=-1) <= COLOR_TOLERANCE
    checks = {
        "guest shows where it is in front": (~host_colored[guest_in_front]).mean() if guest_in_front.any() else 0.0,
        "host's pillar hides the guest behind it": pillar_colored[pillar_hides_guest].mean() if pillar_hides_guest.any() else 0.0,
        "host untouched where the guest drew nothing": host_colored[host_only].mean(),
    }
    counts = {"guest shows where it is in front": int(guest_in_front.sum()),
              "host's pillar hides the guest behind it": int(pillar_hides_guest.sum()),
              "host untouched where the guest drew nothing": int(host_only.sum())}
    passed = True
    for name, fraction in checks.items():
        ok = fraction >= MIN_FRACTION and counts[name] > 0
        passed &= bool(ok)
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {fraction * 100:.2f}% of {counts[name]} pixels")

    # With a sun, the ground is darker where a brick stands between it and the sun, and nowhere else
    sun_path = outdir / "sun.bmp"
    sun_path.unlink(missing_ok=True)
    subprocess.run([str(fake / "fakegta.exe"), *arguments[:-1], str(sun_path), "sun"], cwd=fake, capture_output=True, text=True, timeout=120)
    if not sun_path.exists():
        print("FAIL: fakegta saved no picture with a sun")
        return 1
    sunny = np.asarray(Image.open(sun_path).convert("RGB")).astype(float) / 255
    Image.fromarray((sunny * 255 + 0.5).astype(np.uint8)).save(outdir / "sun.png")
    with np.errstate(divide="ignore", invalid="ignore"):
        on_ground = (rays[..., 2] < 0) & (ground > 0) & (ground < pillar_depth) & ~np.isfinite(guest_depth)
        points = eye + rays * np.where(on_ground, ground, 0.0)[..., None]
        shaded = np.zeros(on_ground.shape, bool)
        for brick in BRICKS.values():
            shaded |= shaded_by(points, brick)
        ratio = sunny.sum(axis=-1) / picture.sum(axis=-1)
    in_shadow = erode(on_ground & shaded, EDGE_PIXELS + 2)
    in_light = erode(on_ground & ~shaded, EDGE_PIXELS + 2)
    darkened = (np.abs(ratio[in_shadow] - (1 - SUN_SHADOW)) < SHADOW_TOLERANCE).mean() if in_shadow.any() else 0.0
    untouched = (np.abs(ratio[in_light] - 1.0) < 0.02).mean() if in_light.any() else 0.0
    ok = darkened >= MIN_SHADOWED and in_shadow.sum() > 2000
    passed &= bool(ok)
    print(f"{'ok  ' if ok else 'FAIL'} the bricks' shadows fall on the host's ground: {darkened * 100:.2f}% of {int(in_shadow.sum())} pixels")
    ok = untouched >= MIN_FRACTION
    passed &= bool(ok)
    print(f"{'ok  ' if ok else 'FAIL'} the ground in the sun is untouched: {untouched * 100:.2f}% of {int(in_light.sum())} pixels")

    # The pillar painted red, as the script paints a person or a prop the paintball gun hit (GtrPaintState): where it shows it
    # takes the red, and the host's ground, round its foot too, is as it was
    paint_path = outdir / "paint.bmp"
    paint_path.unlink(missing_ok=True)
    subprocess.run([str(fake / "fakegta.exe"), *arguments[:-1], str(paint_path), "paint"], cwd=fake, capture_output=True, text=True, timeout=120)
    if not paint_path.exists():
        print("FAIL: fakegta saved no picture with paint")
        return 1
    painted = np.asarray(Image.open(paint_path).convert("RGB")).astype(float) / 255
    Image.fromarray((painted * 255 + 0.5).astype(np.uint8)).save(outdir / "paint.png")
    pillar_shows = erode(np.isfinite(pillar_depth) & (pillar_depth <= host_depth) & ~(guest_depth < pillar_depth), EDGE_PIXELS)
    ground_shows = erode((rays[..., 2] < 0) & (ground > 0) & (ground < pillar_depth) & ~np.isfinite(guest_depth), EDGE_PIXELS)
    red = ((painted[..., 0] > 0.8) & (painted[..., 1] < 0.3) & (painted[..., 2] < 0.3))[pillar_shows].mean() if pillar_shows.any() else 0.0
    same = (np.abs(painted - picture).max(axis=-1) <= COLOR_TOLERANCE)[ground_shows].mean() if ground_shows.any() else 0.0
    ok = red >= MIN_FRACTION and pillar_shows.sum() > 500
    passed &= bool(ok)
    print(f"{'ok  ' if ok else 'FAIL'} a painted prop is painted: {red * 100:.2f}% of {int(pillar_shows.sum())} pixels red")
    ok = same >= MIN_FRACTION
    passed &= bool(ok)
    print(f"{'ok  ' if ok else 'FAIL'} and the ground round it isn't: {same * 100:.2f}% of {int(ground_shows.sum())} pixels unchanged")

    # With the camera swinging, a guest frame put with a host picture of another moment is off by degrees. The stand-in
    # draws a magenta box inside each brick, which shows wherever the guest's brick isn't exactly over it. Run the way things
    # were, taking the newest frame, plenty must show, or this check proves nothing; with each picture marked for the frame
    # that goes with it, none may.
    def magenta_while_orbiting(mode):
        picture_path = outdir / f"{mode}.bmp"
        picture_path.unlink(missing_ok=True)
        orbit = subprocess.run([str(fake / "fakegta.exe"), *arguments[:-1], str(picture_path), mode], cwd=fake, capture_output=True, text=True, timeout=120)
        if orbit.returncode != 0 or not picture_path.exists():
            print(f"FAIL: fakegta saved no picture in mode {mode}: {orbit.stdout.strip()}")
            return None
        orbiting = np.asarray(Image.open(picture_path).convert("RGB")).astype(float) / 255
        Image.fromarray((orbiting * 255 + 0.5).astype(np.uint8)).save(outdir / f"{mode}.png")
        return int(((orbiting[..., 0] > 0.8) & (orbiting[..., 1] < 0.25) & (orbiting[..., 2] > 0.8)).sum())

    loose = magenta_while_orbiting("orbit-unsynced")
    locked = magenta_while_orbiting("orbit")
    ok = loose is not None and loose > MAGENTA_WHEN_LOOSE
    passed &= ok
    print(f"{'ok  ' if ok else 'FAIL'} swinging the camera with the newest frame shows the lag: {loose} magenta pixels")
    ok = locked is not None and locked <= MAGENTA_WHEN_LOCKED
    passed &= ok
    print(f"{'ok  ' if ok else 'FAIL'} swinging the camera with marked pictures stays locked: {locked} magenta pixels")

    print("PASS" if passed else "FAIL", "->", outdir)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
