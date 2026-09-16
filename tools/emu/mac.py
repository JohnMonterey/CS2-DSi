"""macOS primitives the UI harness needs: synthetic input, window geometry, capture.

Everything here is stdlib only. Input goes through Quartz CGEvent via ctypes rather
than a helper binary or `cliclick`, because CGEvent is the only mechanism that can
hold a key down for a known number of frames -- AppleScript's `key down` only accepts
printable characters, and cliclick can only hold modifiers.

Two macOS permissions are required, both granted to the program that RUNS this code
(your terminal, or whatever the agent shells out from), not to melonDS:

  * Accessibility   -- for CGEventPost. Without it, events are silently dropped and
                      every test fails with "nothing happened".
  * Screen Recording -- for `screencapture`. Without it captures come back as an
                      empty desktop picture, which looks like a black DS screen.

`doctor.py` checks both.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import subprocess
import time
from dataclasses import dataclass

# --------------------------------------------------------------------------------------
# Quartz
# --------------------------------------------------------------------------------------

_AS_PATH = "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices"


def _load_quartz():
    try:
        return ctypes.cdll.LoadLibrary(_AS_PATH)
    except OSError as exc:  # pragma: no cover - only on non-macOS
        raise RuntimeError(
            "Quartz is unavailable; the UI harness only runs on macOS"
        ) from exc


_q = None


def quartz():
    global _q
    if _q is None:
        _q = _load_quartz()
        _q.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
        _q.CGEventCreateKeyboardEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint16,
            ctypes.c_bool,
        ]
        _q.CGEventCreateMouseEvent.restype = ctypes.c_void_p
        _q.CGEventCreateMouseEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            CGPoint,
            ctypes.c_uint32,
        ]
        _q.CGEventPost.restype = None
        _q.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        _q.CFRelease.restype = None
        _q.CFRelease.argtypes = [ctypes.c_void_p]
    return _q


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


_HID_EVENT_TAP = 0

# CGEventType
_LEFT_MOUSE_DOWN = 1
_LEFT_MOUSE_UP = 2
_MOUSE_MOVED = 5
_LEFT_MOUSE_DRAGGED = 6

_LEFT_BUTTON = 0


# --------------------------------------------------------------------------------------
# Keyboard
# --------------------------------------------------------------------------------------

# macOS virtual keycodes (kVK_*). Only the keys a DS binding can plausibly use.
VK = {
    "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9,
    "b": 11, "q": 12, "w": 13, "e": 14, "r": 15, "y": 16, "t": 17,
    "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23, "=": 24, "9": 25, "7": 26,
    "-": 27, "8": 28, "0": 29, "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35,
    "l": 37, "j": 38, "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44, "n": 45,
    "m": 46, ".": 47, "`": 50,
    "return": 36, "tab": 48, "space": 49, "delete": 51, "escape": 53,
    "left": 123, "right": 124, "down": 125, "up": 126,
    "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96, "f6": 97, "f7": 98,
    "f8": 100, "f9": 101, "f10": 109, "f11": 103, "f12": 111,
    "shift": 56, "control": 59, "option": 58, "command": 55,
}

# Qt::Key -> our key name. melonDS stores keybinds as Qt key codes, so the harness
# reads the user's actual bindings out of melonDS.toml and translates them here
# instead of assuming a default layout.
QT_KEY = {
    0x20: "space", 0x01000000: "escape", 0x01000001: "tab", 0x01000003: "delete",
    0x01000004: "return", 0x01000005: "return",
    0x01000012: "left", 0x01000013: "up", 0x01000014: "right", 0x01000015: "down",
    0x01000020: "shift", 0x01000021: "control", 0x01000023: "option",
    0x2D: "-", 0x3D: "=", 0x5B: "[", 0x5D: "]", 0x5C: "\\", 0x3B: ";",
    0x27: "'", 0x2C: ",", 0x2E: ".", 0x2F: "/", 0x60: "`",
}
for _i in range(26):
    QT_KEY[0x41 + _i] = chr(ord("a") + _i)
for _i in range(10):
    QT_KEY[0x30 + _i] = chr(ord("0") + _i)
for _i in range(12):
    QT_KEY[0x01000030 + _i] = "f%d" % (_i + 1)


def qt_key_to_name(code: int):
    """Translate a Qt key code from melonDS.toml into a name usable with VK.

    melonDS packs modifier bits into the high bits of the stored value; we only care
    about the base key, so mask them off the way the frontend does.
    """
    if code is None or code < 0:
        return None
    return QT_KEY.get(code & 0x0FFFFFFF)


def _post_key(vk: int, down: bool) -> None:
    q = quartz()
    ev = q.CGEventCreateKeyboardEvent(None, ctypes.c_uint16(vk), ctypes.c_bool(down))
    if not ev:
        raise RuntimeError("CGEventCreateKeyboardEvent failed")
    q.CGEventPost(_HID_EVENT_TAP, ev)
    q.CFRelease(ev)


def key_down(name: str) -> None:
    _post_key(_vk(name), True)


def key_up(name: str) -> None:
    _post_key(_vk(name), False)


def key_tap(name: str, hold: float = 0.03) -> None:
    key_down(name)
    time.sleep(hold)
    key_up(name)


def _vk(name: str) -> int:
    try:
        return VK[name.lower()]
    except KeyError:
        raise KeyError(
            "no macOS keycode for %r; add it to mac.VK" % (name,)
        ) from None


# --------------------------------------------------------------------------------------
# Mouse (the DS touch screen)
# --------------------------------------------------------------------------------------


def _post_mouse(kind: int, x: float, y: float) -> None:
    q = quartz()
    ev = q.CGEventCreateMouseEvent(
        None, ctypes.c_uint32(kind), CGPoint(x, y), ctypes.c_uint32(_LEFT_BUTTON)
    )
    if not ev:
        raise RuntimeError("CGEventCreateMouseEvent failed")
    q.CGEventPost(_HID_EVENT_TAP, ev)
    q.CFRelease(ev)


def mouse_move(x: float, y: float) -> None:
    _post_mouse(_MOUSE_MOVED, x, y)


def mouse_down(x: float, y: float) -> None:
    _post_mouse(_LEFT_MOUSE_DOWN, x, y)


def mouse_up(x: float, y: float) -> None:
    _post_mouse(_LEFT_MOUSE_UP, x, y)


def mouse_drag(x: float, y: float) -> None:
    _post_mouse(_LEFT_MOUSE_DRAGGED, x, y)


# --------------------------------------------------------------------------------------
# Windows
# --------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int

    def as_region(self) -> str:
        return "%d,%d,%d,%d" % (self.x, self.y, self.w, self.h)


def osascript(script: str) -> str:
    proc = subprocess.run(
        ["osascript", "-e", script], capture_output=True, text=True
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "osascript failed: %s" % (proc.stderr.strip() or proc.stdout.strip())
        )
    return proc.stdout.strip()


def window_rect(app_process: str, index: int = 1) -> Rect:
    """Bounds of an app's window, in screen points (not pixels)."""
    out = osascript(
        'tell application "System Events" to tell process "%s" to '
        "get {position, size} of window %d" % (app_process, index)
    )
    parts = [int(p.strip()) for p in out.split(",")]
    if len(parts) != 4:
        raise RuntimeError("unexpected window geometry: %r" % out)
    return Rect(parts[0], parts[1], parts[2], parts[3])


