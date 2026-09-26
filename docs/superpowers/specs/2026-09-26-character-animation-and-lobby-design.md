# Character animation and the singleplayer lobby — design

**Date:** 2026-09-26
**Status:** implemented; builds, passes host tests, and was checked in melonDS. Unverified on hardware.
**Target:** real DSi hardware, verified by the human operator

## Goal

Make other players look alive and move smoothly, and give singleplayer a CS2-style lobby:
your character standing in a lit scene on the top screen, the match set up below, with
animation throughout rather than a chain of static menus.

### Non-goals

- No new character meshes or textures. The rig is built from the model the game already
  ships, so both skins keep working.
- No crouch or jump poses for other players. Nothing sends another player's crouch or
  jump state, and their hull height already carries both (see *Deferred*).
- The online flow keeps its screens. The lobby is the singleplayer front end only.
- Lobby choices are kept in memory, not in the save file.

## What was wrong

1. **Characters were statues or a flipbook.** By default every other player was the static
   model sliding along the ground. The "(Causes crashes) Animated model" option swapped in
   a 5-frame NEA walk with interpolation turned off (`player.c:729`): three distinct poses,
   stepped every 13 draw calls, so about 4.6 poses a second at 60 Hz and 2.3 in
   dual-screen mode, at the same rate whether a bot was running or standing still.
2. **The option did crash.** `checkAiShoot()` called `NE_ModelAnimSetSpeed()` with no NULL
   check (`ai.c:642`). After a match with it on, stale `isAi` flags reach that call with a
   deleted model, and Nitro Engine writes 512 bytes through `NULL->meshdata`.
3. **Motion snapped.** Bots turned instantly at every waypoint and on every target; the
   first leg of each path faced a direction computed from `Path[-1]`, which reads the
   neighbouring struct field (and could be NaN).
4. **Online players surged.** Each tick closed a tenth of the distance to the latest
   snapshot. Snapshots come about every 6 frames, so a steadily running player was drawn
   lurching forward after each packet and slowing before the next, about 160 ms behind.
5. **Dead players vanished** the frame they died.
6. **Starting a match took four screens**: map, mode, (hidden "Mod" settings for bots),
   then a score table to pick a side.

## Architecture

### Skinned rig, posed by the hardware

The player model is one connected low-poly body (170 polygons) holding a pistol. Cutting it
into rigid parts would tear it open at every joint. Instead it is skinned the way DS games
skin characters: every vertex belongs to one bone, and the display list restores that
bone's matrix from the hardware matrix stack (`MTX_RESTORE`) before sending it. The
geometry engine transforms each vertex as it arrives, so a polygon spanning a joint
stretches rather than splits, and normals sent after the restore light it correctly.

Per character per frame the CPU computes nine bone transforms with ordinary
`glTranslate`/`glRotate` calls and stores them in stack slots 22–29 (Nitro Engine only
pushes a few levels deep and never stores by index). One `glCallList` then draws the
whole character by DMA. Nothing is animated per vertex on the CPU, which is what made the
NEA path slow.

Bones: root (falls pivot at the feet), pelvis, chest, head, arms (forearms, hands and
pistol, pivoting at the shoulders), and thigh and shin on each side.

### Procedural poses

`character_anim_core.c` turns what the game already knows about a character — position,
facing, look angle, weapon recoil, dead or alive — into a pose each frame:

- **Gait follows the ground covered.** The stride phase advances by distance travelled
  divided by a stride length that grows with speed, so feet keep pace with the body at any
  speed and at 30 or 60 Hz. Speed is estimated from the drawn position, so bots,
  online players and the lobby all drive it the same way.
- **Blends, never pops.** Walk/run amplitude, hip turn and aim follow their targets
  exponentially; the knee fold is `((1 ± cos φ)/2)²`, smooth all the way round.
- **Hips turn toward the direction of travel** (±50°) while the chest keeps the aim;
  moving more than 110° from the facing runs the stride backwards, with a 90–110° band so
  a wavering diagonal does not flip it every frame.
- **The planted foot stays down.** Each foot's contact with the ground comes from its
  shin's own vertices, collected when the rig is parsed. The pose turns them with the thigh,
  the shin and the hips' sway, and raises or lowers the body so the lowest one sits where
  the mesh's soles are. The mesh stands mid-stride, so standing up straight lifts the hips a
  little.
- **Facing is smoothed** along the short way round, so a bot that snaps 180° turns over a
  few frames.
- **Idle:** breathing, a slow weight shift and glances, offset per character.
- **Recoil:** the arms kick with the existing per-player recoil counter.
- **Death:** knees give, the body falls back about its feet over 40 frames and stays there
  until the player respawns.

Fixed point throughout (angles 32768 per turn, as `glRotate*i` takes them), so the host
tests and the console compute the same poses. Sine and cosine come from a 257-entry
quarter-wave table: the ARM9 has no divider, and a series needed 64-bit divisions in
software (about 20k cycles per character per frame before the table, none after).

### Lobby

A new menu, `LOBBY`, opened by the main menu's Singleplayer. It replaces the map, mode,
bots and team screens (they remain in the source, unreferenced).

- **Top screen:** a studio drawn from vertex-coloured polygons — no texture memory — with a
  spot of light at the character's feet and a glow in the chosen side's colour behind it.
  The rig walks in from the left, stops on its mark and turns to the camera. A side change
  spins it round, swaps the skin while its back is turned and cross-fades the glow. GO
  raises the pistol, pushes the camera in and fades both screens to black; the match loads
  under that black frame.
- **Bottom screen:** MODE, MAP, BOTS (the six lineups `amountOfBots` × `equalTeam` can make)
  and TEAM rows, then GO. Rows slide in one after another; a changed value slides out and
  its successor in from the side that was pressed; a highlight glides between rows; the
  team pill slides between CT and T. A map that forces a mode (the training map) locks MODE
  and BOTS. Touch, d-pad, A, B and START all work.
