// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Nitro Engine glue for the CS:GO movement core.

#include "playermove.h"

#include "collisions.h"
#include "dsidev.h"
#include "input.h"
#include "movement_cfg.h"
#include "movements.h"
#include "sounds.h"
#include "weapons/gun.h"

#include <stdio.h>

static PM_State s_state;
static PM_Tunables s_tunables;
static PM_CSTunables s_cs;
static PM_Events s_events;
static PM_MoveResult s_move;

/* The hull half-height the NE bounding box is currently set to, in f32. */
static int32_t s_halfExtentF32 = 3686; /* 0.9 standing */

/* Set by the touchscreen jump button; consumed by the next tick. */
static bool s_jumpRequested = false;

/* True between PlayerMove_Tick and PlayerMove_PostPhysics in the same frame. */
static bool s_tickRan = false;

/* PlayerMove_Init() runs before dsidev_init(), where dsidev_log() is still a
 * no-op, so the startup config result is stashed and emitted on the first tick
 * instead. A config that silently failed to load looks exactly like one that
 * loaded and changed nothing. */
static char s_startupLog[96];
static bool s_startupLogPending = false;

/* Land sound only fires after a real fall, matching the old 20-frame gate. */
#define PM_LAND_SOUND_MIN_AIR_TICKS 20

static void pm_apply_tunables(void)
{
    PM_TunablesFromCS(&s_cs, &s_tunables);
}

void PlayerMove_Init(void)
{
    PM_CSTunablesDefaults(&s_cs);

    MovementCfgResult res;
    if (MovementCfg_Load(&s_cs, &res))
    {
        snprintf(s_startupLog, sizeof(s_startupLog),
                 "movement.cfg loaded from %s: %d applied, %d unknown, %d bad%s",
                 MovementCfg_LastPath(), res.applied, res.unknown, res.malformed,
                 res.truncated ? ", TRUNCATED" : "");
    }
    else
    {
        snprintf(s_startupLog, sizeof(s_startupLog),
                 "movement.cfg not found; using compiled CS:GO defaults");
    }
    s_startupLogPending = true;

    pm_apply_tunables();
    PlayerMove_Reset();
}

void PlayerMove_ReloadConfig(const char *path)
{
    /* Identify the asset by CONTENT, not by name.
     *
     * A pushed asset is stored on the card as asset<N>.bin (storage.c paths()):
     * the original file name does not survive the transfer, so there is nothing
     * to match "movement.cfg" against. A movement config is instead any file
     * that yields at least one recognised key -- a soundbank or a model pushed
     * through the same callback yields none and is declined.
     *
     * Every outcome is logged. A tuning push that quietly does nothing is worse
     * than one that fails, because the next thing you do is misread the feel of
     * a build that never took the change. */
    PM_CSTunables fresh;
    MovementCfgResult res;
    PM_CSTunablesDefaults(&fresh);

    if (!MovementCfg_LoadPath(path, &fresh, &res))
    {
        dsidev_log("movement reload: cannot open %s", path);
        return;
    }

    if (res.applied == 0)
    {
        dsidev_log("movement reload: %s is not a movement config, ignored", path);
        return;
    }

    s_cs = fresh;
    pm_apply_tunables();
    dsidev_log("movement.cfg reloaded: %d applied, %d unknown, %d bad%s",
               res.applied, res.unknown, res.malformed,
               res.truncated ? ", TRUNCATED" : "");
}

void PlayerMove_Reset(void)
{
    PM_StateResetTuned(&s_state, &s_tunables);
    s_jumpRequested = false;

    Player *player = &AllPlayers[0];
    if (player->PlayerPhysic != NULL)
    {
        player->PlayerPhysic->xspeed = 0;
        player->PlayerPhysic->yspeed = 0;
        player->PlayerPhysic->zspeed = 0;
    }

    /* Put the hull back to standing without moving the feet. */
    int32_t stand = PM_HalfExtentQ12(0);
    if (player->PlayerModel != NULL && s_halfExtentF32 != stand)
    {
        player->PlayerModel->y += stand - s_halfExtentF32;
    }
    s_halfExtentF32 = stand;
    player->ySize = (float)stand / 4096.0f;
    if (player->PlayerPhysic != NULL)
    {
        NE_PhysicsSetSize(player->PlayerPhysic, player->xSize * 2.0f,
                          player->ySize * 2.0f, player->zSize * 2.0f);
    }
}

