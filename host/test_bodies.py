"""Checks, without GTA, the host's vehicles, people and objects as the guest meets them: the character is stopped by a body's
box, a tool's damage to a person's Humanoid is told to the host, a thrown part pushes a body, the host's word on a person's
health and on the character's is taken, and bodies the host no longer lists are gone.

The guest must be running a place:  tools\\run-guest.ps1

    python host/test_bodies.py
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
KEY_W = 119
WALL, PERSON = 101, 102
WALL_AHEAD = 4.0
VEHICLE_KIND, PERSON_KIND = 1, 2


class Host:
    """What the host's script does each frame when it follows the place's camera, and what the guest tells it."""

    def __init__(self):
        self.link = Link()
        self.link.receive()
        self.messages = []

    def run(self, seconds):
        first = len(self.messages)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.link.send(t="view", w=640, h=360)
            self.messages += self.link.drain(1.0 / RATE)
        return self.messages[first:]

    def state(self):
        return [m for m in self.run(0.3) if m.get("t") == "state"][-1]


def body(identity, kind, centre, forward, size, health=-1, velocity=(0.0, 0.0, 0.0)):
    return [identity, kind, *centre, *forward, 0.0, 0.0, 1.0, *size, health, *velocity]


def main():
    results = []

    def check(name, ok, detail):
        results.append(bool(ok))
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")

    host = Host()
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
    tool = Link()
    tool.receive()
    run(tool, "game:GetService('Players').LocalPlayer.Character.Humanoid.Health = 100 return 1")

    start = host.state()
    feet = character(start)
    ahead = camera_forward(start)
    ahead = np.array([ahead[0], ahead[1], 0.0]) / np.linalg.norm(ahead[:2])
    side = np.array([ahead[1], -ahead[0], 0.0])

    # A wall of a vehicle across the way ahead, and a person off to the side
    wall_centre = feet + ahead * WALL_AHEAD + [0, 0, 1.0]
    person_centre = feet + side * 3.0 + [0, 0, 0.9]
    host.link.send(t="bodies", set=[body(WALL, VEHICLE_KIND, wall_centre, ahead, (6.0, 1.0, 2.0)),
                                    body(PERSON, PERSON_KIND, person_centre, ahead, (0.55, 0.4, 1.8), 100)], keep=[WALL, PERSON])
    host.run(0.3)
    listing = run(tool, "local s = {} for _, c in ipairs(workspace.HostBodies:GetChildren()) do table.insert(s, c.ClassName .. ' ' .. c.Name) end table.sort(s) return table.concat(s, ', ')")
    check("a vehicle is a part and a person a model with a Humanoid", listing == "Model HostPerson, Part HostVehicle"
          and run(tool, "return workspace.HostBodies.HostPerson.Humanoid.Health") == "100", listing)
    size = run(tool, "local s = workspace.HostBodies.HostVehicle.Size return string.format('%.2f %.2f %.2f', s.X, s.Y, s.Z)")
    expected = "%.2f %.2f %.2f" % (6.0 / 0.35, 2.0 / 0.35, 1.0 / 0.35)
    check("the box has the body's size, the host's up being the guest's Y", size == expected, f"{size} studs")

    host.link.send(t="key", code=KEY_W, down=True)
    host.run(2.0)
    host.link.send(t="key", code=KEY_W, down=False)
    walked = float((character(host.state()) - feet) @ ahead)
    # The wall's near face is half its thickness short of its centre, and the character half a body short of that
    check("the character is stopped by the box", WALL_AHEAD - 1.6 < walked < WALL_AHEAD - 0.5, f"walked {walked:.2f} m towards a wall {WALL_AHEAD:.1f} m ahead")

    host.run(0.2)
    run(tool, "workspace.HostBodies.HostPerson.Humanoid:TakeDamage(30) return 1")
    hurt = [m for m in host.run(0.4) if m.get("t") == "hurt"]
    check("a tool's damage to a person is told to the host", len(hurt) == 1 and hurt[0]["id"] == PERSON and abs(hurt[0]["damage"] - 30) < 0.1, hurt)
    host.link.send(t="bodies", set=[body(PERSON, PERSON_KIND, person_centre, ahead, (0.55, 0.4, 1.8), 60)])
    echoed = [m for m in host.run(0.4) if m.get("t") == "hurt"]
    health = run(tool, "return workspace.HostBodies.HostPerson.Humanoid.Health")
    check("the host's word on a person's health is taken, and not told back", health == "60" and not echoed, f"health {health}, {len(echoed)} told back")

    # A brick thrown at the wall, to one side of the character
    run(tool, "local w = workspace.HostBodies.HostVehicle local c = game:GetService('Players').LocalPlayer.Character.HumanoidRootPart "
              "local p = Instance.new('Part') p.Name = 'Thrown' p.Size = Vector3.new(2, 2, 2) "
              "local to = Vector3.new(w.Position.X - c.Position.X, 0, w.Position.Z - c.Position.Z).Unit "
              "p.Position = w.Position + w.CFrame.RightVector * 6 - to * 8 p.Parent = workspace p.AssemblyLinearVelocity = to * 80 return 1")
    pushes = [m for m in host.run(1.0) if m.get("t") == "impulse"]
    along = float(np.array(pushes[0]["impulse"]) @ ahead) if pushes else 0.0
    check("a thrown part pushes the body it hits", bool(pushes) and pushes[0]["id"] == WALL and along > 100.0, f"{len(pushes)} pushes, the first {along:.0f} kg m/s onwards")
    run(tool, "local p = workspace:FindFirstChild('Thrown') if p then p:Destroy() end return 1")

    # The wall, as a vehicle driving at 10 m/s, into an anchored brick and then into a loose one: it is stopped by the one, and
    # slowed by the other, which is knocked on
    run(tool, "local w = workspace.HostBodies.HostVehicle local p = Instance.new('Part') p.Name = 'Struck' p.Size = Vector3.new(4, 4, 4) "
              "p.Anchored = true p.Position = w.Position + w.CFrame.LookVector * (w.Size.Z / 2 + 1) p.Parent = workspace return 1")
    host.link.send(t="bodies", set=[body(WALL, VEHICLE_KIND, wall_centre, ahead, (6.0, 1.0, 2.0), velocity=(ahead * 10.0).tolist())])
    crashes = [m for m in host.run(0.5) if m.get("t") == "crash"]
    change = float(np.array(crashes[0]["dv"]) @ ahead) if crashes else 0.0
    check("a vehicle that runs into something anchored is stopped", len(crashes) == 1 and crashes[0]["id"] == WALL and -12.5 < change < -10.0,
          f"{len(crashes)} crashes, its speed changed by {change:.1f} m/s")
    before = np.array([float(v) for v in run(tool, "local p = workspace.Struck p.Anchored = false local q = p.Position return q.X .. ' ' .. q.Y .. ' ' .. q.Z").split()])
    host.link.send(t="bodies", set=[body(WALL, VEHICLE_KIND, wall_centre + ahead * 0.02, ahead, (6.0, 1.0, 2.0), velocity=(ahead * 10.0).tolist())])
    crashes = [m for m in host.run(0.5) if m.get("t") == "crash"]
    change = float(np.array(crashes[0]["dv"]) @ ahead) if crashes else 0.0
    after = np.array([float(v) for v in run(tool, "local p = workspace.Struck local q = p.Position p:Destroy() return q.X .. ' ' .. q.Y .. ' ' .. q.Z").split()])
    moved = float(np.linalg.norm(after - before)) * 0.35
    check("and by something loose is slowed a little, and knocks it on", len(crashes) >= 1 and -3.0 < change < -0.05 and moved > 1.5,
          f"its speed changed by {change:.2f} m/s; the brick went {moved:.1f} m in half a second")
    host.link.send(t="bodies", set=[body(WALL, VEHICLE_KIND, wall_centre, ahead, (6.0, 1.0, 2.0))])
    host.run(0.2)

    host.link.send(t="harm", damage=25)
    host.run(0.3)
    health = float(run(tool, "return game:GetService('Players').LocalPlayer.Character.Humanoid.Health"))
    check("what the host's world does to the player hurts the character", abs(health - 75.0) < 1.0, f"health {health:.0f}")
    run(tool, "game:GetService('Players').LocalPlayer.Character.Humanoid.Health = 100 return 1")

    # A seat in one of the host's vehicles, two metres up and five along: the character is moved to it and held, then let out
    seat = feet + side * 5.0 + [0, 0, 2.0]
    for _ in range(50):
        host.link.send(t="seat", pos=seat.tolist(), fwd=ahead.tolist(), up=[0.0, 0.0, 1.0])
        host.run(0.02)
    held = run(tool, "local r = game:GetService('Players').LocalPlayer.Character.HumanoidRootPart return tostring(r.Anchored) .. ' ' .. tostring(game:GetService('Players').LocalPlayer.Character.Humanoid.Sit)")
    at = character(host.state())
    # The character's place is told by its feet, three studs under its middle
    check("the character is held in the seat it is given", held == "true true" and np.linalg.norm(at + [0, 0, 3 * 0.35] - seat) < 0.1,
          f"anchored and sitting: {held}; {np.linalg.norm(at + [0, 0, 3 * 0.35] - seat):.2f} m from the seat")
    out = feet + side * 2.0
    host.link.send(t="seat", out=out.tolist())
    host.run(1.0)
    held = run(tool, "return tostring(game:GetService('Players').LocalPlayer.Character.HumanoidRootPart.Anchored)")
    at = character(host.state())
    check("and stands where it is let out", held == "false" and np.linalg.norm(at - out) < 0.3, f"anchored: {held}; {np.linalg.norm(at - out):.2f} m from there")

    host.link.send(t="bodies", set=[], keep=[PERSON])
    host.run(0.3)
    left = run(tool, "return #workspace.HostBodies:GetChildren()")
    check("a body the host no longer lists is gone", left == "1", f"{left} left")
    host.link.send(t="clear")
    host.run(0.2)

    passed = all(results)
    print("PASS" if passed else "FAIL")
    tool.close()
    host.link.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
