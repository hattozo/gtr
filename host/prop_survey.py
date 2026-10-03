"""Asks a running game how large its props are, and lists the ones that would fit inside the guest character's body parts.

For the character to be in GTA's own shadow maps, GTA has to draw something where the character is: props hidden inside
its torso, head and limbs, which the guest's picture then covers. A prop can't be scaled, so this looks for ones that
already are the right size: as large as fits inside the part.

    python host/prop_survey.py            ask the game (it must be running unpaused with the guest connected), then list
    python host/prop_survey.py --again    list again from the sizes the game gave last time

The prop names come from a public list of GTA V's object names, fetched once into out/.
"""
import os
import sys
import time
import urllib.request
from pathlib import Path

import numpy as np

from capture import running
from gtrframe import Link
from sun_calibrate import log_matches, wait_for_log

NAMES_URL = "https://raw.githubusercontent.com/DurtyFree/gta-v-data-dumps/master/ObjectList.ini"
FOLDER = Path(os.environ["LOCALAPPDATA"]) / "Gtr"
STUD = 0.35
# A classic character's parts, in metres. The head is a rounded cylinder, so what fits inside it is smaller than its box
PARTS = {
    "arm or leg": np.array([1.0, 2.0, 1.0]) * STUD,
    "torso": np.array([2.0, 2.0, 1.0]) * STUD,
    "head": np.array([1.0, 1.0, 1.0]) * STUD,
}
# A prop has to be this much smaller than its part all round (the parts' edges are rounded), and isn't worth having when
# any side of it is less than this much of the part's
INSET = 0.015
MIN_FILL = 0.7
LISTED = 25
# Names that are plainly not a solid box: a prop's size says nothing of its shape
NOT_SOLID = ("lod", "fence", "rail", "sign", "wire", "cable", "frame", "ladder", "plant", "bush", "tree", "glass", "decal", "shadow", "light", "rope", "net")


def main():
    root = Path(__file__).resolve().parent.parent
    sizes = FOLDER / "modeldims.txt"
    if "--again" not in sys.argv[1:]:
        names = root / "out" / "ObjectList.ini"
        if not names.exists():
            names.parent.mkdir(parents=True, exist_ok=True)
            urllib.request.urlretrieve(NAMES_URL, names)
        (FOLDER / "models.txt").write_text(names.read_text())
        if not running():
            print("FAIL: the host's script isn't ticking (is the game running, unpaused?)")
            return 1
        already = len(log_matches(r"survey done"))
        link = Link()
        link.receive()
        link.send(t="host", op="modeldims", load=0)
        wait_for_log(r"survey done", already, timeout=300.0)
        time.sleep(0.3)
        link.close()

    props = []
    for line in sizes.read_text().splitlines():
        words = line.split()
        if len(words) != 8:
            continue
        low, high = np.array(words[1:4], float), np.array(words[4:7], float)
        if (high - low).min() > 0.01:
            props.append((words[0], high - low))
    print(f"{len(props)} props with a size")

    for part, size in PARTS.items():
        room = np.sort(size) - 2 * INSET
        fits = []
        for name, extent in props:
            fill = np.sort(extent) / np.sort(size)
            if (np.sort(extent) <= room).all() and fill.min() >= MIN_FILL and not any(word in name.lower() for word in NOT_SOLID):
                fits.append((float(fill.prod()), name, extent))
        fits.sort(reverse=True)
        print(f"\n{part}, {size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m: {len(fits)} props fit")
        for fill, name, extent in fits[:LISTED]:
            print(f"  {fill * 100:3.0f}% of its volume  {extent[0]:.2f} x {extent[1]:.2f} x {extent[2]:.2f}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
