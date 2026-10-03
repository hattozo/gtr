"""Checks, without GTA, that a host can play the guest's place by passing its keyboard and mouse on: the place's own controls
walk and jump the character, its own camera turns and zooms, tools are picked and used, the character's and the camera's
positions come back, and a second client works beside the host without disturbing it.

The guest must be running a place:  tools\\run-guest.ps1

    python host/test_drive.py
"""
import math
import sys
import time

import numpy as np

from fakehost import ORIGIN, look_at
from gtrframe import FrameReader, Link
from lua import run

CELL = 0.5
HALF = 20.0
RATE = 60.0
# Roblox KeyCodes
KEY_W, KEY_SPACE, KEY_TWO = 119, 32, 50
LEFT, RIGHT = 0, 1

# Classic Crossroads' seven tools, and the four building tools, the RCL and the pistol the guest adds to any place
TOOLS = 13

class Host:
    """What the host's script does each frame: sends its camera, and reads back where the character and the camera are."""

    def __init__(self):
        self.link = Link()
        print("guest:", self.link.receive())
        self.eye = ORIGIN + [0.0, -8.0, 3.0]
        self.forward, self.right, self.up = look_at(self.eye, ORIGIN + [0.0, 0.0, 1.0])
        self.frame = 0
        self.states = []

    def tick(self):
        self.frame += 1
        self.link.send(t="cam", id=self.frame, pos=self.eye.tolist(), right=self.right.tolist(), fwd=self.forward.tolist(),
                       up=self.up.tolist(), fov=50.0, w=640, h=360)
        messages = self.link.drain(1.0 / RATE)
        self.states += [m for m in messages if m.get("t") == "state"]
        return messages

    def run(self, seconds):
        """Keeps the frames coming for a while and returns the states reported meanwhile."""
        first = len(self.states)
        others = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            others += [m for m in self.tick() if m.get("t") != "state"]
        self.others = others
        return self.states[first:]

    def key(self, code, down):
        self.link.send(t="key", code=code, down=down)

    def state(self):
        return self.run(0.1)[-1]


def character(state):
    return None if state["char"] is None else np.array(state["char"][:3])


