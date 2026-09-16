// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Nitro Engine glue for the CS:GO movement core.

#ifndef PLAYERMOVE_H_ /* Include guard */
#define PLAYERMOVE_H_

#include "main.h"
#include "playermove_core.h"

/*
 * The core (playermove_core.c) owns the physics; this file owns everything that
 * needs the console: reading the d-pad, resizing the bounding box for a crouch,
 * handing velocity to NE_PhysicsUpdate and reading its collision result back.
 *
 * The tick is split in two because Nitro Engine integrates the local player at
 * the END of the frame, inside UpdateEngine(), after rendering. So:
 *
 *     GameLoop()      ... PlayerMove_Tick()        decide velocity
 *     UpdateEngine()  ... NE_PhysicsUpdate()       move and collide
 *                     ... PlayerMove_PostPhysics() absorb the result
 */

/** @brief Load movement.cfg and reset state. Call once, after fatInitDefault(). */
void PlayerMove_Init(void);

/** @brief Re-read the tuning config. Wired to the dsidev asset callback. */
void PlayerMove_ReloadConfig(const char *path);

/** @brief Clear velocity, stamina and crouch. Call on spawn, respawn, teleport. */
void PlayerMove_Reset(void);

/**
 * @brief Decide this frame's velocity from input and hand it to the engine.
 *
 * @param xWithoutY  forward direction X, from UpdateLookRotation()
 * @param zWithoutY  forward direction Z, from UpdateLookRotation()
 * @param frozen     true during freeze time or while dead
 */
void PlayerMove_Tick(float xWithoutY, float zWithoutY, bool frozen);

/** @brief Absorb the collision result, detect landing, play the land sound. */
void PlayerMove_PostPhysics(void);

/** @brief Ground snap from the ramp/stairs system, which moves the player
 *         outside the physics engine. */
void PlayerMove_NotifyStairSnap(void);

/* --- queries used by the rest of the game ------------------------------- */

bool PlayerMove_IsOnGround(void);

/**
 * @brief True when the local player has any horizontal speed.
 *
 * Replaces isLocalPlayerMoving(), which summed the SIGNED velocity components
 * and so read "not moving" whenever xspeed happened to equal -zspeed. It gates
 * bomb planting and defusing, so that mattered.
 */
bool PlayerMove_IsMoving(void);

/** @brief Horizontal speed in CS units per second, for the debug overlay. */
int PlayerMove_SpeedCS(void);

/** @brief Eye height above the hull centre, in world units. Crouch-aware. */
float PlayerMove_EyeOffset(void);

/** @brief The same, premultiplied by 4096 for the integer raycast paths. */
int PlayerMove_EyeOffsetF32(void);

/** @brief 0 standing, 1 fully ducked. */
float PlayerMove_DuckAmount(void);

/** @brief The same, in the core's Q12 form, for hull arithmetic. */
int PlayerMove_DuckAmountQ12(void);

/** @brief Stamina as a percentage of the maximum, for the overlay. */
int PlayerMove_StaminaPercent(void);

/** @brief Queue a jump from the touchscreen button. */
void PlayerMove_RequestJump(void);

/** @brief Read-only view of the movement state, for the debug overlay. */
const PM_State *PlayerMove_State(void);

#endif // PLAYERMOVE_H_
