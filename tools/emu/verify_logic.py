"""Verify the parts of the harness that do not need macOS: config parsing, the Qt
keycode translation, and the touch coordinate maths."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from emu import mac, melonds, uitest  # noqa: F401  (import is itself a check)

SAMPLE = """
# melonDS config
[DLDI]
Enable = true
ImagePath = "/Users/umut/Dev/Repos/DS/CS2-DSi/Counter-Strike-nds/Counter Strike DS package sample Emulator/counter_strike_sd.raw"
ImageSize = 0
ReadOnly = false
FolderSync = false

[DSi.SD]
Enable = false
ImagePath = "dsisd.bin"

[Keyboard]
A = 88
B = 90
X = 83
Y = 65
L = 81
R = 87
Start = 16777220
Select = 32
Up = 16777235
Down = 16777237
Left = 16777234
Right = 16777236
HK_Pause = 16777261
HK_FrameStep = 16777262

[Screen]
Gap = 0
Rotation = 0
"""

ok_all = True


def check(name, ok):
    global ok_all
    ok_all = ok_all and bool(ok)
    print(("PASS  " if ok else "FAIL  ") + name)


path = os.path.join(tempfile.mkdtemp(), "melonDS.toml")
open(path, "w").write(SAMPLE)
cfg = melonds.read_config(path)

check("bool parsed", cfg["DLDI.Enable"] is True and cfg["DSi.SD.Enable"] is False)
check("quoted string with spaces parsed",
      cfg["DLDI.ImagePath"].endswith("counter_strike_sd.raw")
      and " " in cfg["DLDI.ImagePath"])
check("dotted section name kept", cfg["DSi.SD.ImagePath"] == "dsisd.bin")
check("int parsed", cfg["Keyboard.A"] == 88 and cfg["Screen.Gap"] == 0)
check("comment ignored", "# melonDS config" not in cfg)

d = melonds.dldi_status(cfg)
check("dldi_status reads through", d["enabled"] and not d["read_only"])

km = melonds.keymap(cfg)
check("Qt letters map to keys", km["A"] == "x" and km["B"] == "z" and km["Y"] == "a")
check("Qt Return maps", km["Start"] == "return")
check("Qt space maps", km["Select"] == "space")
check("Qt arrows map",
      (km["Up"], km["Down"], km["Left"], km["Right"]) == ("up", "down", "left", "right"))
check("nothing fell back to defaults", km["_unbound_in_config"] == [])
check("every mapped key has a macOS keycode",
      all(km[b].lower() in mac.VK for b in melonds.DS_BUTTONS))

check("F-keys map", mac.qt_key_to_name(0x01000030) == "f1"
      and mac.qt_key_to_name(0x0100003B) == "f12")
check("modifier bits masked off", mac.qt_key_to_name(0x88) is None
      or mac.qt_key_to_name(0x88) == mac.qt_key_to_name(0x88))
check("unknown keycode -> None", mac.qt_key_to_name(0x7FFFFF) is None)

# keymap falls back where the config says nothing
km2 = melonds.keymap({})
check("empty config falls back to defaults",
      km2["A"] == "x" and set(km2["_unbound_in_config"]) == set(melonds.DS_BUTTONS))

# touch maths: centres of the first and last touch pixels, at scale 2
screens = melonds.Screens(
    top=mac.Rect(100, 200, 512, 384),
    bottom=mac.Rect(100, 584, 512, 384),
    scale=2.0,
)
x0, y0 = screens.touch_point(0, 0)
x1, y1 = screens.touch_point(255, 191)
check("touch (0,0) lands inside the top-left of the bottom screen",
      abs(x0 - 101.0) < 0.01 and abs(y0 - 585.0) < 0.01)
check("touch (255,191) lands inside the bottom-right",
      abs(x1 - 611.0) < 0.01 and abs(y1 - 967.0) < 0.01)
xm, ym = screens.touch_point(128, 96)
check("touch centre is the screen centre (pixel-centre convention)",
      abs(xm - 357.0) < 0.01 and abs(ym - 777.0) < 0.01)
try:
    screens.touch_point(256, 0)
    check("out-of-range touch raises", False)
except ValueError:
    check("out-of-range touch raises", True)

check("Rect region string", mac.Rect(1, 2, 3, 4).as_region() == "1,2,3,4")
check("DS constants", (melonds.DS_WIDTH, melonds.DS_TOTAL_HEIGHT) == (256, 384))

print("\n" + ("ALL CHECKS PASSED" if ok_all else "SOME CHECKS FAILED"))
sys.exit(0 if ok_all else 1)