def set_window_rect(app_process: str, rect: Rect, index: int = 1) -> None:
    osascript(
        'tell application "System Events" to tell process "%s" to '
        "set position of window %d to {%d, %d}" % (app_process, index, rect.x, rect.y)
    )
    osascript(
        'tell application "System Events" to tell process "%s" to '
        "set size of window %d to {%d, %d}" % (app_process, index, rect.w, rect.h)
    )


def activate(app_process: str) -> None:
    osascript('tell application "%s" to activate' % app_process)


# --------------------------------------------------------------------------------------
# Capture
# --------------------------------------------------------------------------------------


def capture_region(rect: Rect, out_png: str) -> None:
    """Capture a screen region to PNG. Needs Screen Recording permission."""
    subprocess.run(
        ["screencapture", "-x", "-o", "-R", rect.as_region(), out_png],
        check=True,
        capture_output=True,
    )


def to_bmp(src: str, dst: str, width: int, height: int) -> None:
    """Normalise an image to a fixed size and format with the built-in `sips`.

    Resizing to a canonical size is what makes a reference image portable across
    Retina and non-Retina displays and across window scales. BMP is used because it
    can be parsed without any third-party library.
    """
    subprocess.run(
        ["sips", "-s", "format", "bmp", "-z", str(height), str(width), src, "--out", dst],
        check=True,
        capture_output=True,
    )
