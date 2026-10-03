"""Reads the frames the guest publishes (shared/gtr_frame.h) and talks to it over its link."""
import ctypes
import json
import socket
import struct
import time
from ctypes import wintypes

import numpy as np

MAPPING_NAME = "Local\\GtrPassthroughFrame"
MAGIC = 0x46525447
HEADER_BYTES = 4096
SLOT_DESC_OFFSET = 256
SLOT_DESC_BYTES = 256
LINK_PORT = 25610
EMPTY_DEPTH = 1.0e9
FILE_MAP_READ = 0x0004

# Sequence, CameraId, Width, Height, FovY, reserved, position, right, forward, up, reserved, GuestSeconds
SLOT_FORMAT = "<qqIIff3d3f3f3fIdIII4H4x3d3f3f3ffI"

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.OpenFileMappingW.restype = wintypes.HANDLE
_kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_kernel32.MapViewOfFile.restype = ctypes.c_void_p
_kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
_kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


class Frame:
    def __init__(self, fields, color, depth, gui=None):
        (_, self.camera_id, self.width, self.height, self.fov_y, _, px, py, pz,
         rx, ry, rz, fx, fy, fz, ux, uy, uz, _, self.guest_seconds, _, _, _, *rest) = fields
        self.rect = list(rest[:4])
        # The shadow map's camera, as the frame's own, the tangent of half its field of view, and its size (0 for none)
        self.light_position = np.array(rest[4:7])
        self.light_right, self.light_forward, self.light_up = np.array(rest[7:10]), np.array(rest[10:13]), np.array(rest[13:16])
        self.light_tan, self.light_size = rest[16], rest[17]
        self.light = None
        self.position = np.array([px, py, pz])
        self.right = np.array([rx, ry, rz])
        self.forward = np.array([fx, fy, fz])
        self.up = np.array([ux, uy, uz])
        self.color = color  # height x width x 4, uint8 RGBA
        self.depth = depth  # height x width, float32 metres along the forward axis
        # The guest's interface, or None: its own height x width x 4, uint8 RGBA, colour multiplied by alpha
        self.gui = gui


class FrameReader:
    """Opens the guest's mapping; raises FileNotFoundError while the guest isn't running."""

    def __init__(self):
        self._handle = _kernel32.OpenFileMappingW(FILE_MAP_READ, False, MAPPING_NAME)
        if not self._handle:
            raise FileNotFoundError("the guest's frame mapping doesn't exist: is gtr-guest running?")
        header = _kernel32.MapViewOfFile(self._handle, FILE_MAP_READ, 0, 0, HEADER_BYTES)
        magic, _version, self.slots, _max_w, _max_h, _, self.stride = struct.unpack_from("<IIiIIIq", ctypes.string_at(header, 32))
        _kernel32.UnmapViewOfFile(header)
        if magic != MAGIC:
            raise RuntimeError("the frame mapping has the wrong magic")
        self._size = HEADER_BYTES + self.stride * self.slots
        self._view = _kernel32.MapViewOfFile(self._handle, FILE_MAP_READ, 0, 0, self._size)
        if not self._view:
            raise OSError(ctypes.get_last_error(), "could not map the guest's frames")
        self._bytes = (ctypes.c_ubyte * self._size).from_address(self._view)

    def close(self):
        self._bytes = None
        _kernel32.UnmapViewOfFile(self._view)
        _kernel32.CloseHandle(self._handle)

    def published(self):
        return struct.unpack_from("<q", self._bytes, 32)[0]

    def latest(self):
        """The newest frame, or None when there is none yet or the guest rewrote it mid-copy."""
        slot = struct.unpack_from("<i", self._bytes, 40)[0]
        if self.published() == 0 or not 0 <= slot < self.slots:
            return None
        desc = SLOT_DESC_OFFSET + SLOT_DESC_BYTES * slot
        fields = struct.unpack_from(SLOT_FORMAT, self._bytes, desc)
        sequence, width, height = fields[0], fields[2], fields[3]
        if sequence & 1 or width == 0 or height == 0:
            return None
        base = HEADER_BYTES + self.stride * slot
        layer = width * height * 4
        color = np.frombuffer(self._bytes, dtype=np.uint8, count=layer, offset=base).reshape(height, width, 4).copy()
        depth = np.frombuffer(self._bytes, dtype=np.float32, count=width * height, offset=base + layer).reshape(height, width).copy()
        # Only the part the guest drew in is written; the rest is empty whatever bytes are there
        left, top, rect_width, rect_height = fields[23:27]
        outside = np.ones((height, width), bool)
        outside[top:top + rect_height, left:left + rect_width] = False
        color[outside] = 0
        depth[outside] = EMPTY_DEPTH
        gui = None
        gui_width, gui_height, blue_first = fields[20:23]
        if gui_width and gui_height:
            gui = np.frombuffer(self._bytes, dtype=np.uint8, count=gui_width * gui_height * 4, offset=base + layer * 2).reshape(gui_height, gui_width, 4).copy()
            if blue_first:
                gui = gui[..., [2, 1, 0, 3]]
        if struct.unpack_from("<q", self._bytes, desc)[0] != sequence:
            return None
        frame = Frame(fields, color, depth, gui)
        if frame.light_size:
            frame.light = np.frombuffer(self._bytes, dtype=np.float32, count=frame.light_size ** 2,
                                        offset=base + layer * 2 + gui_width * gui_height * 4).reshape(frame.light_size, frame.light_size).copy()
        if struct.unpack_from("<q", self._bytes, desc)[0] != sequence:
            return None
        return frame

    def wait_for(self, camera_id, timeout=10.0):
        """The first frame drawn from the cam message with this id."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.latest()
            if frame is not None and frame.camera_id == camera_id:
                return frame
            time.sleep(0.002)
        raise TimeoutError(f"no frame for camera {camera_id} within {timeout} s")


class Link:
    def __init__(self, port=LINK_PORT, timeout=10.0):
        self._socket = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buffer = b""

    def send(self, **message):
        self._socket.sendall((json.dumps(message) + "\n").encode())

    def receive(self, timeout=5.0):
        self._socket.settimeout(timeout)
        while b"\n" not in self._buffer:
            chunk = self._socket.recv(65536)
            if not chunk:
                raise ConnectionError("the guest closed the link")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line)

    def drain(self, seconds=0.0):
        """Every message that has arrived, and those that arrive in the next `seconds`."""
        messages = []
        deadline = time.monotonic() + seconds
        while True:
            while b"\n" in self._buffer:
                line, self._buffer = self._buffer.split(b"\n", 1)
                messages.append(json.loads(line))
            remaining = deadline - time.monotonic()
            self._socket.settimeout(max(remaining, 0.001))
            try:
                chunk = self._socket.recv(65536)
            except (socket.timeout, BlockingIOError):
                if remaining <= 0:
                    return messages
                continue
            if not chunk:
                raise ConnectionError("the guest closed the link")
            self._buffer += chunk

    def close(self):
        self._socket.close()
