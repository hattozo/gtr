"""Checks, without GTA, how the host's vehicles meet a trowel wall: a vehicle driving at one is told how far ahead it is (the
"blocked" messages a GTA driver stops short of), and one rolling into it at a fed-up driver's pace tips it over, instead of
shooting it off along the ground.

The guest must be running a place:  tools\\run-guest.ps1

    python host/test_walls.py
"""
import sys
import time

import numpy as np

from fakehost import ORIGIN
from gtrframe import Link
from lua import run

CELL = 0.5
HALF = 24.0
RATE = 60.0
STUD = 0.35
CAR = 201
VEHICLE_KIND = 1
# A car: 1.9 m wide, 4.5 m long and 1.5 m high (the host's x across, y along and z up, as the script sends a body's size)
CAR_SIZE = (1.9, 4.5, 1.5)
# The wall's near face this far east of the origin, and the car's way this far north of the character, which stands at the
# origin and must not be in it
WALL_AT = 16.0
LANE = 6.0
# The fed-up driver's speed (kBlockedPushSpeed in the script)
PUSH_SPEED = 2.2


def body(identity, centre, velocity):
    """The car as the script lists it, facing east."""
    return [identity, VEHICLE_KIND, *centre, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, *CAR_SIZE, -1, *velocity]


def main():
    results = []

    def check(name, ok, detail):
        results.append(bool(ok))
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")

    host = Link()
    host.receive()
    host.send(t="origin", pos=ORIGIN.tolist())
    host.send(t="clear")
    steps = int(HALF / CELL)
    tiles = []
    for i in range(-steps, steps):
        for j in range(-steps, steps):
            tiles += [ORIGIN[0] + (i + 0.5) * CELL, ORIGIN[1] + (j + 0.5) * CELL, ORIGIN[2]]
    for start in range(0, len(tiles), 600):
        host.send(t="ground", cell=CELL, depth=3.0, tiles=tiles[start:start + 600])
    host.send(t="spawn", pos=ORIGIN.tolist())
    end = time.monotonic() + 2.5
    while time.monotonic() < end:
        host.send(t="view", w=640, h=360)
        host.drain(1.0 / RATE)
    tool = Link()
    tool.receive()

    # The trowel's wall: 4 by 1.2 by 2 stud bricks, three along and four up, joined as it joins them, across the car's way.
    # Host east is the guest's +X and host north its -Z, so the wall runs along Z
    face_x = WALL_AT / STUD
    lane_z = -LANE / STUD
    run(tool, f"""
        local wall = Instance.new('Model') wall.Name = 'TestWall' wall.Parent = workspace
        for row = 0, 3 do
            for column = -1, 1 do
                local b = Instance.new('Part') b.Name = 'Brick' .. column b.Size = Vector3.new(2, 1.2, 4)
                b.CFrame = CFrame.new({face_x} + 1, 0.6 + row * 1.2, {lane_z} + column * 4)
                b.Parent = wall b:MakeJoints()
            end
        end
        return 1""")
    centre = np.array([ORIGIN[0], ORIGIN[1] + LANE, ORIGIN[2] + CAR_SIZE[2] / 2 + 0.05])

    def wall_state():
        """The middle column's (the one in the car's way) top and least tilt, how fast the fastest brick goes, and how far on
        the side columns are: the car only clips those."""
        text = run(tool, "local top, tilt, fast, side = -1e9, 180, 0, 0 for _, b in ipairs(workspace.TestWall:GetChildren()) do "
                         "fast = math.max(fast, b.AssemblyLinearVelocity.Magnitude) "
                         "if b.Name == 'Brick0' then top = math.max(top, b.Position.Y) "
                         "tilt = math.min(tilt, math.deg(math.acos(math.min(1, math.max(-1, b.CFrame.UpVector.Y))))) "
                         "else side = math.max(side, b.Position.X) end end "
                         "return string.format('%.3f %.2f %.3f %.3f', top, tilt, fast, side)")
        top, tilt, fast, side = (float(v) for v in text.split())
        return top * STUD, tilt, fast * STUD, side * STUD

    fastest = [0.0]

    def drive(start_x, speed, seconds, watch=False):
        """The car driven east at a steady speed; what the guest said meanwhile. Watching, the fastest any brick goes is kept."""
        said = []
        x = start_x
        frame = 0
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            frame += 1
            if watch and frame % 6 == 0:
                fastest[0] = max(fastest[0], wall_state()[2])
            at = centre + [x - ORIGIN[0], 0.0, 0.0]
            host.send(t="bodies", set=[body(CAR, at.tolist(), [speed, 0.0, 0.0])], keep=[CAR])
            host.send(t="view", w=640, h=360)
            said += [(x, m) for m in host.drain(1.0 / RATE)]
            x += speed / RATE
        return said, x

    # Driving at it at 10 m/s from 30 m off: told how far its front is from the wall, which is right to within a frame's travel
    start_x = ORIGIN[0] + WALL_AT - 30.0
    said, x = drive(start_x, 10.0, 1.8)
    gaps = [(x, m["gap"]) for x, m in said if m.get("t") == "blocked" and m.get("id") == CAR]
    errors = [abs(gap - (ORIGIN[0] + WALL_AT - (x + CAR_SIZE[1] / 2))) for x, gap in gaps]
    first = (ORIGIN[0] + WALL_AT - (gaps[0][0] + CAR_SIZE[1] / 2)) if gaps else 0.0
    check("a vehicle driving at a wall is told it is there, from far enough off to stop", len(gaps) >= 5 and first >= 15.0,
          f"{len(gaps)} times, first {first:.1f} m short of it")
    check("and how far it is", gaps and max(errors) < 0.5, f"off by {max(errors) if errors else float('nan'):.2f} m at most")

    # Stopped short of it, 0.8 m off: still told, so the driver stays stopped
    stop_x = ORIGIN[0] + WALL_AT - CAR_SIZE[1] / 2 - 0.8
    said, _ = drive(stop_x, 0.0, 0.6)
    still = [m["gap"] for _, m in said if m.get("t") == "blocked"]
    check("a vehicle stopped at it is told it is still there", len(still) >= 3 and all(abs(g - 0.8) < 0.1 for g in still),
          f"{len(still)} times, gaps {sorted(set(round(g, 2) for g in still))}")

    # Out of patience, rolling into it: the wall goes over, and isn't flung
    before = wall_state()
    said, _ = drive(stop_x, PUSH_SPEED, 4.0, watch=True)
    end = time.monotonic() + 1.5
    while time.monotonic() < end:
        fastest[0] = max(fastest[0], wall_state()[2])
        host.send(t="view", w=640, h=360)
        host.drain(0.1)
    after = wall_state()
    crashes = [m for _, m in said if m.get("t") == "crash"]
    check("rolled into, the wall in its way tips over", after[1] > 60.0 and after[0] < before[0] - 0.6,
          f"its top from {before[0]:.2f} m to {after[0]:.2f} m up, tilted {after[1]:.0f} degrees")
    check("and isn't flung", fastest[0] < 6.0, f"no brick faster than {fastest[0]:.1f} m/s")
    check("the vehicle is hardly slowed, and not dented", crashes and all(m["hard"] < 3.0 for m in crashes),
          f"{len(crashes)} crashes, hardest {max((m['hard'] for m in crashes), default=0):.1f} m/s")

    host.send(t="bodies", set=[], keep=[])
    run(tool, "local w = workspace:FindFirstChild('TestWall') if w then w:Destroy() end return 1")
    host.close()
    tool.close()
    print("PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
