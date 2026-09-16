"""Check every assumption the UI harness makes, and say exactly what to fix.

Run this first on a new machine, and whenever a UI test fails in a way that looks like
the emulator rather than the game. Each check prints ok / WARN / FAIL and, when it
fails, the one action that fixes it.
"""

from __future__ import annotations

import os
import subprocess
import sys

from . import mac, melonds

OK, WARN, FAIL = "ok  ", "WARN", "FAIL"


class Report:
    def __init__(self):
        self.failed = 0
        self.warned = 0

    def say(self, level, what, detail=""):
        if level == FAIL:
            self.failed += 1
        elif level == WARN:
            self.warned += 1
        print("[%s] %-34s %s" % (level, what, detail))


def check_app(r: Report):
    try:
        app = melonds.find_app()
    except melonds.MelonDSError as exc:
        r.say(FAIL, "melonDS installed", str(exc))
        return None
    r.say(OK, "melonDS installed", app)
    try:
        melonds.binary()
    except melonds.MelonDSError as exc:
        r.say(FAIL, "melonDS binary", str(exc))
    return app


def check_config(r: Report, sd_image=None):
    path = melonds.config_path()
    if not path:
        r.say(FAIL, "melonDS config found",
              "launch melonDS once so it writes melonDS.toml, then re-run")
        return {}
    r.say(OK, "melonDS config found", path)
    config = melonds.read_config(path)

    dldi = melonds.dldi_status(config)
    if not dldi["enabled"]:
        r.say(FAIL, "DLDI SD card enabled",
              "melonDS > Config > Emu settings > DLDI: tick 'Enable DLDI'")
    else:
        r.say(OK, "DLDI SD card enabled", "")
    if sd_image:
        want = os.path.abspath(sd_image)
        have = os.path.abspath(os.path.expanduser(dldi["image"])) if dldi["image"] else ""
        if have != want:
            r.say(FAIL, "DLDI image is the game's",
                  "set it to %s (currently %s)" % (want, have or "unset"))
        else:
            r.say(OK, "DLDI image is the game's", want)
        if not os.path.isfile(want):
            r.say(FAIL, "DLDI image exists", want)
    if dldi["read_only"]:
        r.say(WARN, "DLDI is writable",
              "read-only is on; the game cannot save. Fine for UI tests, not for saves.")

    # Screen layout: the harness maps captures and touches assuming the two screens are
    # stacked with no gap. melonDS's key names for this have moved between versions, so
    # report whatever is there rather than guessing at one.
    layout = {k: v for k, v in config.items()
              if any(t in k for t in ("Gap", "Layout", "Rotation", "Sizing", "Aspect"))}
    if layout:
        r.say(OK, "screen layout keys", ", ".join("%s=%s" % kv for kv in sorted(layout.items())))
        gaps = [v for k, v in layout.items() if "Gap" in k and isinstance(v, int)]
        if any(g != 0 for g in gaps):
            r.say(FAIL, "screen gap is 0",
                  "set the screen gap to 0; a gap shifts every capture and touch")
    else:
        r.say(WARN, "screen layout keys", "none found; confirm layout is vertical, gap 0")

    keys = melonds.keymap(config)
    unbound = keys.pop("_unbound_in_config", [])
    r.say(OK, "DS button mapping", ", ".join("%s=%s" % (b, keys[b]) for b in melonds.DS_BUTTONS))
    if unbound:
        r.say(WARN, "buttons read from config",
              "%s fell back to the harness default; rebind in melonDS to be sure"
              % ", ".join(unbound))
    unmapped = [b for b in melonds.DS_BUTTONS if not keys.get(b)]
    if unmapped:
        r.say(FAIL, "every button has a key", "unmapped: %s" % ", ".join(unmapped))

    for hk in ("HK_Pause", "HK_FrameStep"):
        name = melonds.hotkey(hk, config)
        if name:
            r.say(OK, "hotkey %s" % hk, name)
        else:
            r.say(WARN, "hotkey %s" % hk,
                  "unbound; bind it if you want frame-exact captures")
    return config


def check_permissions(r: Report):
    # Accessibility: posting a key event to nothing is harmless, but the call itself
    # fails loudly when the framework cannot be loaded at all.
    try:
        mac.quartz()
        r.say(OK, "Quartz reachable", "")
    except Exception as exc:  # noqa: BLE001
        r.say(FAIL, "Quartz reachable", str(exc))
        return

    try:
        mac.osascript('tell application "System Events" to get name of first process')
        r.say(OK, "Accessibility granted", "System Events responds")
    except RuntimeError as exc:
        r.say(FAIL, "Accessibility granted",
              "System Preferences > Privacy & Security > Accessibility: add the app you "
              "run these tests from (Terminal, iTerm, your editor). %s" % exc)

    # Screen Recording: a capture taken without it returns a picture of the desktop
    # wallpaper instead of the window, so check the binary at least runs.
    try:
        out = subprocess.run(["screencapture", "-h"], capture_output=True, text=True)
        if out.returncode in (0, 1):
            r.say(OK, "screencapture available", "")
    except FileNotFoundError:
        r.say(FAIL, "screencapture available", "missing from this system")
    r.say(WARN, "Screen Recording granted",
          "cannot be detected programmatically. If captures come out as your desktop "
          "wallpaper, grant Screen Recording to the app running the tests.")

    if not shutil_which("sips"):
        r.say(FAIL, "sips available", "needed to normalise captures")
    else:
        r.say(OK, "sips available", "")


def shutil_which(name):
    import shutil
    return shutil.which(name)


def check_rom(r: Report, rom):
    if not rom:
        return
    if os.path.isfile(rom):
        r.say(OK, "ROM present", "%s (%.1f MB)" % (rom, os.path.getsize(rom) / 1e6))
    else:
        r.say(FAIL, "ROM present", "%s -- run `make emu-build` first" % rom)


def main(argv=None):
    import argparse

    parser = argparse.ArgumentParser(description="check the melonDS UI test setup")
    parser.add_argument("--rom", default=None)
    parser.add_argument("--sd", default=None, help="expected DLDI image path")
    args = parser.parse_args(argv)

    if sys.platform != "darwin":
        print("this harness only runs on macOS", file=sys.stderr)
        return 2

    r = Report()
    check_app(r)
    check_config(r, args.sd)
    check_permissions(r)
    check_rom(r, args.rom)

    print("\n%d failing, %d to look at" % (r.failed, r.warned))
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())
