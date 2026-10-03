"""Checks, without GTA, the weapons the guest adds to a place: the RCL and the 2009 pistol are in the backpack, and each, fired
at one of the host's people standing where the place's camera looks, hurts them.

The guest must be running a place, freshly started:  tools\\run-guest.ps1

    python host/test_weapons.py
"""
import sys

import numpy as np

from fakehost import ORIGIN
from gtrframe import Link
from lua import run
from test_bodies import CELL, HALF, KEY_W, PERSON, PERSON_KIND, VEHICLE_KIND, WALL, Host, body

RIDER_KIND = 3
from test_drive import camera_forward, character

LEFT = 0
HEALTH = "local p = workspace.HostBodies:FindFirstChild('HostPerson') return p and p.Humanoid.Health or -1"


def ground_and_spawn(host):
    """A flat ground round the origin, and the character standing on it."""
    host.link.send(t="origin", pos=ORIGIN.tolist())
    host.link.send(t="clear")
    steps = int(HALF / CELL)
    tiles = []
    for i in range(-steps, steps):
        for j in range(-steps, steps):
            tiles += [ORIGIN[0] + (i + 0.5) * CELL, ORIGIN[1] + (j + 0.5) * CELL, ORIGIN[2]]
    for start in range(0, len(tiles), 600):
        host.link.send(t="ground", cell=CELL, depth=3.0, tiles=tiles[start:start + 600])
    host.link.send(t="spawn", pos=ORIGIN.tolist())
    host.run(2.5)


