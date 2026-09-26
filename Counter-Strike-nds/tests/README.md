# Optimization regressions

From the repository root, run:

```sh
python3 -m unittest discover -s Counter-Strike-nds/tests -v
```

Requires Python 3 and Clang with AddressSanitizer and UndefinedBehaviorSanitizer.
Set `CC` to use another compatible compiler. The tests compile selected production
C functions with minimal platform types; they do not compile the full DS game.

Coverage includes all waypoint pairs on all seven maps across repeated loads,
integer-distance tie behavior, fragmented and oversized network packets, wall
visibility masks, collision-cache invalidation, trig precision, and map teardown.

The path matrix payload hash records the original 171,845 Boolean entries before
packing. Matrices use row-major bits, least significant bit first, with each
matrix padded to a whole byte. To update exported map data, pack entry `i` into
byte `i / 8`, bit `i % 8`, and update the equivalence fixture after checking the
new graph. Keep route tests passing when changing waypoints or matrix data.

Raw matrix storage is now 21,539 bytes instead of 687,380 bytes, with no heap
copies. These are source-data sizes, not measured executable or total RAM sizes.
Per-zone raycast masks and player collision caches add a small amount of memory;
path buffers also grew to safely hold routes longer than the old 15-entry limit.

Before release, build with devkitARM and the modified Nitro Engine, then check
repeated map/match changes, ten-player offline matches, shotgun bursts, grenades,
spectating, minimap use, and multiplayer packet handling on DS hardware or an
emulator. Record frame time and heap usage in these scenes to quantify gains.

Characters no longer use Nitro Engine's keyframe animation (`NE_ModelAnimateAll()`).
`source/player/character_anim.c` poses every player, visible or not, once per drawn
frame, so culling a model never changes its animation timing. The pose code and the
rig file it reads are covered by `make test-anim` (`tests/anim/test_anim.c`), as are the
tombstones the dead leave (`source/player/tombstone_core.c`): their mesh, the timing of the
body's fade and the stone's rise, and where the ground is under a death.
