"""Boot the ROM and check the first screens the player sees.

This is the template for every other UI test. The shape is always the same: get the
emulator into a known state, take a checkpoint, act, take another checkpoint. Keep
each test to one screen or one transition -- when a whole flow is in one test, a
failure at step 7 tells you nothing about steps 1-6.

First run on a machine:
    make test-ui-update      # writes the reference images
Then LOOK at tests/ui/refs/boot/*.bmp before committing them. A reference captured
from a broken build is a test that passes forever and means nothing.
"""

NAME = "boot to the first interactive screen"

# The intro animates, so the frame we land on is never byte-identical. These limits
# were picked to absorb that; tighten them for a static screen.
TOLERANCE = {"max_mean": 3.0, "max_changed": 0.02}


def run(emu, shot):
    # Emulator.start() already waited --boot-seconds. Give the splash its own time so
    # the checkpoint lands somewhere stable rather than mid-fade.
    emu.wait(3.0)
    shot("splash")

    # Nitro Engine's menu takes Start; if this build opens straight into a touch menu,
    # replace this with emu.touch(x, y) and re-record.
    emu.press("Start")
    emu.wait(2.0)
    shot("after_start")
