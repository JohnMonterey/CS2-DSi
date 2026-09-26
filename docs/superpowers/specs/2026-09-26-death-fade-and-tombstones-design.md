# Fading bodies and tombstones — design

**Date:** 2026-09-26
**Status:** implemented; builds, passes host tests, and was checked in melonDS. Unverified on hardware.
**Builds on:** [character animation and the lobby](2026-09-26-character-animation-and-lobby-design.md)

## Request

> Make it so that upon death, the user slowly turns transparent and leaves a tombstone
> behind, it should be a gray tombstone and on it should read either CT or T. It should face
> the direction last faced before death. Make sure the tombstone extends a bit into the
> ground so that if the player dies at a sloped surface that you can't see through the
> tombstone from below

## What happens

- **Every death** (bots, online players, and the local player): the body falls as before
  (40 frames), then fades, quickly off full strength and then slowly, and is gone 115
  frames (1.9 s) after the death: before the quickest respawn (2 s in training, deathmatch
  and gun game) could pop it back up.
- **A gray tombstone rises** out of the ground where the player died, once the body is faint
  (from frame 76), and stands as the body vanishes. It reads **CT** or **T**, the side the
  player died on, in dark raised letters, and it faces the way the player last faced.
- **Your own death** is seen from outside. The camera leaves your eyes, rises and swings
  round to stand in front of the spot, and looks back: you watch yourself fall and fade and
  your stone rise. The look buttons turn the view round the grave. LEFT and RIGHT still
  spectate the living, and your grave is now one of the views, so you can come back to it.
  While you spectate, your body and stone are drawn like anyone's.
- **One stone per player.** In deathmatch a stone stays after the respawn; the next death
  sinks it back into the ground and raises a new one. A new round, match or map clears them,
  as does a player leaving or a new one taking the slot.

## The stone

