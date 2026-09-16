"""Locating, configuring and driving melonDS on macOS.

melonDS has no scripting or automation interface -- the developers say so on their own
forum, and their standing suggestion for automation is BizHawk, which does not run
usefully on Apple Silicon. So everything here is built on top of the two things melonDS
does expose: a ROM path on the command line, and a readable config file. Input and
capture go through macOS itself (see mac.py).
"""

from __future__ import annotations

import os
import re
import subprocess
import time
from dataclasses import dataclass

from . import mac

APP_PROCESS = "melonDS"

DS_WIDTH = 256
DS_SCREEN_HEIGHT = 192
DS_TOTAL_HEIGHT = DS_SCREEN_HEIGHT * 2  # 384, both screens, no gap

# Homebrew's melonDS cask was disabled on 2026-09-01: Homebrew 5.0 stopped shipping
# casks that fail the macOS Gatekeeper check, and melonDS's macOS builds are unsigned.
# So the app can be anywhere -- a source build's output directory as easily as
# /Applications. MELONDS_APP wins over all of these.
APP_CANDIDATES = [
    "/Applications/melonDS.app",
    os.path.expanduser("~/Applications/melonDS.app"),
    os.path.expanduser("~/Dev/Repos/melonDS/build/melonDS.app"),
    os.path.expanduser("~/melonDS/build/melonDS.app"),
]

CONFIG_CANDIDATES = [
    "~/Library/Application Support/melonDS/melonDS.toml",
    "~/.config/melonDS/melonDS.toml",
    "~/Library/Application Support/melonDS/melonDS.ini",
    "~/.config/melonDS/melonDS.ini",
]

DS_BUTTONS = ["A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y"]


class MelonDSError(RuntimeError):
    pass


# --------------------------------------------------------------------------------------
# Installation
# --------------------------------------------------------------------------------------


