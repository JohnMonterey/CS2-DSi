"""Launch melonDS on a ROM and leave it running for a human to look at.

`make emu-run`. Unlike the test runner this does not take the window over: it sizes it,
reports where the screens are, and then gets out of the way.
"""

from __future__ import annotations

import argparse
import sys

from .melonds import Emulator, MelonDSError, dldi_status, read_config


def main(argv=None):
    parser = argparse.ArgumentParser(description="run a ROM in melonDS")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--wait", action="store_true",
                        help="block until melonDS exits instead of detaching")
    args = parser.parse_args(argv)

    dldi = dldi_status(read_config())
    if not dldi["enabled"]:
        print("warning: DLDI is off, so the game will find no SD card and load no "
              "sounds or config. Run `make emu-doctor`.", file=sys.stderr)

    emu = Emulator(args.rom, scale=args.scale, boot_seconds=0.0)
    try:
        emu.start()
    except MelonDSError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1

    s = emu.screens
    print("melonDS running: %s" % args.rom)
    print("  top screen    %s" % (s.top,))
    print("  touch screen  %s" % (s.bottom,))
    print("  scale         %.2fx" % s.scale)
    if args.wait:
        emu.proc.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
