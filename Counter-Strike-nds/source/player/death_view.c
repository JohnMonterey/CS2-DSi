// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// The local player's own death, seen from outside. See death_view.h.

#include "death_view.h"
#include "tombstone.h"
#include "tombstone_core.h"
#include "character_anim_core.h"
#include "main.h"
#include "map.h"
#include "party.h"
#include "playermove.h"

#define VIEW_DISTANCE 2.3f    // from the grave, in front of it
#define VIEW_MIN_DISTANCE 0.7f
#define VIEW_WALL_MARGIN 0.3f // kept between the camera and a wall behind it
#define VIEW_HEIGHT 1.25f     // above the ground at the grave
#define VIEW_LOOK_HEIGHT 0.45f
#define VIEW_CLEARANCE 0.6f   // above the ground under the camera
#define VIEW_TRANSITION 45    // frames to leave the body

static bool active = false;
static bool leaving = false; // the transition's starting view is known
static float fromX, fromY, fromZ, fromLookX, fromLookY, fromLookZ;
static float camX, camZ;

// The last distance worked out, and the direction it was worked out for.
static int32_t distanceYaw = INT32_MIN;
static float distanceCached;
static int distanceMap = -1;

// How far the camera can stand from (ox, oz) along (fx, fz) before a wall: the map's wall
// boxes, sampled every 0.1 along the way at camera height, keeping a margin clear.
static float clearDistance(float ox, float oy, float oz, float fx, float fz)
{
    Wall *walls = getMapWalls();
    int wallsInMap = getMapWallsCount();
    if (walls == NULL)
        return VIEW_DISTANCE;

    // Wall boxes are in a doubled scale: 8192 to the unit.
    const int margin = (int)(VIEW_WALL_MARGIN * 8192.0f);
    int y2 = (int)(oy * 8192.0f);
    for (float d = 0.3f; d <= VIEW_DISTANCE + VIEW_WALL_MARGIN; d += 0.1f)
    {
        int x2 = (int)((ox + fx * d) * 8192.0f), z2 = (int)((oz + fz * d) * 8192.0f);
        for (int i = 0; i < wallsInMap; i++)
        {
            const CollisionBox *box = &walls[i].WallCollisionBox;
            if (x2 >= box->BoxXRangeB - margin && x2 <= box->BoxXRangeA + margin && z2 >= box->BoxZRangeB - margin &&
                z2 <= box->BoxZRangeA + margin && y2 >= box->BoxYRangeB - margin && y2 <= box->BoxYRangeA + margin)
            {
                float clear = d - 0.1f;
                return clear < VIEW_MIN_DISTANCE ? VIEW_MIN_DISTANCE : clear;
            }
        }
    }
    return VIEW_DISTANCE;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Map parts are chosen inside a wedge from the culling origin along two edges, about 56
// degrees either side of a view looking along `radians` (the facing convention: forward is
// (-sin, -cos)), as UpdateLookRotation sets them for a player.
static void aimCulling(float radians)
{
    float spread = 80 * (float)(M_TWOPI / 512.0);
    xWithoutYForOcclusionSide1 = -sinf(radians - spread);
    zWithoutYForOcclusionSide1 = -cosf(radians - spread);
    xWithoutYForOcclusionSide2 = -sinf(radians + spread);
    zWithoutYForOcclusionSide2 = -cosf(radians + spread);
}

bool DeathView_Update(void)
{
    bool wasActive = active;
    active = false;
    int32_t gx, gy, gz, yaw, age;
    float angleAtDeath;
    if (!localPlayer->IsDead || CurrentCameraPlayer != 0 || localPlayer->PlayerModel == NULL ||
        !Tombstone_Spot(0, &gx, &gy, &gz, &yaw, &angleAtDeath, &age))
    {
        // Back in someone's eyes: UpdateLookRotation refreshes the wedge only every other
        // call, and only when the view turns, so aim it along that view now.
        if (wasActive)
            aimCulling(AllPlayers[CurrentCameraPlayer].Angle * (float)(M_TWOPI / 512.0));
        leaving = false;
        return false;
    }

    // Where the view starts: the eyes, as the first-person camera last had them.
    if (!leaving)
    {
        fromX = localPlayer->position.x;
        fromY = localPlayer->position.y + PlayerMove_EyeOffset();
        fromZ = localPlayer->position.z;
        fromLookX = fromX + x;
        fromLookY = fromY + y;
        fromLookZ = fromZ + z;
        leaving = true;
    }

    // In front of the grave, turned by however far the look buttons have turned the view
    // since (they still turn Angle while dead).
    yaw += (int32_t)((localPlayer->Angle - angleAtDeath) * (ANIM_TURN / 512));
    float a = (yaw & (ANIM_TURN - 1)) * (float)(M_TWOPI / ANIM_TURN);
    float fx = -sinf(a), fz = -cosf(a);
    float ox = gx / 4096.0f, oy = gy / 4096.0f, oz = gz / 4096.0f;
    if (yaw != distanceYaw || distanceMap != currentMap)
    {
        distanceCached = clearDistance(ox, oy + 1.1f, oz, fx, fz);
        distanceYaw = yaw;
        distanceMap = currentMap;
    }
    float toX = ox + fx * distanceCached, toZ = oz + fz * distanceCached;
    float toY = oy + VIEW_HEIGHT;
    float under = Tombstone_GroundAt((int32_t)(toX * 4096.0f), (int32_t)(toZ * 4096.0f), (int32_t)(toY * 4096.0f)) / 4096.0f;
    if (toY < under + VIEW_CLEARANCE)
        toY = under + VIEW_CLEARANCE;

    // Out of the body, up and round, easing in and out.
    int32_t t = age * ANIM_ONE / VIEW_TRANSITION;
    float s = AnimEaseInOut(AnimClamp(t, 0, ANIM_ONE)) / (float)ANIM_ONE;
    float lift = sinf(s * (float)M_PI) * 0.5f;
    camX = lerpf(fromX, toX, s);
    float camY = lerpf(fromY, toY, s) + lift;
    camZ = lerpf(fromZ, toZ, s);
    NE_CameraSet(Camera, camX, camY, camZ, lerpf(fromLookX, ox, s), lerpf(fromLookY, oy + VIEW_LOOK_HEIGHT, s),
                 lerpf(fromLookZ, oz, s), 0, 1, 0);

    // Culling looks from the camera, back at the grave.
    aimCulling(a + (float)M_PI);

    active = true;
    return true;
}

bool DeathView_Active(void)
{
    return active;
}

bool DeathView_CullOrigin(int *cx, int *cz)
{
    if (!active)
        return false;
    *cx = (int)(camX * 4096.0f);
    *cz = (int)(camZ * 4096.0f);
    return true;
}