def find_app() -> str:
    override = os.environ.get("MELONDS_APP")
    if override:
        path = os.path.expanduser(override)
        if not os.path.isdir(path):
            raise MelonDSError("MELONDS_APP points at nothing: %s" % path)
        return path
    for path in APP_CANDIDATES:
        if os.path.isdir(path) and path.endswith(".app"):
            return path
    # Fall back to a Spotlight query so a source build or a hand-placed copy is found
    # wherever it sits.
    try:
        out = subprocess.run(
            ["mdfind", "kMDItemCFBundleIdentifier == 'net.kuribo64.melonDS'"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip()
    except Exception:
        out = ""
    for line in out.splitlines():
        if line.endswith(".app") and os.path.isdir(line):
            return line
    raise MelonDSError(
        "melonDS.app not found. The Homebrew cask was disabled on 2026-09-01 (unsigned "
        "build, fails Gatekeeper), so install it from source -- see CLAUDE.md, "
        "'One-time setup' -- or point MELONDS_APP at an existing copy."
    )


def binary() -> str:
    app = find_app()
    exe = os.path.join(app, "Contents", "MacOS", "melonDS")
    if not os.path.isfile(exe):
        raise MelonDSError("melonDS.app has no Contents/MacOS/melonDS: %s" % app)
    return exe


# --------------------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------------------


def config_path():
    for path in CONFIG_CANDIDATES:
        full = os.path.expanduser(path)
        if os.path.isfile(full):
            return full
    return None


_SECTION = re.compile(r"^\[([^\]]+)\]\s*$")
_ENTRY = re.compile(r"^([A-Za-z0-9_.\-]+)\s*=\s*(.+?)\s*$")


def read_config(path: str = None) -> dict:
    """Flat {"DLDI.Enable": True, ...} view of melonDS.toml.

    melonDS 1.x writes plain TOML tables with scalar values, so a full TOML parser is
    unnecessary -- and `tomllib` does not exist on the Python 3.9 that macOS ships.
    """
    path = path or config_path()
    if not path:
        return {}
    out = {}
    section = ""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue
            m = _SECTION.match(line)
            if m:
                section = m.group(1).strip()
                continue
            m = _ENTRY.match(line)
            if not m:
                continue
            key, value = m.group(1), m.group(2)
            full = "%s.%s" % (section, key) if section else key
            out[full] = _coerce(value)
    return out


def _coerce(value: str):
    value = value.strip()
    if value.endswith(","):
        value = value[:-1].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    low = value.lower()
    if low == "true":
        return True
    if low == "false":
        return False
    try:
        if low.startswith("0x"):
            return int(value, 16)
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value


DEFAULT_KEYMAP = {
    # Only used when melonDS.toml has no binding for a button; melonDS's own defaults
    # are the authority and are read from the config whenever they are present.
    "A": "x", "B": "z", "X": "s", "Y": "a",
    "L": "q", "R": "w",
    "Start": "return", "Select": "space",
    "Up": "up", "Down": "down", "Left": "left", "Right": "right",
}


def keymap(config: dict = None) -> dict:
    """{"A": "x", ...} -- DS button to a key name understood by mac.key_down."""
    config = read_config() if config is None else config
    out = {}
    missing = []
    for button in DS_BUTTONS:
        raw = config.get("Keyboard.%s" % button)
        name = mac.qt_key_to_name(raw) if isinstance(raw, int) else None
        if name is None:
            name = DEFAULT_KEYMAP.get(button)
            missing.append(button)
        out[button] = name
    out["_unbound_in_config"] = missing
    return out


def hotkey(name: str, config: dict = None):
    """A melonDS hotkey (e.g. "HK_Pause", "HK_FrameStep") as a mac key name, or None."""
    config = read_config() if config is None else config
    raw = config.get("Keyboard.%s" % name)
    return mac.qt_key_to_name(raw) if isinstance(raw, int) else None


def dldi_status(config: dict = None) -> dict:
    config = read_config() if config is None else config
    return {
        "enabled": bool(config.get("DLDI.Enable", False)),
        "image": config.get("DLDI.ImagePath", ""),
        "read_only": bool(config.get("DLDI.ReadOnly", False)),
        "folder_sync": bool(config.get("DLDI.FolderSync", False)),
    }


# --------------------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------------------


@dataclass
class Screens:
    """Where the two DS screens sit, in macOS screen points."""
    top: mac.Rect
    bottom: mac.Rect
    scale: float

    def touch_point(self, tx: int, ty: int):
        """DS touch coordinate (0-255, 0-191) -> screen point on the bottom screen."""
        if not (0 <= tx < DS_WIDTH and 0 <= ty < DS_SCREEN_HEIGHT):
            raise ValueError("touch (%d,%d) is outside the DS touch screen" % (tx, ty))
        x = self.bottom.x + (tx + 0.5) * self.bottom.w / float(DS_WIDTH)
        y = self.bottom.y + (ty + 0.5) * self.bottom.h / float(DS_SCREEN_HEIGHT)
        return x, y


class Emulator:
    """A running melonDS process with a known window geometry."""

    def __init__(self, rom: str, scale: int = 2, boot_seconds: float = 6.0):
        self.rom = os.path.abspath(rom)
        self.scale = scale
        self.boot_seconds = boot_seconds
        self.proc = None
        self.config = read_config()
        self.keys = keymap(self.config)
        self._screens = None

    # -- lifecycle ---------------------------------------------------------------
    def start(self):
        if not os.path.isfile(self.rom):
            raise MelonDSError("ROM not found: %s" % self.rom)
        self.proc = subprocess.Popen(
            [binary(), self.rom],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._wait_for_window()
        self.layout()
        time.sleep(self.boot_seconds)
        return self

    def _wait_for_window(self, timeout: float = 25.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise MelonDSError(
                    "melonDS exited immediately (code %d). Run it by hand once: it may "
                    "be waiting on a Gatekeeper prompt or a missing BIOS." % self.proc.returncode
                )
            try:
                mac.window_rect(APP_PROCESS)
                return
            except RuntimeError as exc:
                last = exc
                time.sleep(0.4)
        raise MelonDSError(
            "melonDS window never appeared (%s). If the error mentions assistive access, "
            "grant Accessibility to the program running these tests." % last
        )

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False

    # -- geometry ----------------------------------------------------------------
    def layout(self) -> Screens:
        """Pin the window to an exact multiple of the DS resolution and measure it.

        The content height is derived from the measured width rather than from a
        hardcoded title-bar height, so this stays correct if macOS changes its window
        chrome. It assumes the vertical screen layout with a zero screen gap -- see
        `doctor.py`, which checks that.
        """
        want_w = DS_WIDTH * self.scale
        want_h = DS_TOTAL_HEIGHT * self.scale
        mac.activate(APP_PROCESS)
        mac.set_window_rect(APP_PROCESS, mac.Rect(60, 80, want_w, want_h + 28))
        time.sleep(0.4)
        frame = mac.window_rect(APP_PROCESS)

        content_h = frame.w * DS_TOTAL_HEIGHT / float(DS_WIDTH)
        chrome = frame.h - content_h
        if not (0 <= chrome <= 60):
            raise MelonDSError(
                "window is %dx%d, which is not a 256:384 content area plus a title bar. "
                "Check that the screen layout is vertical ('Natural') and the screen gap "
                "is 0 in melonDS's Screen settings." % (frame.w, frame.h)
            )
        content_y = int(round(frame.y + chrome))
        half = int(round(content_h / 2.0))
        self._screens = Screens(
            top=mac.Rect(frame.x, content_y, frame.w, half),
            bottom=mac.Rect(frame.x, content_y + half, frame.w, half),
            scale=frame.w / float(DS_WIDTH),
        )
        return self._screens

    @property
    def screens(self) -> Screens:
        if self._screens is None:
            self.layout()
        return self._screens

    def content_rect(self) -> mac.Rect:
        s = self.screens
        return mac.Rect(s.top.x, s.top.y, s.top.w, s.top.h + s.bottom.h)

    # -- input -------------------------------------------------------------------
    def focus(self):
        mac.activate(APP_PROCESS)
        time.sleep(0.15)

    def press(self, button: str, frames: int = 6):
        """Hold a DS button for roughly `frames` frames at 60 Hz, then release."""
        key = self._key(button)
        mac.key_down(key)
        time.sleep(max(frames, 1) / 60.0)
        mac.key_up(key)

    def hold(self, button: str, seconds: float):
        key = self._key(button)
        mac.key_down(key)
        time.sleep(seconds)
        mac.key_up(key)

    def _key(self, button: str) -> str:
        if button not in DS_BUTTONS:
            raise KeyError("%r is not a DS button; expected one of %s"
                           % (button, ", ".join(DS_BUTTONS)))
        key = self.keys.get(button)
        if not key:
            raise MelonDSError(
                "no keyboard binding for %s. Bind it in melonDS (Config > Input) or add "
                "it to melonds.DEFAULT_KEYMAP." % button
            )
        return key

    def touch(self, tx: int, ty: int, hold: float = 0.08):
        x, y = self.screens.touch_point(tx, ty)
        mac.mouse_move(x, y)
        time.sleep(0.02)
        mac.mouse_down(x, y)
        time.sleep(hold)
        mac.mouse_up(x, y)

    def swipe(self, start, end, steps: int = 12, hold: float = 0.02):
        sx, sy = self.screens.touch_point(*start)
        ex, ey = self.screens.touch_point(*end)
        mac.mouse_move(sx, sy)
        mac.mouse_down(sx, sy)
        for i in range(1, steps + 1):
            mac.mouse_drag(sx + (ex - sx) * i / steps, sy + (ey - sy) * i / steps)
            time.sleep(hold)
        mac.mouse_up(ex, ey)

    def wait(self, seconds: float):
        time.sleep(seconds)