A slab 0.54 wide, 0.14 thick and 0.89 tall above the ground (the player's hull is 1.8), with
a round top of 6 segments. 17 polygons, closed, every face wound counter-clockwise from
outside like the rig's so back faces cull; the engraving is 5 quads (CT) or 2 (T), raised
0.03 off the face. Unlit, untextured, vertex-coloured: a cool gray, darker toward the ground,
and 30% darker in the map's shadow zones, as the game darkens the gun there.

The letters stand 0.03 off the face: enough for the depth buffer to keep them in front out
to about 7 units (a step is about 3e-4 * d^2 with the game's projection), past which they are
a few pixels high; and little enough that seen from 60 degrees aside their outer stroke stays
inside the stone's edge (at 0.05 it stood out past it). The equal depth test in the face's
plane was tried and does not work: the large face and the small letters do not interpolate
depth closely enough, and the letters break up.

## Where it stands

`Tomb_Place()`, on the map's collision (wall boxes, whose tops are floors, and the ramps a
standing player's feet follow):

- **The floor** is the highest surface under the hull's centre within a reach above the
  feet (0.45 for the local player, whose feet the physics placed; 0.9 for others, whose
  snapshots and waypoint lines can cut into a floor), or under the hull's footprint within
  0.1. The hull stands on anything it overlaps, so a death on the 0.3 seam at a ramp's top
  lands on the platform, not the floor under the seam. A death in mid-air lands on the floor
  below.
- **Ahead of the centre** by up to 0.25, clear of the fallen body's feet, but always inside
  the hull's square, so never in a wall the player stood against.
- **Off ledges:** a corner over a drop of more than 0.6 moves the stone back onto its floor
  (as far in as its own extent, at most 0.63).
- **Into the ground:** 0.15 below the lowest floor under it on flat ground (a floor can be a
  thin slab over a room), 0.65 below on a ramp (a staircase's steps can lie that far below the
  line players walk on). On a slope the lower end sets the depth.
- **The rise** grows the stone out of its buried bottom rather than sliding it up from
  deeper, so no part of it ever goes below where it rests, through a floor it clears.

Placement and ground are computed once, at the death.

## Drawing

- Stones: immediate mode, one matrix push each, polygon ID 2 (the map uses 0), after the map
  and before the players.
- Fading bodies: translucent, each with its own polygon ID (40 + player): a translucent pixel
  is not drawn over another left by the same ID, so two bodies, or a body and a shadow,
  would cut holes in each other. They are drawn after the solid bodies and shadows, farthest
  first, since the hardware draws translucent polygons in arrival order. A body faded out is
  not drawn at all (alpha 0 would draw it as wireframe).
- Culling follows the players' own test, from the camera. In the death view the culling
  origin and wedge are the view's own, aimed back at the grave, and re-aimed along the
  player's view when it ends.

## Changes by file

| file | change |
| --- | --- |
| `source/player/tombstone_core.{c,h}` | new: the mesh, the timing, the ground probe and `Tomb_Place` (no libnds; host-tested) |
| `source/player/tombstone.{c,h}` | new: deaths noted, stones placed, cleared and drawn |
| `source/player/death_view.{c,h}` | new: the local player's death seen from outside |
| `source/player/character_anim.{c,h}` | the local player is posed too; a body's facing and fade alpha; the local body drawn with its team's skin at the usual scale |
| `source/player/player.c` | `killPlayer` notes the death (once, though online repeats it); a new player clears its slot |
| `source/graphics/draw3d.c` | stones drawn; fading bodies after the rest, farthest first; culling from the death view's camera |
| `source/graphics/camera.c` | the dead local player's grave is one of the spectator views |
| `source/main.c` | the death view places the camera when it is active |
| `source/party/party.c`, `map/map.c`, `network/network.c` | stones cleared on a new round, match, map, or a player leaving |
| `tests/anim/test_anim.c`, `Makefile` | tombstone tests in `make test-anim` |

## Testing

- `make test-anim` (190 checks), new here:
  - the mesh is closed, every edge shared once each way, and every face is wound outward;
  - the engraving is raised on the face, below the round top, reads CT left to right, keeps
    its depth to 7 units, and stays inside the stone's edge seen from 60 degrees aside;
  - it faces the player's last facing;
  - the body stays solid through the fall, fades, stays drawable until gone, and is gone
    before a 2 s respawn;
  - the stone rises once the body is faint, never below its resting bottom, and folds back
    the same way;
  - shadowed stones are darker, and the engraving keeps its contrast;
  - `Tomb_Place` on made-up maps: a floor, mid-air, no floor, a ramp-top seam, both sides of
    a ledge, a slope, a thin slab over a room, a bot beside a curb and one sunk into its
    floor, and the stone inside the hull at every facing.
- melonDS, with scratch builds that kill a bot or the local player on a key and put a camera
  in front of the grave: the fall, the fade and the rise; the engraving; on flat ground, a
  Dust II staircase (0.63 slope), a smooth ramp and the seam at a ramp's top, with no gap
  under the stone seen from downhill; the local death view, spectating away and back, and
  turning round the grave.
- A four-lens critique of the design (DS rendering, lifecycle, ground, visual), each finding
  checked by a second reviewer. It brought in the death view, the timing, the letters'
  height and relief, the footprint-aware placement, the per-stone depth and the rise by
  extrusion, the per-body polygon IDs and ordering, and recording deaths in `killPlayer`.

## Risks

- None of it has been seen on a console. Translucency, polygon IDs and depth precision are
  where melonDS and the hardware could differ.
- Inside one fading body, overlapping parts show in display-list order rather than nearest
  first (the same-ID rule). It shows least with the fade starting after the fall and leaving
  full strength quickly; reordering the rig's list by height in the lying pose would help
  more if it shows on hardware.
- Stones have no collision; players and shots pass through them.
- The local body is drawn from its model, which the online server can move on respawn before
  the player is alive again; by then the body has long faded.

## Deferred

- Reorder the rig's display list for the lying pose (see Risks).
- Collision for stones, if wanted.
