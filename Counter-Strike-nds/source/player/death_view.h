// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// The local player's own death, seen from outside: the camera leaves the body, rising and
// swinging round to stand in front of the spot, and looks back at it. The player watches
// themself fall and fade, and their tombstone rise. The look buttons turn the view round
// the grave; LEFT and RIGHT still spectate the living, and come back to the grave.

#ifndef DEATH_VIEW_H_ /* Include guard */
#define DEATH_VIEW_H_

#include <stdbool.h>
#include <stdint.h>

// Places the camera while the local player is dead and looking at its own grave. Once per
// GameLoop, after the look rotation is updated. False when that is not the case, and the
// caller places the camera as usual.
bool DeathView_Update(void);

// Whether the last update placed the camera.
bool DeathView_Active(void);

// Where culling should look from while it is active: the camera, not the body (f32).
bool DeathView_CullOrigin(int *x, int *z);

#endif // DEATH_VIEW_H_
