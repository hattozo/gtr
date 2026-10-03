"""A stand-in for GTA V that checks the guest without the game.

It places bricks through the link at known host coordinates, flies a camera round them using GTA's conventions (metres, Z up),
and compares each exported frame with its own ray-traced prediction of where those bricks must appear and how far away. It then
composites the guest over a synthetic host scene (a checkerboard ground and a pillar only the host has) by depth, the way the
real compositor will, and writes the pictures.

    python host/fakehost.py [outdir]

Exit code 0 when every pose passes.
"""
import json
import math
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

from gtrframe import EMPTY_DEPTH, FrameReader, Link

# Somewhere in Los Santos, so the coordinates are as large as the real ones
ORIGIN = np.array([-75.0, -818.0, 326.0])
GROUND_Z = ORIGIN[2]

# Host-space boxes: centre, size along the box's own x, y and z (z is up), yaw about +Z in degrees, colour
BRICKS = {
    "red": (ORIGIN + [0.0, 6.0, 0.5], (1.0, 1.0, 1.0), 0.0, (0.77, 0.16, 0.11)),
    "green": (ORIGIN + [3.0, 8.0, 0.75], (2.0, 0.5, 1.5), 30.0, (0.29, 0.59, 0.29)),
    "blue": (ORIGIN + [-2.5, 5.0, 1.5], (0.5, 0.5, 3.0), 0.0, (0.05, 0.41, 0.67)),
    "floating": (ORIGIN + [0.5, 9.0, 3.0], (1.5, 1.5, 0.4), 65.0, (0.96, 0.80, 0.19)),
}
# Only the host has this one: the guest's bricks must disappear behind it
HOST_PILLAR = (ORIGIN + [1.2, 4.5, 1.25], (0.6, 0.6, 2.5), 15.0, (0.85, 0.75, 0.10))

MIN_IOU = 0.97
MAX_MEDIAN_DEPTH_ERROR = 0.01
MAX_P95_DEPTH_ERROR = 0.03
EDGE_PIXELS = 3


def normalize(v):
    return v / np.linalg.norm(v)


def look_at(position, target, roll_degrees=0.0):
    forward = normalize(np.asarray(target, float) - position)
    right = normalize(np.cross(forward, [0.0, 0.0, 1.0]))
    up = np.cross(right, forward)
    roll = math.radians(roll_degrees)
    right, up = right * math.cos(roll) + up * math.sin(roll), up * math.cos(roll) - right * math.sin(roll)
    return forward, right, up


def pixel_rays(forward, right, up, fov_y, width, height):
    """Per-pixel ray directions with a forward component of 1, so a hit's ray parameter is its depth along the forward axis."""
    tan_y = math.tan(math.radians(fov_y) / 2)
    tan_x = tan_y * width / height
    x = ((np.arange(width) + 0.5) / width * 2 - 1) * tan_x
    y = (1 - (np.arange(height) + 0.5) / height * 2) * tan_y
    return forward + x[None, :, None] * right + y[:, None, None] * up


def trace_box(origin, rays, box):
    centre, size, yaw, _ = box
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    axes = np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]])
    local_origin = axes @ (origin - centre)
    local_rays = rays @ axes.T
    half = np.asarray(size) / 2
    with np.errstate(divide="ignore", invalid="ignore"):
        t1 = (-half - local_origin) / local_rays
        t2 = (half - local_origin) / local_rays
    near = np.minimum(t1, t2).max(axis=-1)
    far = np.maximum(t1, t2).min(axis=-1)
    hit = (far >= near) & (far > 0)
    return np.where(hit, np.maximum(near, 0.0), np.inf)


def trace_boxes(origin, rays, boxes):
    depth = np.full(rays.shape[:2], np.inf)
    index = np.full(rays.shape[:2], -1)
    for i, box in enumerate(boxes):
        t = trace_box(origin, rays, box)
        closer = t < depth
        depth = np.where(closer, t, depth)
        index = np.where(closer, i, index)
    return depth, index


def render_host(origin, rays):
    """The host's own picture and depth: sky, a checkerboard ground and the pillar."""
    height, width = rays.shape[:2]
    sky = np.linspace(0.75, 0.45, height)[:, None, None] * np.array([0.75, 0.85, 1.0])
    color = np.broadcast_to(sky, (height, width, 3)).copy()
    depth = np.full((height, width), np.inf)

    with np.errstate(divide="ignore", invalid="ignore"):
        t = (GROUND_Z - origin[2]) / rays[..., 2]
    ground = (rays[..., 2] < 0) & (t > 0)
    point = origin + rays * np.where(ground, t, 0.0)[..., None]
    checker = (np.floor(point[..., 0]) + np.floor(point[..., 1])) % 2
    color[ground] = np.where(checker[ground][:, None] > 0, [0.42, 0.42, 0.44], [0.30, 0.30, 0.32])
    depth[ground] = t[ground]

    pillar = trace_box(origin, rays, HOST_PILLAR)
    front = pillar < depth
    color[front] = HOST_PILLAR[3]
    depth[front] = pillar[front]
    return color, depth


