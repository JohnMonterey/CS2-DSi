"""Exercise the UI test runner with a fake emulator, so its control flow is checked
before it ever reaches a Mac. Stubs out everything macOS-specific."""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from emu import image as imglib
from emu import uitest

ok_all = True


def check(name, ok):
    global ok_all
    ok_all = ok_all and bool(ok)
    print(("PASS  " if ok else "FAIL  ") + name)


W, H = 256, 384
STATE = {"tint": 0, "actions": []}


def synth(tint):
    px = bytearray()
    for y in range(H):
        for x in range(W):
            px += bytes(((x + tint) % 256, y % 256, 128))
    return imglib.Image(W, H, px)


class FakeRect:
    pass


class FakeEmulator:
    def __init__(self, rom, scale=2, boot_seconds=6.0):
        self.rom = rom
        STATE["actions"].append("construct")

    def start(self):
        STATE["actions"].append("start")
        return self

    def stop(self):
        STATE["actions"].append("stop")

    def focus(self):
        pass

    def content_rect(self):
        return FakeRect()

    def wait(self, seconds):
        STATE["actions"].append("wait")

    def press(self, button, frames=6):
        STATE["actions"].append("press:" + button)

    def touch(self, x, y, hold=0.08):
        STATE["actions"].append("touch")


class FakeMac:
    @staticmethod
    def capture_region(rect, out_png):
        open(out_png, "wb").write(b"fake png")

    @staticmethod
    def to_bmp(src, dst, width, height):
        imglib.write_bmp(synth(STATE["tint"]), dst)


uitest.Emulator = FakeEmulator
uitest.mac = FakeMac

root = tempfile.mkdtemp()
tests_dir = os.path.join(root, "tests", "ui")
os.makedirs(tests_dir)
open(os.path.join(tests_dir, "demo.py"), "w").write(
    'NAME = "demo"\n'
    'TOLERANCE = {"max_mean": 1.0, "max_changed": 0.001}\n'
    "def run(emu, shot):\n"
    "    emu.wait(1)\n"
    '    shot("one")\n'
    '    emu.press("Start")\n'
    '    shot("two")\n'
)
# a file starting with _ must not be collected
open(os.path.join(tests_dir, "_helper.py"), "w").write("def run(emu, shot):\n    pass\n")

rom = os.path.join(root, "game.nds")
open(rom, "wb").write(b"\x00" * 16)

args = ["--rom", rom, "--tests-dir", tests_dir]

check("discovery skips _-prefixed files",
      [os.path.basename(p) for p in uitest.discover(tests_dir, set())] == ["demo.py"])

# 1. no references yet -> must fail, not silently pass
rc = uitest.main(args)
check("missing reference fails the run", rc == 1)

# 2. --update writes references
rc = uitest.main(args + ["--update"])
refs = sorted(os.listdir(os.path.join(tests_dir, "refs", "demo")))
check("--update exits 0", rc == 0)
check("--update wrote both references", refs == ["one.bmp", "two.bmp"])

# 3. identical capture -> pass
rc = uitest.main(args)
check("matching capture passes", rc == 0)
check("emulator was started and stopped",
      "start" in STATE["actions"] and STATE["actions"][-1] == "stop")
check("test body actually ran", "press:Start" in STATE["actions"])

# 4. changed capture -> fail, with artefacts on disk
STATE["tint"] = 90
rc = uitest.main(args)
out = os.path.join(tests_dir, ".out", "demo")
check("changed capture fails the run", rc == 1)
check("actual capture kept for inspection", os.path.exists(os.path.join(out, "one.png")))
check("diff image written", os.path.exists(os.path.join(out, "one.diff.bmp")))
check("reference was NOT overwritten by a failing run",
      imglib.read_bmp(os.path.join(tests_dir, "refs", "demo", "one.bmp")).pixel(0, 0)
      == synth(0).pixel(0, 0))

# 5. tolerance absorbs small drift
STATE["tint"] = 0
big_tolerance = os.path.join(tests_dir, "loose.py")
open(big_tolerance, "w").write(
    'NAME = "loose"\nTOLERANCE = {"max_mean": 255.0, "max_changed": 1.0}\n'
    "def run(emu, shot):\n    shot('only')\n"
)
uitest.main(args + ["loose", "--update"])
STATE["tint"] = 90
rc = uitest.main(args + ["loose"])
check("a wide tolerance lets drift through", rc == 0)

# 6. selecting one test does not run the other
STATE["actions"] = []
uitest.main(args + ["loose"])
check("named selection runs exactly one test", STATE["actions"].count("start") == 1)

# 7. an exception inside a test is reported as ERROR, not a crash
open(os.path.join(tests_dir, "boom.py"), "w").write(
    "NAME='boom'\ndef run(emu, shot):\n    raise ValueError('kaboom')\n"
)
rc = uitest.main(args + ["boom"])
check("exception in a test fails the run without crashing it", rc == 1)

shutil.rmtree(root)
print("\n" + ("ALL CHECKS PASSED" if ok_all else "SOME CHECKS FAILED"))
sys.exit(0 if ok_all else 1)