void PlayerMove_RequestJump(void)
{
    s_jumpRequested = true;
}

/**
 * @brief Resize the bounding box for the current duck amount.
 *
 * Nitro Engine anchors a box on the model coordinate, so the model Y is the
 * player's waist and shrinking the box would otherwise lift the feet off the
 * floor. Re-anchor explicitly: on the ground keep the feet planted, in the air
 * keep the head still so that crouch-jumping actually raises the feet -- which
 * is the whole point of a crouch-jump.
 */
static void pm_resize_hull(Player *player)
{
    int32_t half = s_events.halfExtentQ12; /* Q12 world units == f32 */
    if (half == s_halfExtentF32)
        return;

    int32_t delta = half - s_halfExtentF32;

    if (player->PlayerModel != NULL)
    {
        if (s_state.onGround)
            player->PlayerModel->y += delta; /* feet fixed */
        else
            player->PlayerModel->y -= delta; /* head fixed */
    }

    s_halfExtentF32 = half;
    player->ySize = (float)half / 4096.0f;

    if (player->PlayerPhysic != NULL)
    {
        NE_PhysicsSetSize(player->PlayerPhysic, player->xSize * 2.0f,
                          player->ySize * 2.0f, player->zSize * 2.0f);
    }
}

/** @brief Build a unit wish direction from the d-pad and the view vector. */
static void pm_build_wish(float xWithoutY, float zWithoutY, int32_t *outX,
                          int32_t *outZ)
{
    int forward = 0;
    int side = 0;

    if (isKey(UP_BUTTON))
        forward = 1;
    else if (isKey(DOWN_BUTTON))
        forward = -1;

    if (isKey(RIGHT_BUTTON))
        side = 1;
    else if (isKey(LEFT_BUTTON))
        side = -1;

    if (forward == 0 && side == 0)
    {
        *outX = 0;
        *outZ = 0;
        return;
    }

    /* forward = (xWithoutY, zWithoutY), right = (-zWithoutY, xWithoutY). */
    float wx = forward * xWithoutY + side * -zWithoutY;
    float wz = forward * zWithoutY + side * xWithoutY;

    /* Both axes held gives a vector of length sqrt(2); Source clamps wishvel to
     * maxspeed, so normalise rather than letting diagonals run 41% faster. */
    if (forward != 0 && side != 0)
    {
        wx *= 0.70710678f;
        wz *= 0.70710678f;
    }

    *outX = (int32_t)(wx * (float)PM_Q12_ONE);
    *outZ = (int32_t)(wz * (float)PM_Q12_ONE);
}

