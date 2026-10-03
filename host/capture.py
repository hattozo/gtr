"""Saves the game's finished picture (the host with the guest composited in) without touching the game's window: asks the
compositor add-on for it through the host state the script shares (GtrHostState in shared/gtr_frame.h).

    python host/capture.py [out.png]
    python host/capture.py --running    exits 0 when the host's script is ticking (the game is unpaused and in front)
"""
import ctypes
import struct
import sys
import time
from ctypes import wintypes
from pathlib import Path

from PIL import Image

MAPPING_NAME = "Local\\GtrPassthroughHost"
MAGIC = 0x48525447
STATE_BYTES = 512
REQUEST_OFFSET, DONE_OFFSET, HEARTBEAT_OFFSET, PATH_OFFSET, PATH_BYTES = 20, 24, 28, 32, 260
DEBUG_VIEW_OFFSET, SHOW_MARK_OFFSET = 360, 364
FILE_MAP_ALL_ACCESS = 0x000F001F

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.OpenFileMappingW.restype = wintypes.HANDLE
_kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_kernel32.MapViewOfFile.restype = ctypes.c_void_p
_kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]


def open_state():
    handle = _kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, MAPPING_NAME)
    if not handle:
        raise FileNotFoundError("the host's state mapping doesn't exist: is the game running with GtrHost.asi?")
    view = _kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, STATE_BYTES)
    state = (ctypes.c_ubyte * STATE_BYTES).from_address(view)
    if struct.unpack_from("<I", state, 0)[0] != MAGIC:
        raise RuntimeError("the host's state mapping has the wrong magic")
    return state


def running(seconds=0.4):
    """Whether the host's script is ticking: the game stops it in the pause menu and while another window is in front."""
    state = open_state()
    before = struct.unpack_from("<I", state, HEARTBEAT_OFFSET)[0]
    time.sleep(seconds)
    return struct.unpack_from("<I", state, HEARTBEAT_OFFSET)[0] != before


def set_debug(view=None, show_mark=None):
    """Has the effect show one of its debug views (its number in the effect's list; 0 is the finished picture), or leave
    the script's mark in the picture."""
    state = open_state()
    if view is not None:
        struct.pack_into("<i", state, DEBUG_VIEW_OFFSET, view + 1)
    if show_mark is not None:
        struct.pack_into("<i", state, SHOW_MARK_OFFSET, 1 if show_mark else 0)


def capture(out, timeout=10.0):
    state = open_state()

    bmp = Path(out).with_suffix(".bmp").resolve()
    bmp.unlink(missing_ok=True)
    path = str(bmp).encode("mbcs")
    if len(path) >= PATH_BYTES:
        raise ValueError("the path is too long for the host state")
    state[PATH_OFFSET:PATH_OFFSET + PATH_BYTES] = path.ljust(PATH_BYTES, b"\0")
    request = struct.unpack_from("<i", state, REQUEST_OFFSET)[0] + 1
    struct.pack_into("<i", state, REQUEST_OFFSET, request)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if struct.unpack_from("<i", state, DONE_OFFSET)[0] == request and bmp.exists():
            time.sleep(0.1)
            Image.open(bmp).convert("RGB").save(out)
            bmp.unlink()
            return
        time.sleep(0.02)
    raise TimeoutError("the compositor saved no picture: is the game presenting frames?")


if __name__ == "__main__":
    if sys.argv[1:] == ["--running"]:
        sys.exit(0 if running() else 1)
    target = sys.argv[1] if len(sys.argv) > 1 else "out/game.png"
    Path(target).parent.mkdir(parents=True, exist_ok=True)
    capture(target)
    print("saved", target)