- **Starting:** GO sets what the old screens set and calls `StartSinglePlayer()`. The
  match starts on the next pass of `checkStartGameLoop()`, which applies the side chosen in
  the lobby instead of opening the score table.

Everything animates off a frame counter advanced once per 60 Hz frame in `Lobby_Update()`;
menus draw each screen on alternate frames, so draws only sample that state.

### Online smoothing

`remote_lerp.c` draws each online player on a straight segment from where it is drawn now
to the latest snapshot, timed to the longer of the last two gaps between snapshots plus a
frame (the server's 30 Hz loop spaces them 6 and 8 frames apart in turn). A steadily moving
player is drawn moving steadily, one snapshot behind. The game's own teleports (spawns,
resets) are noticed because they move the destination without a snapshot, and snap.

## Changes by file

| file | change |
| --- | --- |
| `tools/assets/player_rig.py` | new: builds `data/player_rig.bin` from `obj_PlayerStatic.bin`; `--preview` renders bones and a test pose |
| `data/player_rig.bin` | new: the skinned player (11.7 KB) |
| `data/obj_PlayerAnim.bin` | removed with the NEA path (27.8 KB) |
| `data/font_cs12.bin` | new: 12 px companion to `font_cs20.bin`, same TTF |
| `source/player/character_anim_core.{c,h}` | new: rig parsing, pose maths (no libnds) |
| `source/player/character_anim.{c,h}` | new: per-player state, bone matrices, drawing |
| `source/graphics/lobby.{c,h}` | new: the lobby |
| `source/network/remote_lerp.{c,h}` | new: snapshot interpolation (no libnds) |
| `source/graphics/draw3d.c` | players drawn with the rig; dead players drawn as bodies; lobby top-screen hook |
| `source/player/player.c` | every player loads the static mesh (no clone of slot 1); `PlayerAnim` removed |
| `source/ai/ai.c` | NEA speed calls removed (the NULL-model crash) |
| `source/player/movements.c` | online players use `remote_lerp`; first path leg no longer reads `Path[-1]` |
| `source/network/network.c` | POS pushes snapshots; LEAVE clears the deleted model pointer |
| `source/graphics/ui.{c,h}` | `LOBBY` id, Singleplayer opens it, `SetMenuDrawsOwnScreen()`, menu hooks exported |
| `source/graphics/font.{c,h}` | `Font_DrawAlpha()`, `Font_TextWidth()` |
| `source/main.c` | rig init, `Lobby_Update()` hook, lobby side applied at match start |
| `Makefile` | `make test-anim` |
| `tests/ui/lobby.py` | UI test: the lobby once settled (references to be recorded on the Mac) |

## Testing

- `make test-anim` (129 checks): fixed-point trig against libm; the shipped rig parses and
  damaged copies are refused; the stride follows distance; backpedal and its hysteresis;
  the same motion at 30 and 60 Hz ends in the same state; turning takes the short way
  round; the planted foot stays on the ground, measured on the mesh itself (every vertex
  skinned in the geometry engine's order, within 0.005 model units through a walk and a
  run); no bone's motion changes abruptly (second
  difference), walking, running, idle and lowering the weapon; the death fall; online
  snapshot interpolation (steady pace, no jumps at packets, no backtracking with irregular
  packets, the relay's 6/8-frame rhythm, counter resets, pauses, teleports).
- melonDS (Linux build, scratch copy of the SD image): skinned players render and animate
  in a match; bodies fall and lie still; the lobby opens, animates, locks rows on the
  training map, spins on a side change, starts a match on the chosen side, returns to the
  main menu with B or X, and opens again after a match.
- melonDS, two instances joined to `tools/server` over melonDS's emulated Wi-Fi, with a
  scratch build graphing the other player's drawn speed per frame: the old lerp drew a
  sawtooth (about 2x the real speed after each packet, a quarter before the next); the new
  one holds near-flat steps around the real pace.
- `tools/assets/player_rig.py` checks that the skinned list sends exactly the original
  polygons, and regenerates `player_rig.bin` byte for byte.
- A four-lens code review (DS hardware, flow and lifecycle, numeric C, tooling), each
  finding checked by a second reviewer. Its fixes: the lobby character's walk-in reset,
  respawns always resetting the gait, alternate polygon IDs for text fading under other
  text, the sine table, the feet's ground contact (it had followed the ankles, 0.42 units
  above the soles, so toes dipped into the floor mid-stride), the rig parser's bounds, and
  the lobby's animation timer overflowing after two hours open.
- The local player's movement against the commit before this work (`9ad56bf`), in melonDS:
  a scratch build of each fed the same 560-tick input script (run, release, backpedal,
  strafe, counter-strafe, diagonal) from the same spot and logged the movement core every
  tick. Speed, velocity and position were identical on every tick. The movement files
  themselves (`playermove*`, `movement_cfg*`, `movement.cfg`) are unchanged.

## Risks

- **`MTX_RESTORE` inside a vertex list** is standard for DS skinning and melonDS draws it
  correctly, but it has not been seen on a console yet.
- **Online smoothing** is host-tested and was measured with two emulated clients, but has
  not been played between consoles.
- **5 vs 5** was labelled "(May cause crashes)" on the old settings screen. The lobby offers
  the same lineups without the label; the rig does not add polygons, but dead bodies now
  stay drawn, so a round's polygon count is the round-start count throughout.

## Deferred

- Save the lobby's choices (append keys after `total_wins`; see `saveManager.c`).
- Return to the lobby rather than the main menu after a singleplayer match.
- Crouch and jump poses for other players, once the protocol carries the state.
- Lobby for online play.
