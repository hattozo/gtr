"""Checks, without GTA, that the place's building tools move and delete the host's things only when used on them: with a tool
merely out and the pointer over GTA's people and drivers, nothing is sent; a box nudged by anything else is put back; a box
lost without the hammer out isn't deleted in GTA; a drag with the Move tool moves the thing, and the hammer deletes it.

The guest must be running a place:  tools\\run-guest.ps1

    python host/test_buildtools.py
"""
import sys
import time

import numpy as np

from fakehost import ORIGIN
from gtrframe import Link
from lua import run
from test_drive import camera_forward, character

CELL = 0.5
HALF = 20.0
RATE = 60.0
PERSON, VEHICLE, DRIVER = 301, 302, 303
OBJECT_KIND, VEHICLE_KIND, PERSON_KIND, RIDER_KIND = 0, 1, 2, 3


def body(identity, kind, centre, forward, size, health=-1):
    return [identity, kind, *centre, *forward, 0.0, 0.0, 1.0, *size, health, 0.0, 0.0, 0.0]


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

    listed = []

    def pump(seconds):
        """The host's frames: its view, and its bodies as it lists them; what the guest said meanwhile."""
        said = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            host.send(t="view", w=1280, h=720)
            if listed:
                host.send(t="bodies", set=listed, keep=[b[0] for b in listed])
            said += host.drain(1.0 / RATE)
        return said

    said = pump(2.5)
    tool = Link()
    tool.receive()
    state = [m for m in pump(0.3) if m.get("t") == "state"][-1]
    feet = character(state)
    ahead = camera_forward(state)
    ahead = np.array([ahead[0], ahead[1], 0.0]) / np.linalg.norm(ahead[:2])
    side = np.array([ahead[1], -ahead[0], 0.0])
    # A person two metres ahead, a car beside it and its driver at the car's near window, all in the camera's view
    person_at = feet + ahead * 2.5 + side * 0.8 + [0, 0, 0.9]
    car_at = feet + ahead * 5.0 - side * 2.5 + [0, 0, 0.75]
    driver_at = car_at + side * 1.1 + [0, 0, 0.3]
    listed[:] = [body(PERSON, PERSON_KIND, person_at.tolist(), ahead.tolist(), (0.55, 0.4, 1.8), 100),
                 body(VEHICLE, VEHICLE_KIND, car_at.tolist(), side.tolist(), (1.9, 4.5, 1.5)),
                 body(DRIVER, RIDER_KIND, driver_at.tolist(), side.tolist(), (0.5, 0.6, 0.9), 100)]
    pump(0.5)

    def box_named(identity):
        return ("(function() for _, d in ipairs(workspace.HostBodies:GetDescendants()) do "
                f"if d.Name == 'HostId' and d.Value == '{identity}' then return d.Parent end end end)()")

    def screen_of(identity):
        text = run(tool, f"local p = {box_named(identity)} local c = workspace.CurrentCamera local v = c:WorldToViewportPoint(p.Position) "
                         "return string.format('%.4f %.4f', v.X / c.ViewportSize.X, v.Y / c.ViewportSize.Y)")
        return [float(v) for v in text.split()]

    def select_bin(name):
        index = run(tool, "local i = 0 for n, item in ipairs(game:GetService('Players').LocalPlayer.Backpack:GetChildren()) do "
                          f"if item.Name == '{name}' then i = n end end return i")
        # The hotbar's number keys: 1 to 9, then 0
        code = 48 + int(index) % 10
        host.send(t="key", code=code, down=True)
        pump(0.1)
        host.send(t="key", code=code, down=False)
        pump(0.4)
        return run(tool, f"local b = game:GetService('Players').LocalPlayer.Backpack:FindFirstChild('{name}') return tostring(b ~= nil and b:IsA('HopperBin') and b.Active)")

    def told(said, kind):
        return [m for m in said if m.get("t") == kind]

    active = select_bin("Move")
    check("the Move tool is taken out with its number key", active == "true", f"Active {active}")

    # Hovering over each of them
    said = []
    for identity in (PERSON, DRIVER, VEHICLE, PERSON):
        x, y = screen_of(identity)
        host.send(t="mouse", pos=[x, y], delta=[0.0, 0.0])
        said += pump(0.6)
    check("with the tool out, hovering over GTA's people, drivers and cars sends nothing", not told(said, "move") and not told(said, "delete"),
          f"{len(told(said, 'move'))} moves, {len(told(said, 'delete'))} deletes")

    # Something other than the tool nudging the person's box
    run(tool, f"local p = {box_named(PERSON)} p.CFrame = p.CFrame + Vector3.new(0.5, 0, 0.3) return 1")
    said = pump(0.4)
    back = run(tool, f"local p = {box_named(PERSON)} return string.format('%.3f %.3f', p.Position.X, p.Position.Z)")
    check("a box nudged by anything but a drag is put back, and nothing is sent", not told(said, "move"), f"{len(told(said, 'move'))} moves; at {back}")

    # The car's box taken away by something other than the hammer
    run(tool, f"{box_named(VEHICLE)}:Destroy() return 1")
    said = pump(0.6)
    again = run(tool, f"return tostring({box_named(VEHICLE)} ~= nil)")
    check("a box lost without the hammer out isn't deleted in GTA, and is made again", not told(said, "delete") and again == "true",
          f"{len(told(said, 'delete'))} deletes, made again: {again}")

    # A real drag of the person with the Move tool
    x, y = screen_of(PERSON)
    host.send(t="mouse", pos=[x, y], delta=[0.0, 0.0])
    pump(0.3)
    host.send(t="button", button=0, down=True)
    said = pump(0.2)
    for step in range(1, 16):
        host.send(t="mouse", pos=[x + step * 0.006, y], delta=[8.0, 0.0])
        said += pump(1.0 / 30)
    host.send(t="button", button=0, down=False)
    said += pump(0.5)
    moves = [m for m in told(said, "move") if m.get("id") == PERSON]
    check("a drag with the Move tool moves the person in GTA", len(moves) >= 3, f"{len(moves)} moves of the person")
    # The host puts it where it was dragged to
    listed[0] = body(PERSON, PERSON_KIND, moves[-1]["pos"] if moves else person_at.tolist(), ahead.tolist(), (0.55, 0.4, 1.8), 100)
    pump(0.5)

    # The hammer on the driver
    active = select_bin("Delete")
    x, y = screen_of(DRIVER)
    host.send(t="mouse", pos=[x, y], delta=[0.0, 0.0])
    pump(0.4)
    host.send(t="button", button=0, down=True)
    said = pump(0.1)
    host.send(t="button", button=0, down=False)
    said += pump(0.6)
    deletes = [m for m in told(said, "delete")]
    check("the hammer deletes what it hits in GTA", active == "true" and any(m.get("id") in (DRIVER, VEHICLE) for m in deletes),
          f"Active {active}; deletes {[m.get('id') for m in deletes]}")

    select_bin("Delete")

    # The paintball gun at the person and at the car: the host is told to paint each the ball's colour
    select_bin("PaintballGun")
    painted = {}
    for identity in (PERSON, VEHICLE):
        for _ in range(3):
            x, y = screen_of(identity)
            host.send(t="mouse", pos=[x, y], delta=[0.0, 0.0])
            pump(0.3)
            host.send(t="button", button=0, down=True)
            said = pump(0.1)
            host.send(t="button", button=0, down=False)
            said += pump(1.2)
            paints = [m for m in told(said, "paint") if m.get("id") == identity]
            if paints:
                painted[identity] = paints[-1]["color"]
                break
    check("the paintball gun paints the person and the car it hits", PERSON in painted and VEHICLE in painted, f"painted {painted}")
    select_bin("PaintballGun")
    listed.clear()
    host.send(t="bodies", set=[], keep=[])
    host.close()
    tool.close()
    print("PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
