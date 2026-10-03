"""Checks the guest's side of the host's world without GTA: a place loaded without its map, a character that spawns where the
host says, and ground the host feeds in that the character can stand and walk on.

The guest must be running a place:  gtr-guest.exe Classic-Crossroads.rbxl

    python host/test_ground.py [outdir]
"""
import math
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

from fakehost import ORIGIN, look_at
from gtrframe import FrameReader, Link
from lua import run

CELL = 0.5
HALF = 12.0
# The character's pivot is its root part's centre: three studs above its feet, at 0.35 m a stud
PIVOT_HEIGHT = 1.05
WALK = np.array([6.0, 6.0])

# Classic Crossroads' seven tools, and the four building tools, the RCL and the pistol the guest adds to any place
TOOLS = 13

def ground_height(x, y):
    """A slope of about 14 degrees rising to the north-east of the spawn point, flat everywhere else."""
    return ORIGIN[2] + 0.25 * max(0.0, min(x - ORIGIN[0], 8.0)) * (1.0 if y - ORIGIN[1] > 2.0 else 0.0)


def where(link):
    link.send(t="where")
    while True:
        answer = link.receive()
        if answer.get("t") == "where":
            return None if answer["character"] is None else np.array(answer["character"])


def main():
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else "out/ground")
    outdir.mkdir(parents=True, exist_ok=True)
    reader = FrameReader()
    link = Link()
    print("guest:", link.receive())
    results = []

    def check(name, ok, detail):
        results.append(bool(ok))
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")

    link.send(t="origin", pos=ORIGIN.tolist())
    link.send(t="clear")
    tiles = []
    steps = int(HALF / CELL)
    for i in range(-steps, steps):
        for j in range(-steps, steps):
            x, y = ORIGIN[0] + (i + 0.5) * CELL, ORIGIN[1] + (j + 0.5) * CELL
            tiles += [x, y, ground_height(x, y)]
    for start in range(0, len(tiles), 600):
        link.send(t="ground", cell=CELL, depth=3.0, tiles=tiles[start:start + 600])
    link.send(t="spawn", pos=ORIGIN.tolist())

    summary = run(link, """
        local kept = {}
        for _, child in ipairs(workspace:GetChildren()) do
            if child.Name ~= "HostGround" and child.Name ~= "HostBricks" then table.insert(kept, child.ClassName .. " " .. child.Name) end
        end
        local player = game:GetService("Players").LocalPlayer
        local tools = #player.Backpack:GetChildren()
        for _, child in ipairs(player.Character and player.Character:GetChildren() or {}) do
            if child:IsA("Tool") then tools += 1 end
        end
        return #workspace.HostGround:GetChildren() .. "|" .. tools .. "|" .. table.concat(kept, ", ")
    """)
    ground_count, tools, kept = summary.split("|")
    check("ground tiles made", int(ground_count) == len(tiles) // 3, f"{ground_count} of {len(tiles) // 3}")
    check("tools kept without the map", int(tools) == TOOLS, f"{tools} tools; workspace holds: {kept}")

    time.sleep(3.0)
    position = where(link)
    check("character has spawned", position is not None, position)
    if position is not None:
        feet = position[2] - PIVOT_HEIGHT
        expected = ground_height(position[0], position[1])
        check("spawned at the host's point", np.linalg.norm(position[:2] - ORIGIN[:2]) < 1.5, f"{np.round(position - ORIGIN, 2)} m from it")
        check("stands on the ground", abs(feet - expected) < 0.15, f"feet {feet - expected:+.3f} m from the ground")

        # Up the slope: the tiles are steps a quarter of a stud high or so, and the character must climb them
        goal = ORIGIN + [WALK[0], WALK[1], 0.0]
        goal[2] = ground_height(goal[0], goal[1])
        metres_per_stud = 0.35
        guest_goal = ((goal[0] - ORIGIN[0]) / metres_per_stud, (goal[2] - ORIGIN[2]) / metres_per_stud, -(goal[1] - ORIGIN[1]) / metres_per_stud)
        run(link, "game:GetService('Players').LocalPlayer.Character:FindFirstChildOfClass('Humanoid'):MoveTo(Vector3.new(%f, %f, %f))" % guest_goal)
        time.sleep(6.0)
        walked = where(link)
        if walked is None:
            check("walked up the slope", False, "the character is gone")
        else:
            feet = walked[2] - PIVOT_HEIGHT
            expected = ground_height(walked[0], walked[1])
            check("walked up the slope", np.linalg.norm(walked[:2] - goal[:2]) < 1.0, f"{np.linalg.norm(walked[:2] - goal[:2]):.2f} m from the goal")
            check("stands on the slope", abs(feet - expected) < 0.2 and expected > ORIGIN[2] + 0.5,
                  f"feet {feet - expected:+.3f} m from ground that is {expected - ORIGIN[2]:.2f} m up")
            position = walked

        # A picture, with the ground shown so it can be seen
        link.send(t="debug", ground=True)
        eye = position + [7.0 * math.cos(math.radians(-140)), 7.0 * math.sin(math.radians(-140)), 3.0]
        forward, right, up = look_at(eye, position)
        camera_id = int(time.time())
        link.send(t="cam", id=camera_id, pos=eye.tolist(), right=right.tolist(), fwd=forward.tolist(), up=up.tolist(), fov=50.0, w=1280, h=720)
        reader.wait_for(camera_id)
        time.sleep(1.0)
        frame = reader.wait_for(camera_id)
        covered = frame.color[..., 3:] > 127
        Image.fromarray(np.where(covered, frame.color[..., :3], np.full((720, 1280, 3), 96, np.uint8))).save(outdir / "ground.png")
        link.send(t="debug", ground=False)
        time.sleep(0.5)
        hidden = reader.wait_for(camera_id)
        frame = None
        for _ in range(50):
            frame = reader.latest()
            if frame is not None and frame.guest_seconds > hidden.guest_seconds:
                break
            time.sleep(0.02)
        Image.fromarray(np.where(frame.color[..., 3:] > 127, frame.color[..., :3], np.full((720, 1280, 3), 96, np.uint8))).save(outdir / "ground_hidden.png")
        ground_pixels = int((frame.color[..., 3] > 127).sum())
        check("ground is invisible when not shown", ground_pixels < 0.2 * covered.sum(), f"{ground_pixels} guest pixels against {int(covered.sum())} with it shown")

    passed = all(results)
    print("PASS" if passed else "FAIL", "->", outdir)
    link.close()
    reader.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