def erode(mask, pixels):
    out = mask.copy()
    for _ in range(pixels):
        shrunk = out.copy()
        shrunk[1:, :] &= out[:-1, :]
        shrunk[:-1, :] &= out[1:, :]
        shrunk[:, 1:] &= out[:, :-1]
        shrunk[:, :-1] &= out[:, 1:]
        out = shrunk
    return out


def save(path, array):
    Image.fromarray(np.clip(array * 255 + 0.5, 0, 255).astype(np.uint8)).save(path)


def main():
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else "out/fakehost")
    outdir.mkdir(parents=True, exist_ok=True)

    reader = FrameReader()
    link = Link()
    hello = link.receive()
    print("guest:", hello)
    link.send(t="origin", pos=ORIGIN.tolist())
    link.send(t="clear")
    for name, (centre, size, yaw, color) in BRICKS.items():
        link.send(t="brick", id=name, pos=np.asarray(centre).tolist(), size=size, yaw=yaw, color=color)

    target = ORIGIN + [0.0, 6.5, 1.0]
    poses = []
    for i, (azimuth, distance, eye_height, fov, roll, size) in enumerate([
        (-90, 9.0, 1.7, 50.0, 0.0, (1280, 720)),
        (-55, 10.0, 2.5, 50.0, 0.0, (1280, 720)),
        (-130, 8.0, 4.0, 70.0, 0.0, (1280, 720)),
        (20, 11.0, 1.2, 50.0, 0.0, (1280, 720)),
        (-90, 9.0, 1.7, 50.0, 20.0, (1280, 720)),
        (160, 9.0, 6.0, 35.0, -10.0, (1000, 562)),
    ]):
        a = math.radians(azimuth)
        position = target + [distance * math.cos(a), distance * math.sin(a), eye_height - 1.0]
        poses.append((i + 1, position, look_at(position, target, roll), fov, size))

    report = []
    passed = True
    bricks = list(BRICKS.values())
    for camera_id, position, (forward, right, up), fov, (width, height) in poses:
        sent = time.monotonic()
        link.send(t="cam", id=camera_id, pos=position.tolist(), right=right.tolist(), fwd=forward.tolist(), up=up.tolist(),
                  fov=fov, w=width, h=height)
        frame = reader.wait_for(camera_id)
        latency = time.monotonic() - sent
        # The first frame after a resize or a new brick can be drawn before its geometry reaches the GPU: take a later one
        time.sleep(0.25)
        frame = reader.wait_for(camera_id)

        rays = pixel_rays(forward, right, up, fov, width, height)
        expected_depth, _ = trace_boxes(position, rays, bricks)
        expected = np.isfinite(expected_depth)
        actual = frame.color[..., 3] > 127
        union = (expected | actual).sum()
        iou = (expected & actual).sum() / union if union else 0.0

        # Depth is compared away from edges: the outline, and one brick's silhouette against another
        known_depth = np.where(expected, expected_depth, 0.0)
        jump = np.zeros_like(expected)
        jump[:, 1:] |= np.abs(np.diff(known_depth, axis=1)) > 0.05
        jump[1:, :] |= np.abs(np.diff(known_depth, axis=0)) > 0.05
        flat = erode(expected, EDGE_PIXELS) & erode(~jump, EDGE_PIXELS) & actual
        errors = np.abs(frame.depth[flat] - expected_depth[flat])
        median = float(np.median(errors)) if errors.size else float("nan")
        p95 = float(np.percentile(errors, 95)) if errors.size else float("nan")
        empty_ok = bool((frame.depth[~actual] >= EMPTY_DEPTH * 0.5).all())

        host_color, host_depth = render_host(position, rays)
        guest_front = actual & (frame.depth < host_depth)
        composite = np.where(guest_front[..., None], frame.color[..., :3] / 255.0, host_color)
        hidden = int((actual & ~guest_front).sum())

        ok = (iou >= MIN_IOU and errors.size > 0 and median <= MAX_MEDIAN_DEPTH_ERROR and p95 <= MAX_P95_DEPTH_ERROR and empty_ok)
        passed &= bool(ok)
        report.append({
            "pose": camera_id, "size": [width, height], "fov": fov, "ok": bool(ok), "iou": round(float(iou), 4),
            "depth_error_median_m": round(median, 6), "depth_error_p95_m": round(p95, 6), "depth_pixels_compared": int(errors.size),
            "empty_depth_ok": empty_ok,
            "guest_pixels": int(actual.sum()), "guest_pixels_hidden_by_host": hidden, "first_frame_latency_s": round(latency, 3),
        })
        print(report[-1])

        save(outdir / f"pose{camera_id}_composite.png", composite)
        save(outdir / f"pose{camera_id}_guest.png", frame.color[..., :3] / 255.0 * actual[..., None])
        difference = np.zeros((height, width, 3))
        difference[expected & actual] = [0.2, 0.7, 0.2]
        difference[expected & ~actual] = [1.0, 0.0, 0.0]
        difference[~expected & actual] = [0.0, 0.3, 1.0]
        save(outdir / f"pose{camera_id}_mask_diff.png", difference)

    (outdir / "report.json").write_text(json.dumps({"passed": passed, "guest": hello, "poses": report}, indent=2))
    print("PASS" if passed else "FAIL", "->", outdir)
    link.close()
    reader.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
