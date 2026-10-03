"""Checks, without GTA, the trainer the guest adds to a place: its menu is there, a zombie it spawns walks at the player and
hurts them, the zombies can be cleared, and what the menu asks of the host reaches the host.

The guest must be running a place, freshly started:  tools\\run-guest.ps1

    python host/test_trainer.py
"""
import sys

import numpy as np

from fakehost import ORIGIN
from gtrframe import Link
from lua import run
from test_bodies import CELL, HALF, Host

ZOMBIES = "local n = 0 for _, c in ipairs(workspace:GetChildren()) do if c.Name == 'Zombie' then n += 1 end end return n"
NEAREST = ("local r = game:GetService('Players').LocalPlayer.Character.HumanoidRootPart local best = 1e9 "
           "for _, c in ipairs(workspace:GetChildren()) do if c.Name == 'Zombie' and c:FindFirstChild('Torso') then "
           "best = math.min(best, (c.Torso.Position - r.Position).Magnitude) end end return best")
HEALTH = "return game:GetService('Players').LocalPlayer.Character.Humanoid.Health"


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

    menu = run(tool, "local g = game:GetService('Players').LocalPlayer.PlayerGui:FindFirstChild('Trainer') return g and #g:GetDescendants() or 0")
    check("the trainer's menu is in the player's interface", int(menu) > 10, f"{menu} things in it")

    run(tool, "_G.GtrTrainer.spawnZombies(2) return 1")
    host.run(0.5)
    count = int(run(tool, ZOMBIES))
    far = float(run(tool, NEAREST))
    check("zombies are spawned round the player", count == 2 and 10 < far < 30, f"{count} zombies, the nearest {far:.1f} studs away")
    host.run(7.0)
    near = float(run(tool, NEAREST))
    health = float(run(tool, HEALTH))
    check("a zombie walks at the player and hurts them", near < 6.0 and health < 100.0, f"the nearest now {near:.1f} studs away, the player's health {health:.0f}")

    run(tool, "_G.GtrTrainer.clearZombies() return 1")
    host.run(0.3)
    check("the zombies can be cleared", int(run(tool, ZOMBIES)) == 0, f"{run(tool, ZOMBIES)} left")

    run(tool, "local v = Instance.new('StringValue') v.Name = 'HostRequest' v.Value = '{\"t\":\"host\",\"op\":\"time\",\"clock\":[8,0]}' v.Parent = workspace.HostCamera return 1")
    asked = [m for m in host.run(0.5) if m.get("t") == "host"]
    check("what the menu asks of the host reaches it", len(asked) == 1 and asked[0].get("op") == "time", asked)
    run(tool, "game:GetService('Players').LocalPlayer.Character.Humanoid.Health = 100 return 1")

    passed = all(results)
    print("PASS" if passed else "FAIL")
    tool.close()
    host.link.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
