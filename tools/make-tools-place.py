"""Makes places/Tools.rbxlx, the guest's default place: a StarterPack with the classic brickbattle tools, and a StarterPlayer
that asks for the classic R6 character (Vanadium's own default is R15, which the guest's scripts, the noob look and the
trainer's zombies, don't fit). Nothing else.

The tools are taken from a place the guest is running that has them (Classic Crossroads):

    tools\\run-guest.ps1 -Place <a place with the tools>
    .venv\\Scripts\\python.exe tools\\make-tools-place.py

What the guest's own scripts add to the StarterPack (the HopperBins, the RCL, the pistol; see guest/scripts) is left out:
the guest adds those to any place it runs. The file is written by Vanadium's own XML writer, through the guest's "save"
message.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
from gtrframe import Link  # noqa: E402
from lua import run  # noqa: E402

PLACE = Path(__file__).resolve().parent.parent / "places" / "Tools.rbxlx"
TOOLS = ("PaintballGun", "RocketLauncher", "Slingshot", "Superball", "Sword", "Timebomb", "Trowel")
# GameSettingsAvatar 0 is R6
STARTER_PLAYER = """	<Item class="StarterPlayer" referent="RBXSTARTERPLAYER">
		<Properties>
			<string name="Name">StarterPlayer</string>
			<token name="GameSettingsAvatar">0</token>
		</Properties>
	</Item>
"""


def main():
    PLACE.parent.mkdir(exist_ok=True)
    link = Link()
    link.receive()
    names = ", ".join(f'"{name}"' for name in TOOLS)
    # Everything but the tools is set aside while the StarterPack is written, and put back after
    found = run(link, f"local keep = {{}} for _, n in ipairs({{{names}}}) do keep[n] = true end "
                      "_G.GtrSetAside = {} local had = {} "
                      "for _, c in ipairs(game:GetService('StarterPack'):GetChildren()) do "
                      "  if keep[c.Name] and c:IsA('Tool') then table.insert(had, c.Name) else table.insert(_G.GtrSetAside, c) c.Parent = nil end "
                      "end return table.concat(had, ',')")
    missing = [name for name in TOOLS if name not in found.split(",")]
    try:
        link.send(t="save", what="StarterPack", path=str(PLACE))
        while (answer := link.receive(10.0)).get("t") != "saved":
            pass
    finally:
        run(link, "for _, c in ipairs(_G.GtrSetAside or {}) do c.Parent = game:GetService('StarterPack') end _G.GtrSetAside = nil return 1")
    link.close()
    if not answer.get("ok"):
        print("the guest could not write", PLACE)
        return 1
    text = PLACE.read_text(encoding="utf-8")
    text = text.replace("</roblox>", STARTER_PLAYER + "</roblox>", 1)
    PLACE.write_text(text, encoding="utf-8", newline="\n")
    # Nothing in it may point at a file on this computer
    local = sorted(set(re.findall(r"(?<![A-Za-z])[A-Za-z]:\\[^<\"\n]*", text)))
    print(f"wrote {PLACE} ({len(text)} bytes): {found}")
    if missing:
        print("not in the running place:", ", ".join(missing))
    if local:
        print("refers to files on this computer:", local)
    return 0 if not missing and not local else 1


if __name__ == "__main__":
    sys.exit(main())
