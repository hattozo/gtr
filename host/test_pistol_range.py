"""Fires the 2009 pistol at one of the host's people at several distances, a few times each, and prints how many shots hurt
them: a fast thin bullet can pass through a person's box between physics steps.

The guest must be running a place, freshly started:  tools\\run-guest.ps1

    python host/test_pistol_range.py
"""
import sys

import numpy as np

from gtrframe import Link
from lua import run
from test_bodies import PERSON, PERSON_KIND, Host, body
from test_drive import camera_forward, character
from test_weapons import HEALTH, LEFT, ground_and_spawn

SHOTS = 6


def main():
    host = Host()
    ground_and_spawn(host)
    tool = Link()
    tool.receive()
    run(tool, "local p = game:GetService('Players').LocalPlayer p.Character.Humanoid:EquipTool(p.Backpack.Pistol) return 1")
    host.run(0.5)
    worst = 1.0
    for distance in (6.0, 12.0, 20.0, 28.0):
        hits = 0
        for _ in range(SHOTS):
            state = host.state()
            feet = character(state)
            ahead = camera_forward(state)
            ahead = np.array([ahead[0], ahead[1], 0.0]) / np.linalg.norm(ahead[:2])
            host.link.send(t="bodies", set=[body(PERSON, PERSON_KIND, feet + ahead * distance + [0.0, 0.0, 0.9], -ahead, (0.55, 0.4, 1.8), 100)], keep=[PERSON])
            host.run(0.2)
            run(tool, "workspace.HostBodies.HostPerson.Humanoid.Health = 100 workspace.HostBodies.HostPerson.Humanoid.Parent = workspace.HostBodies.HostPerson "
                      "game:GetService('Players').LocalPlayer.Character.Pistol.Ammo.Value = 10 return 1")
            at = run(tool, "local c = workspace.CurrentCamera local v = c:WorldToViewportPoint(workspace.HostBodies.HostPerson.Torso.Position) "
                           "return v.X / c.ViewportSize.X .. ' ' .. v.Y / c.ViewportSize.Y")
            x, y = (float(v) for v in at.split())
            host.link.send(t="mouse", pos=[x, y], delta=[1, 0])
            host.run(0.15)
            host.link.send(t="button", button=LEFT, down=True)
            host.run(0.1)
            host.link.send(t="button", button=LEFT, down=False)
            host.run(1.0)
            hits += float(run(tool, HEALTH)) < 100
        print(f"{distance:4.0f} m: {hits}/{SHOTS} shots hurt")
        worst = min(worst, hits / SHOTS)
    run(tool, "game:GetService('Players').LocalPlayer.Character.Humanoid:UnequipTools() return 1")
    host.link.send(t="clear")
    host.run(0.2)
    tool.close()
    host.link.close()
    return 0 if worst >= 0.8 else 1


if __name__ == "__main__":
    sys.exit(main())
