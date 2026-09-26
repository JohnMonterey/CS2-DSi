"""Open the singleplayer lobby from the main menu and check it once it has settled.

Assumes the save already marks the tutorial as done, so the game boots to the main menu
(a fresh SD image starts the tutorial instead; skip it once by hand and let it save).

The lobby animates: its character walks in, then breathes and glances around, dust drifts
through the light and the camera sways a little. The checkpoint waits for the walk-in and
the panel's entrance to finish; the tolerance absorbs the idle motion, like boot's does
for its intro.
"""

NAME = "singleplayer opens the lobby, which settles with the character on its mark"

TOLERANCE = {"max_mean": 3.0, "max_changed": 0.02}


def run(emu, shot):
    emu.wait(2.0)

    # Main menu: "Singleplayer" is the first strip.
    emu.touch(128, 46)

    # Walk-in (76 frames), turn (to frame 84) and the panel's entrance all end well
    # inside three seconds.
    emu.wait(3.0)
    shot("settled")