void PlayerMove_Tick(float xWithoutY, float zWithoutY, bool frozen)
{
    if (s_startupLogPending)
    {
        s_startupLogPending = false;
        dsidev_log("%s", s_startupLog);
    }

    /* Both jump latches are one-shot and must be consumed even on the early
     * return below: NeedJump is a global the touchscreen button sets and the
     * tutorial reads, and leaving it raised holds in.jump true forever, which
     * pins jumpHeld and stops the player ever jumping again. */
    bool jumpLatched = s_jumpRequested || NeedJump;
    s_jumpRequested = false;
    NeedJump = false;

    Player *player = &AllPlayers[0];
    if (player->PlayerPhysic == NULL || player->PlayerModel == NULL)
        return;

    PM_Input in;
    in.maxSpeedQ8 = 0;
    in.jump = false;
    in.duck = false;
    in.canUnduck = true;
    in.frozen = frozen;
    in.wishXQ12 = 0;
    in.wishZQ12 = 0;

    /* The weapon table is already calibrated against CS speeds: WalkSpeed * 2
     * f32/tick lands on 205-258 CS u/s, which is CS:GO's AWP-to-knife range. */
    int walkSpeed = defaultWalkSpeed;
    if (getPlayerCurrentGunIndex(player) < GunCount)
        walkSpeed = getPlayerCurrentGun(player).WalkSpeed;
    in.maxSpeedQ8 = walkSpeed << 9;

    if (!frozen)
    {
        pm_build_wish(xWithoutY, zWithoutY, &in.wishXQ12, &in.wishZQ12);
        in.jump = isKey(JUMP_BUTTON) || jumpLatched;
        in.duck = isKey(CROUCH_BUTTON);
        /* Tested whenever the player is ducked at all, not only when the button
         * is released: the crouch-spam penalty can drive duck speed below the
         * DuckingEnabled floor, and then the core stands the player up while the
         * button is still held. Without the trace that happens inside a ceiling. */
        if (s_state.duckAmountQ12 > 0)
            in.canUnduck = CanPlayerStandUp(s_state.onGround);
    }

    PM_BeginTick(&s_state, &in, &s_tunables, &s_events);
    s_tickRan = true;

    pm_resize_hull(player);

    /* Round to nearest rather than truncating: velocity itself stays precise in
     * Q8, so the quantisation here is zero-mean instead of a slow drag. */
    s_move.vxBeforeF32 = (s_state.vx + 128) >> PM_Q8_SHIFT;
    s_move.vyBeforeF32 = (s_state.vy + 128) >> PM_Q8_SHIFT;
    s_move.vzBeforeF32 = (s_state.vz + 128) >> PM_Q8_SHIFT;

    player->PlayerPhysic->xspeed = s_move.vxBeforeF32;
    player->PlayerPhysic->yspeed = s_move.vyBeforeF32;
    player->PlayerPhysic->zspeed = s_move.vzBeforeF32;

    /* The tutorial counts distance travelled through these. */
    xSpeedAdded = abs(s_move.vxBeforeF32);
    zSpeedAdded = abs(s_move.vzBeforeF32);
}

void PlayerMove_PostPhysics(void)
{
    Player *player = &AllPlayers[0];
    if (player->PlayerPhysic == NULL)
        return;
    /* Only consume a move we actually issued this frame. */
    if (!s_tickRan)
        return;
    s_tickRan = false;

    s_move.vxAfterF32 = player->PlayerPhysic->xspeed;
    s_move.vyAfterF32 = player->PlayerPhysic->yspeed;
    s_move.vzAfterF32 = player->PlayerPhysic->zspeed;

    PM_EndTick(&s_state, &s_tunables, &s_move, &s_events);

    if (s_events.landed && s_events.airTicksAtLand > PM_LAND_SOUND_MIN_AIR_TICKS)
    {
        Play2DSound(SFX_LAND, 140);
        dsidev_log("land: %d CS u/s after %d ticks, stamina %d%%",
                   (int)((int64_t)s_events.landSpeedQ8 * PM_CS_VEL_DEN /
                         PM_CS_VEL_NUM),
                   (int)s_events.airTicksAtLand, PlayerMove_StaminaPercent());
    }
}

void PlayerMove_NotifyStairSnap(void)
{
    PM_ForceGrounded(&s_state);
}

/* --- queries ------------------------------------------------------------ */

bool PlayerMove_IsOnGround(void)
{
    return s_state.onGround;
}

bool PlayerMove_IsMoving(void)
{
    return PM_HorizontalSpeed(&s_state) != 0;
}

int PlayerMove_SpeedCS(void)
{
    return PM_HorizontalSpeedCS(&s_state);
}

float PlayerMove_EyeOffset(void)
{
    return (float)PM_EyeOffsetQ12(s_state.duckAmountQ12) / 4096.0f;
}

int PlayerMove_EyeOffsetF32(void)
{
    return PM_EyeOffsetQ12(s_state.duckAmountQ12);
}

float PlayerMove_DuckAmount(void)
{
    return (float)s_state.duckAmountQ12 / (float)PM_Q12_ONE;
}

int PlayerMove_DuckAmountQ12(void)
{
    return s_state.duckAmountQ12;
}

int PlayerMove_StaminaPercent(void)
{
    if (s_tunables.staminaMaxQ12 <= 0)
        return 0;
    return (int)(((int64_t)s_state.staminaQ12 * 100) / s_tunables.staminaMaxQ12);
}

const PM_State *PlayerMove_State(void)
{
    return &s_state;
}
