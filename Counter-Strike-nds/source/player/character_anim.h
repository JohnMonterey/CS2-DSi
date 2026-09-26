// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Drawing animated characters: the rig from character_anim_core, posed with hardware
// matrices.

#ifndef CHARACTER_ANIM_H_ /* Include guard */
#define CHARACTER_ANIM_H_

#include <NEMain.h>
#include "character_anim_core.h"

// Parses the rig built into the game. If that fails, characters are drawn as the static
// model, as they were before.
void CharacterAnim_Init(void);
bool CharacterAnim_Ready(void);
const CharacterRig *CharacterAnim_Rig(void); // NULL unless ready

// Forget a player's motion: call when its model is created. Respawns (dead, then alive)
// and jumps of more than a few units are noticed on their own.
void CharacterAnim_ResetPlayer(int playerIndex);

// Advances every player by the frames since the last call. Once per drawn 3D frame.
void CharacterAnim_UpdatePlayers(void);

// Draws a player with its pose, its model's position, material and scale. Leaves the
// polygon format as it found it.
void CharacterAnim_DrawPlayer(int playerIndex);

// Draws the rig anywhere: x, y, z are world coordinates (f32), yaw is ANIM_TURN units
// with the model's usual meaning, and scale is f32 per axis.
void CharacterRig_Draw(const RigPose *pose, NE_Material *material, int x, int y, int z, int yaw,
                       int scaleX, int scaleY, int scaleZ);

#endif // CHARACTER_ANIM_H_
