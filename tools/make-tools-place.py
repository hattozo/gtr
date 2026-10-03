"""Makes places/Tools.rbxlx, the guest's default place: a StarterPack with the classic brickbattle tools, and a StarterPlayer
that asks for the classic R6 character (Vanadium's own default is R15, which the guest's scripts, the noob look and the
trainer's zombies, don't fit), and the modules the tools load from elsewhere in the place: ServerStorage.Modules (the
rocket's and the bomb's doExplosion) and ReplicatedStorage.HandleReload (the guns' cursors). Without those the rocket and
the bomb fail on their first line and the cursors wait for ever. Nothing else.

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
# What is kept of each service saved: the tools, and the modules they require
KEPT = (
    ("StarterPack", TOOLS),
    ("ReplicatedStorage", ("HandleReload",)),
    ("ServerStorage", ("Modules",)),
)
# GameSettingsAvatar 0 is R6
STARTER_PLAYER = """	<Item class="StarterPlayer" referent="RBXSTARTERPLAYER">
		<Properties>
			<string name="Name">StarterPlayer</string>
			<token name="GameSettingsAvatar">0</token>
		</Properties>
	</Item>
"""


def save(link, service, kept, path):
    """Writes the service with only the named children in it, and puts the rest back. Returns the names it had."""
    names = ", ".join(f'"{name}"' for name in kept)
    # Everything else is set aside while the service is written, and put back after
    found = run(link, f"local keep = {{}} for _, n in ipairs({{{names}}}) do keep[n] = true end "
                      "_G.GtrSetAside = {} local had = {} "
                      f"local service = game:GetService('{service}') "
                      "for _, c in ipairs(service:GetChildren()) do "
                      "  if keep[c.Name] then table.insert(had, c.Name) else table.insert(_G.GtrSetAside, c) c.Parent = nil end "
                      "end return table.concat(had, ',')")
    try:
        link.send(t="save", what=service, path=str(path))
        while (answer := link.receive(10.0)).get("t") != "saved":
            pass
    finally:
        run(link, f"for _, c in ipairs(_G.GtrSetAside or {{}}) do c.Parent = game:GetService('{service}') end _G.GtrSetAside = nil return 1")
    return [name for name in found.split(",") if name] if answer.get("ok") else None


def items(text, prefix):
    """The top-level items of a saved file, with its referents renamed so that another file's can't clash with them."""
    body = text[text.index("<Item "):text.rindex("</roblox>")]
    return re.sub(r'(referent="|<Ref name="[^"]*">)RBX', lambda m: m.group(1) + prefix, body)


def main():
    PLACE.parent.mkdir(exist_ok=True)
    link = Link()
    link.receive()
    parts, missing, had = [], [], []
    for index, (service, kept) in enumerate(KEPT):
        part = PLACE.with_suffix(f".{service}.rbxlx")
        found = save(link, service, kept, part)
        if found is None:
            link.close()
            print("the guest could not write", part)
            return 1
        missing += [name for name in kept if name not in found]
        had += found
        saved = part.read_text(encoding="utf-8")
        # The XML writer's own heading, up to the first item
        head = saved[:saved.index("<Item ")]
        parts.append(items(saved, f"RBX{index}_"))
        part.unlink()
    link.close()
    text = head + "".join(parts) + STARTER_PLAYER + "</roblox>"
    PLACE.write_text(text, encoding="utf-8", newline="\n")
    # Nothing in it may point at a file on this computer
    local = sorted(set(re.findall(r"(?<![A-Za-z])[A-Za-z]:\\[^<\"\n]*", text)))
    print(f"wrote {PLACE} ({len(text)} bytes): {', '.join(had)}")
    if missing:
        print("not in the running place:", ", ".join(missing))
    if local:
        print("refers to files on this computer:", local)
    return 0 if not missing and not local else 1


if __name__ == "__main__":
    sys.exit(main())
