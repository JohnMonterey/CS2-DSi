// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// A gray tombstone, engraved CT or T, rises where each player dies, facing the way they
// last faced, while their body fades out. The mesh, timing and ground search are in
// tombstone_core.c.

#ifndef TOMBSTONE_H_ /* Include guard */
#define TOMBSTONE_H_

#include <stdbool.h>
#include <stdint.h>

// Polygon IDs. In a match the top screen otherwise uses only 0 (the map, bodies, shadows,
// smoke), 1 (the smoke and flash overlays) and 63 (the clear plane). A stone gets its own so
// its outline is antialiased against the map. A fading body gets one per player: a
// translucent pixel is not drawn over another left by the same ID, so two bodies, or a body
// and a shadow, would cut holes in each other.
#define TOMB_POLY_ID 2
#define BODY_FADE_POLY_ID 40 // + player index, up to 49

// A player has just been killed: remember its side and facing as they are now. From
// killPlayer(), before IsDead is set, once per death.
void Tombstone_NoteDeath(int playerIndex);

// Places the stones for deaths noted since the last call. Once per drawn 3D frame, after
// CharacterAnim_UpdatePlayers() (the facing viewers saw comes from the pose).
void Tombstone_Update(void);

// Removes every stone (a new round, match or map), or one player's (the slot is taken by
// someone else, or emptied).
void Tombstone_ClearAll(void);
void Tombstone_ClearPlayer(int playerIndex);

// Where a player's standing stone is: the ground point (f32), its facing (ANIM_TURN units),
// the player's Angle at death (512 per turn) and frames since the death. False if none.
bool Tombstone_Spot(int playerIndex, int32_t *x, int32_t *y, int32_t *z, int32_t *yaw, float *angleAtDeath,
                    int32_t *age);

// The ground at (x, z) for something whose feet are at `feet` (all f32): the highest wall box
// top or ramp surface there no more than 0.45 above the feet, else the feet.
int32_t Tombstone_GroundAt(int32_t x, int32_t z, int32_t feet);

// Draws the stones the camera can see. `visible(zone, x, z)` is the players' own test.
void Tombstone_DrawAll(bool (*visible)(int zone, int x, int z));

#endif // TOMBSTONE_H_