def camera_forward(state):
    pitch, yaw = math.radians(state["cam"][3]), math.radians(state["cam"][5])
    return np.array([-math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch), math.sin(pitch)])


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
    host.link.send(t="mouse", pos=[0.5, 0.5], delta=[0, 0])

    states = host.run(2.5)
    check("the host is told where the character and the camera are", bool(states) and states[-1]["char"] is not None and states[-1]["cam"] is not None,
          states[-1] if states else "nothing")
    if not states or states[-1]["char"] is None or states[-1]["cam"] is None:
        return 1
    start = states[-1]
    feet = character(start)
    check("the character's feet are on the ground at the spawn point", abs(feet[2] - ORIGIN[2]) < 0.1 and np.linalg.norm(feet[:2] - ORIGIN[:2]) < 1.0,
          f"{np.round(feet - ORIGIN, 3)} m from it")
    distance = np.linalg.norm(np.array(start["cam"][:3]) - (feet + [0, 0, 1.5]))
    check("the place's camera follows the character", 1.0 < distance < 15.0, f"{distance:.1f} m from the character's head")

    # W walks away from the place's camera, whichever way that faces
    ahead = camera_forward(start)[:2]
    ahead /= np.linalg.norm(ahead)
    host.key(KEY_W, True)
    walked = character(host.run(1.5)[-1]) - feet
    host.key(KEY_W, False)
    along = float(walked[:2] @ ahead)
    sideways = abs(float(ahead[0] * walked[1] - ahead[1] * walked[0]))
    check("W walks forward, relative to the place's camera", along > 5.0 and sideways < 1.0, f"{along:.2f} m forward, {sideways:.2f} m sideways in 1.5 s")
    host.run(0.5)
    first, second = character(host.state()), character(host.run(0.4)[-1])
    check("letting go stops", np.linalg.norm(second - first) < 0.05, f"moved {np.linalg.norm(second - first):.3f} m in 0.4 s")

    # A host that goes quiet with a key down (the game paused) can't send the key coming up
    host.key(KEY_W, True)
    host.run(0.3)
    time.sleep(0.8)
    first = character(host.state())
    second = character(host.run(0.4)[-1])
    check("a held key is let go when the host goes quiet", np.linalg.norm(second - first) < 0.05, f"moved {np.linalg.norm(second - first):.3f} m after it came back")
    host.key(KEY_W, False)

    host.key(KEY_SPACE, True)
    heights = [character(s)[2] for s in host.run(0.2)]
    host.key(KEY_SPACE, False)
    heights += [character(s)[2] for s in host.run(0.8)]
    check("Space jumps", max(heights) - ORIGIN[2] > 0.5, f"rose {max(heights) - ORIGIN[2]:.2f} m")

    # Dragging with the right button turns the place's camera; the wheel zooms it
    before = host.state()
    host.link.send(t="button", button=RIGHT, down=True)
    for _ in range(30):
        host.link.send(t="mouse", pos=[0.5, 0.5], delta=[8, 0])
        host.tick()
    host.link.send(t="button", button=RIGHT, down=False)
    after = host.run(0.3)[-1]
    turned = (after["cam"][5] - before["cam"][5] + 180.0) % 360.0 - 180.0
    check("dragging with the right button turns the camera", abs(turned) > 20.0, f"yaw changed by {turned:.1f} degrees")

    def zoom(state):
        return float(np.linalg.norm(np.array(state["cam"][:3]) - character(state)))
    near = zoom(after)
    for _ in range(4):
        host.link.send(t="wheel", delta=-1.0)
        host.run(0.1)
    far = zoom(host.run(0.6)[-1])
    check("the wheel zooms the camera", abs(far - near) > 0.5, f"camera went from {near:.2f} to {far:.2f} m from the character")

    # A tool beside the host: it must get its answer, and the host must keep its place as the host
    tool = Link()
    tool.receive()
    answer = run(tool, "return #game:GetService('Players').LocalPlayer.Backpack:GetChildren()")
    check("a second client works beside the host", answer == str(TOOLS) and bool(host.run(0.3)), f"backpack {answer}; the host still gets states")
    tool.send(t="host", op="look", heading=10)
    host.run(0.3)
    relayed = [m for m in host.others if m.get("t") == "host"]
    check("a tool's message for the host's script is passed on", len(relayed) == 1 and relayed[0].get("op") == "look", relayed)
    shape = run(tool, "local s = workspace.CurrentCamera.ViewportSize return s.X .. 'x' .. s.Y")
    width, height = (float(v) for v in shape.split("x"))
    check("the guest's window has the shape of the host's picture", abs(width / height - 640 / 360) < 0.02, shape)

    # The number keys pick a tool, as they do in the place itself; a click uses it
    equipped_before = run(tool, "return tostring(game:GetService('Players').LocalPlayer.Character:FindFirstChildOfClass('Tool'))")
    host.key(KEY_TWO, True)
    host.run(0.1)
    host.key(KEY_TWO, False)
    host.run(0.8)
    equipped = run(tool, "return tostring(game:GetService('Players').LocalPlayer.Character:FindFirstChildOfClass('Tool'))")
    check("a number key equips a tool", equipped_before == "nil" and equipped != "nil", f"{equipped_before} -> {equipped}")

    listing = "local s = {} for _, c in ipairs(workspace:GetChildren()) do if c.Name ~= 'HostGround' and c.Name ~= 'HostBricks' then table.insert(s, c.Name) end end return #s .. ': ' .. table.concat(s, ', ')"
    before = run(tool, listing)
    host.link.send(t="mouse", pos=[0.5, 0.4], delta=[0, 0])
    host.link.send(t="button", button=LEFT, down=True)
    host.run(0.15)
    host.link.send(t="button", button=LEFT, down=False)
    host.run(0.5)
    after = run(tool, listing)
    check("a click uses the tool", int(after.split(":")[0]) > int(before.split(":")[0]), f"{equipped}: workspace went from [{before}] to [{after}]")

    # The host that plays the place follows the place's camera instead of sending one: each frame then carries the camera it
    # was drawn from, which is where the host puts its own camera for that frame
    reader = FrameReader()

    def follow(seconds):
        """Sends what such a host sends each frame, and returns (frame, state) pairs seen meanwhile."""
        seen = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            host.link.send(t="view", w=640, h=360)
            states = [m for m in host.link.drain(1.0 / RATE) if m.get("t") == "state"]
            frame = reader.latest()
            if frame is not None and states:
                seen.append((frame, states[-1]))
        return seen

    def off_axis(frame, state):
        """The angle, in degrees, between the camera a frame was drawn from and the place's camera as reported."""
        return math.degrees(math.acos(min(1.0, float(frame.forward @ camera_forward(state)))))

    seen = follow(1.5)
    ids = [frame.camera_id for frame, _ in seen]
    frame, state = seen[-1]
    apart = float(np.linalg.norm(frame.position - np.array(state["cam"][:3])))
    check("frames are drawn from the place's camera and say which", ids[-1] > ids[0] and apart < 0.05 and off_axis(frame, state) < 0.2 and abs(frame.fov_y - state["cam"][6]) < 0.01,
          f"camera ids {ids[0]}..{ids[-1]}; {apart * 100:.1f} cm and {off_axis(frame, state):.2f} degrees from the camera reported")

    # An explosion near the camera shakes the camera the frames are drawn from, and so the host's with it
    run(tool, "local c = workspace.CurrentCamera.CFrame local e = Instance.new('Explosion') e.BlastPressure = 0 e.BlastRadius = 12 "
              "e.Position = (c * CFrame.new(0, 0, -20)).Position e.Parent = workspace return 1")
    shaken = max(off_axis(frame, state) for frame, state in follow(0.5))
    settled = max(off_axis(frame, state) for frame, state in follow(1.5)[-10:])
    check("an explosion shakes the camera, then it settles", shaken > 0.5 and settled < 0.2, f"up to {shaken:.2f} degrees off, then {settled:.2f}")
    reader.close()

    passed = all(results)
    print("PASS" if passed else "FAIL")
    tool.close()
    host.link.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