def main():
    results = []

    def check(name, ok, detail):
        results.append(bool(ok))
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")

    host = Host()
    ground_and_spawn(host)
    tool = Link()
    tool.receive()

    names = run(tool, "local s = {} for _, c in ipairs(game:GetService('Players').LocalPlayer.Backpack:GetChildren()) do table.insert(s, c.Name) end return table.concat(s, ',')")
    check("the RCL and the pistol are in the backpack", "RCL" in names.split(",") and "Pistol" in names.split(","), names)

    for weapon in ("RCL", "Pistol", "Slingshot", "Superball"):
        state = host.state()
        feet = character(state)
        ahead = camera_forward(state)
        ahead = np.array([ahead[0], ahead[1], 0.0]) / np.linalg.norm(ahead[:2])
        # A person eight metres ahead, where the middle of the picture is
        person = feet + ahead * 8.0 + [0.0, 0.0, 0.9]
        host.link.send(t="bodies", set=[body(PERSON, PERSON_KIND, person, -ahead, (0.55, 0.4, 1.8), 100)], keep=[PERSON])
        host.run(0.3)
        run(tool, "workspace.HostBodies.HostPerson.Humanoid.Health = 100 return 1")
        run(tool, f"local p = game:GetService('Players').LocalPlayer local t = p.Backpack:FindFirstChild('{weapon}') "
                  "if t then p.Character.Humanoid:EquipTool(t) end return 1")
        host.run(0.5)
        # Aimed at the person: the pointer where they are in the guest's own window
        at = run(tool, "local c = workspace.CurrentCamera local v = c:WorldToViewportPoint(workspace.HostBodies.HostPerson.Torso.Position) "
                       "return v.X / c.ViewportSize.X .. ' ' .. v.Y / c.ViewportSize.Y")
        x, y = (float(v) for v in at.split())
        host.link.send(t="mouse", pos=[x, y], delta=[1, 0])
        host.run(0.2)
        host.link.send(t="button", button=LEFT, down=True)
        told = host.run(0.6)
        host.link.send(t="button", button=LEFT, down=False)
        told += host.run(1.0)
        health = float(run(tool, HEALTH))
        pushes = [m for m in told if m.get("t") == "impulse" and m.get("id") == PERSON]
        check(f"the {weapon} hurts the host's person it is fired at", 0 <= health < 100,
              f"health {health:.0f}; {len(pushes)} pushes {[round(float(np.linalg.norm(m['impulse'])), 1) for m in pushes]}")
        run(tool, "local h = game:GetService('Players').LocalPlayer.Character.Humanoid h:UnequipTools() h.Health = 100 return 1")
        host.run(0.3)

    # One of the host's people riding in a vehicle side on, eight metres ahead: their box is at the vehicle's near side, as
    # high as they sit, and the guest passes through it. Shot at, they are hurt, through the window as it were
    for weapon in ("Pistol", "Slingshot"):
        state = host.state()
        feet = character(state)
        ahead = camera_forward(state)
        ahead = np.array([ahead[0], ahead[1], 0.0]) / np.linalg.norm(ahead[:2])
        across = np.array([-ahead[1], ahead[0], 0.0])
        car = feet + ahead * 8.0 + [0.0, 0.0, 0.7]
        # As the host makes it: a seat 0.45 m in from the side, the box from 0.25 m inside it to 0.5 m out past the side
        rider = car - ahead * 0.85 + [0.0, 0.0, 0.35]
        host.link.send(t="bodies", set=[body(WALL, VEHICLE_KIND, car, across, (1.9, 4.5, 1.4)),
                                        body(PERSON, RIDER_KIND, rider, across, (1.2, 1.2, 1.1), 100)], keep=[WALL, PERSON])
        host.run(0.3)
        run(tool, "workspace.HostBodies.HostPerson.Humanoid.Health = 100 return 1")
        run(tool, f"local p = game:GetService('Players').LocalPlayer local t = p.Backpack:FindFirstChild('{weapon}') "
                  "if t then p.Character.Humanoid:EquipTool(t) end return 1")
        host.run(0.5)
        at = run(tool, "local c = workspace.CurrentCamera local v = c:WorldToViewportPoint(workspace.HostBodies.HostPerson.Torso.Position) "
                       "return v.X / c.ViewportSize.X .. ' ' .. v.Y / c.ViewportSize.Y")
        x, y = (float(v) for v in at.split())
        host.link.send(t="mouse", pos=[x, y], delta=[1, 0])
        host.run(0.2)
        host.link.send(t="button", button=LEFT, down=True)
        told = host.run(0.3)
        host.link.send(t="button", button=LEFT, down=False)
        told += host.run(1.2)
        health = float(run(tool, HEALTH))
        collides = run(tool, "return tostring(workspace.HostBodies.HostPerson.Torso.CanCollide)")
        hurts = [m for m in told if m.get("t") == "hurt"]
        check(f"the {weapon} hurts one of the host's people riding in a vehicle", 0 <= health < 100 and hurts and all(m["id"] == PERSON for m in hurts),
              f"health {health:.0f}, told {[(m['id'], m['damage']) for m in hurts]}, the box collides: {collides}")
        run(tool, "local h = game:GetService('Players').LocalPlayer.Character.Humanoid h:UnequipTools() h.Health = 100 return 1")
        host.run(0.3)

    # And the sword, the character walking up to the vehicle's side and swinging at the window
    state = host.state()
    feet = character(state)
    ahead = camera_forward(state)
    ahead = np.array([ahead[0], ahead[1], 0.0]) / np.linalg.norm(ahead[:2])
    across = np.array([-ahead[1], ahead[0], 0.0])
    car = feet + ahead * 3.5 + [0.0, 0.0, 0.7]
    host.link.send(t="bodies", set=[body(WALL, VEHICLE_KIND, car, across, (1.9, 4.5, 1.4)),
                                    body(PERSON, RIDER_KIND, car - ahead * 0.85 + [0.0, 0.0, 0.35], across, (1.2, 1.2, 1.1), 100)], keep=[WALL, PERSON])
    host.run(0.3)
    run(tool, "workspace.HostBodies.HostPerson.Humanoid.Health = 100 local p = game:GetService('Players').LocalPlayer "
              "p.Character.Humanoid:EquipTool(p.Backpack.Sword) return 1")
    host.run(0.5)
    host.link.send(t="mouse", pos=[0.5, 0.5], delta=[1, 0])
    host.link.send(t="key", code=KEY_W, down=True)
    told = []
    for _ in range(6):
        host.link.send(t="button", button=LEFT, down=True)
        told += host.run(0.15)
        host.link.send(t="button", button=LEFT, down=False)
        told += host.run(0.35)
    host.link.send(t="key", code=KEY_W, down=False)
    told += host.run(0.5)
    health = float(run(tool, HEALTH))
    hurts = [m for m in told if m.get("t") == "hurt"]
    check("the sword hurts one of the host's people riding in a vehicle, swung at the window", 0 <= health < 100 and hurts,
          f"health {health:.0f}, told {[(m['id'], m['damage']) for m in hurts]}")
    run(tool, "local h = game:GetService('Players').LocalPlayer.Character.Humanoid h:UnequipTools() h.Health = 100 return 1")
    host.run(0.3)

    host.link.send(t="clear")
    host.run(0.2)
    passed = all(results)
    print("PASS" if passed else "FAIL")
    tool.close()
    host.link.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
