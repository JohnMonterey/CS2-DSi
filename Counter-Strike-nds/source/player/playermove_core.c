// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Source-engine (CS:GO) player movement, in fixed point.
//
// Written against the public Source SDK 2013 (ValveSoftware/source-sdk-2013,
// game/shared/gamemovement.cpp) for the Friction/Accelerate/AirAccelerate/
// CheckJumpButton structure, which is the Quake -> GoldSrc -> Source lineage.
// The stamina model and PreventBunnyJumping exist only in Counter-Strike, and
// the constants used here are the published convar defaults (sv_friction,
// sv_staminamax, ...). No Valve code is reproduced.

#include "playermove_core.h"

/* ------------------------------------------------------------------------ *
 * Fixed-point helpers
 * ------------------------------------------------------------------------ */

int32_t PM_MulQ12(int32_t a, int32_t bQ12)
{
    return (int32_t)(((int64_t)a * (int64_t)bQ12) >> PM_Q12_SHIFT);
}

int32_t PM_MulQ16(int32_t a, int32_t bQ16)
{
    return (int32_t)(((int64_t)a * (int64_t)bQ16) >> PM_Q16_SHIFT);
}

/**
 * @brief Integer square root of a 64-bit value.
 *
 * Deliberately software, not the ARM9's hardware sqrt: the host test runner and
 * the console must agree bit for bit, and this runs at most twice per tick.
 */
uint32_t PM_Sqrt64(uint64_t v)
{
    uint64_t rem = 0;
    uint64_t root = 0;

    for (int i = 0; i < 32; i++)
    {
        root <<= 1;
        rem = (rem << 2) | (v >> 62);
        v <<= 2;
        if (root < rem)
        {
            rem -= root | 1;
            root += 2;
        }
    }
    return (uint32_t)(root >> 1);
}

static int32_t pm_abs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static int32_t pm_clamp(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/** @brief Move @p cur toward @p target by at most @p step. Source's Approach(). */
static int32_t pm_approach(int32_t target, int32_t cur, int32_t step)
{
    int32_t delta = target - cur;

    if (step < 0)
        step = -step;

    if (delta > step)
        return cur + step;
    if (delta < -step)
        return cur - step;
    return target;
}

/** @brief Divide, rounding to nearest, without pulling in libm. */
static int32_t pm_div_round(int64_t num, int64_t den)
{
    if (den == 0)
        return 0;
    if ((num < 0) != (den < 0))
        return (int32_t)((num - den / 2) / den);
    return (int32_t)((num + den / 2) / den);
}

/** @brief dot(velocity, wishdir) -> Q8, with a Q12 unit wish direction. */
static int32_t pm_dot(const PM_State *st, int32_t wishXQ12, int32_t wishZQ12)
{
    int64_t d = (int64_t)st->vx * (int64_t)wishXQ12 +
                (int64_t)st->vz * (int64_t)wishZQ12;
    return (int32_t)(d >> PM_Q12_SHIFT);
}

/** @brief Scale horizontal velocity by a Q16 fraction. */
static void pm_scale_horizontal(PM_State *st, int32_t fracQ16)
{
    st->vx = PM_MulQ16(st->vx, fracQ16);
    st->vz = PM_MulQ16(st->vz, fracQ16);
}

/* ------------------------------------------------------------------------ *
 * Hull geometry
 *
 * ySize in the Player struct is the HALF extent: the standing hull is 1.8 world
 * units tall (72 CS units) and Nitro Engine anchors the box on the model
 * coordinate, so the model Y is the waist. Ducked is 1.35 units (54 CS units).
 * The eye sits 0.2 units below the hull top in both stances, which is CS's
 * 64-of-72 and 46-of-54.
 * ------------------------------------------------------------------------ */

/* CS:GO's DuckingEnabled() floor: below this the duck button is ignored. */
#define PM_DUCK_SPEED_MIN_Q12 6144 /* 1.5 */

#define PM_HALF_EXTENT_STAND_Q12 3686 /* 0.900 */
#define PM_HALF_EXTENT_DUCK_Q12  2765 /* 0.675 */
#define PM_EYE_STAND_Q12         2867 /* 0.700 */
#define PM_EYE_DUCK_Q12          1946 /* 0.475 */

int32_t PM_HalfExtentQ12(int32_t duckAmountQ12)
{
    int32_t d = pm_clamp(duckAmountQ12, 0, PM_Q12_ONE);
    return PM_HALF_EXTENT_STAND_Q12 +
           PM_MulQ12(PM_HALF_EXTENT_DUCK_Q12 - PM_HALF_EXTENT_STAND_Q12, d);
}

int32_t PM_EyeOffsetQ12(int32_t duckAmountQ12)
{
    int32_t d = pm_clamp(duckAmountQ12, 0, PM_Q12_ONE);
    return PM_EYE_STAND_Q12 + PM_MulQ12(PM_EYE_DUCK_Q12 - PM_EYE_STAND_Q12, d);
}

/* ------------------------------------------------------------------------ *
 * Setup
 * ------------------------------------------------------------------------ */

void PM_CSTunablesDefaults(PM_CSTunables *cs)
{
    cs->gravity = 800.0f;          /* sv_gravity                        */
    cs->jumpImpulse = 301.993377f; /* sv_jump_impulse, sqrt(2*800*57)   */
    cs->stopSpeed = 80.0f;         /* sv_stopspeed                      */
    cs->airSpeedCap = 30.0f;       /* GetAirSpeedCap()                  */
    cs->friction = 5.2f;           /* sv_friction                       */
    cs->accelerate = 5.5f;         /* sv_accelerate                     */
    cs->airAccelerate = 12.0f;     /* sv_airaccelerate                  */
    cs->staminaMax = 80.0f;        /* sv_staminamax                     */
    cs->staminaRange = 100.0f;     /* STAMINA_RANGE                     */
    cs->staminaRecovery = 60.0f;   /* sv_staminarecoveryrate            */
    cs->staminaJumpCost = 0.080f;  /* sv_staminajumpcost                */
    cs->staminaLandCost = 0.050f;  /* sv_staminalandcost                */
    cs->duckModifier = 0.34f;      /* CS_PLAYER_SPEED_DUCK_MODIFIER     */
    cs->bhopFactor = 1.1f;         /* BUNNYJUMP_MAX_SPEED_FACTOR        */
    cs->duckSpeedIdeal = 8.0f;     /* CS_PLAYER_DUCK_SPEED_IDEAL        */
    cs->duckSpamPenalty = 2.0f;    /* crouch spam penalty               */
}

/*
 * The conversions below use floating point on purpose: they run once, when the
 * config is loaded, never per tick. Everything the tick touches is integer.
 */

static int32_t pm_cs_vel_to_q8(float csUnitsPerSecond)
{
    return pm_div_round((int64_t)((double)csUnitsPerSecond * PM_CS_VEL_NUM),
                        PM_CS_VEL_DEN);
}

static int32_t pm_cs_acc_to_q8(float csUnitsPerSecondSq)
{
    return pm_div_round((int64_t)((double)csUnitsPerSecondSq * PM_CS_ACC_NUM),
                        PM_CS_ACC_DEN);
}

/** @brief A per-second rate into a Q16 per-tick multiplier. */
static int32_t pm_rate_to_q16(float perSecond)
{
    return pm_div_round((int64_t)((double)perSecond * PM_Q16_ONE),
                        PM_TICKS_PER_SECOND);
}

/**
 * @brief Clamp a hand-edited config value into a range the tick can survive.
 *
 * movement.cfg is edited by a human on a console with no debugger. A negative
 * friction would make velocity grow about 9% per tick and overflow int32 inside
 * two seconds; a zero stamina range would be a divisor. Values are clamped
 * rather than rejected so a typo degrades the feel instead of the build.
 */
static float pm_clampf(float v, float lo, float hi)
{
    if (!(v >= lo)) /* also catches NaN */
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void PM_TunablesFromCS(const PM_CSTunables *csIn, PM_Tunables *out)
{
    PM_CSTunables safe = *csIn;

    safe.gravity = pm_clampf(safe.gravity, 0.0f, 20000.0f);
    safe.jumpImpulse = pm_clampf(safe.jumpImpulse, 0.0f, 2000.0f);
    safe.stopSpeed = pm_clampf(safe.stopSpeed, 0.0f, 2000.0f);
    safe.airSpeedCap = pm_clampf(safe.airSpeedCap, 0.0f, 2000.0f);
    /* Friction must stay under 1/dt or a tick would reverse the velocity. */
    safe.friction = pm_clampf(safe.friction, 0.0f, 50.0f);
    safe.accelerate = pm_clampf(safe.accelerate, 0.0f, 100.0f);
    safe.airAccelerate = pm_clampf(safe.airAccelerate, 0.0f, 100.0f);
    safe.staminaMax = pm_clampf(safe.staminaMax, 0.0f, 100.0f);
    /* Used as a divisor. */
    safe.staminaRange = pm_clampf(safe.staminaRange, 1.0f, 1000.0f);
    safe.staminaRecovery = pm_clampf(safe.staminaRecovery, 0.0f, 10000.0f);
    safe.staminaJumpCost = pm_clampf(safe.staminaJumpCost, 0.0f, 10.0f);
    safe.staminaLandCost = pm_clampf(safe.staminaLandCost, 0.0f, 10.0f);
    safe.duckModifier = pm_clampf(safe.duckModifier, 0.0f, 1.0f);
    safe.bhopFactor = pm_clampf(safe.bhopFactor, 0.0f, 10.0f);
    /* Used as a rate; zero would mean the crouch never completes. */
    safe.duckSpeedIdeal = pm_clampf(safe.duckSpeedIdeal, 0.1f, 100.0f);
    safe.duckSpamPenalty = pm_clampf(safe.duckSpamPenalty, 0.0f, 100.0f);

    const PM_CSTunables *cs = &safe;

    out->gravityQ8 = pm_cs_acc_to_q8(cs->gravity);
    out->jumpImpulseQ8 = pm_cs_vel_to_q8(cs->jumpImpulse);
    out->stopSpeedQ8 = pm_cs_vel_to_q8(cs->stopSpeed);
    out->airSpeedCapQ8 = pm_cs_vel_to_q8(cs->airSpeedCap);

    out->frictionQ16 = pm_rate_to_q16(cs->friction);
    out->accelQ16 = pm_rate_to_q16(cs->accelerate);
    out->airAccelQ16 = pm_rate_to_q16(cs->airAccelerate);

    out->staminaMaxQ12 = (int32_t)((double)cs->staminaMax * PM_Q12_ONE + 0.5);
    out->staminaRangeQ12 = (int32_t)((double)cs->staminaRange * PM_Q12_ONE + 0.5);
    out->staminaRecoverQ12 =
        pm_div_round((int64_t)((double)cs->staminaRecovery * PM_Q12_ONE),
                     PM_TICKS_PER_SECOND);

    /* CS:GO charges the jump in proportion to the impulse it just applied. */
    out->staminaJumpQ12 = (int32_t)((double)cs->staminaJumpCost *
                                        (double)cs->jumpImpulse * PM_Q12_ONE +
                                    0.5);

    /* Landing charges in proportion to the impact speed, which reaches us as a
     * Q8 f32/tick. Fold the unit conversion into the multiplier so the tick
     * only has to do one multiply. */
    out->staminaLandQ16 = pm_div_round(
        (int64_t)((double)cs->staminaLandCost * PM_Q12_ONE * PM_Q16_ONE *
                  PM_CS_VEL_DEN),
        (int64_t)PM_CS_VEL_NUM);

    out->duckModifierQ12 = (int32_t)((double)cs->duckModifier * PM_Q12_ONE + 0.5);
    out->bhopFactorQ12 = (int32_t)((double)cs->bhopFactor * PM_Q12_ONE + 0.5);
    out->duckSpeedIdealQ12 =
        (int32_t)((double)cs->duckSpeedIdeal * PM_Q12_ONE + 0.5);
    out->duckSpamPenaltyQ12 =
        (int32_t)((double)cs->duckSpamPenalty * PM_Q12_ONE + 0.5);
    /* m_flDuckSpeed recovers toward the ideal at frametime * 3.0. */
    out->duckSpeedRecoverQ12 =
        pm_div_round((int64_t)3 * PM_Q12_ONE, PM_TICKS_PER_SECOND);
}

void PM_StateReset(PM_State *st)
{
    st->vx = 0;
    st->vy = 0;
    st->vz = 0;
    st->staminaQ12 = 0;
    st->duckAmountQ12 = 0;
    st->duckSpeedQ12 = 8 * PM_Q12_ONE; /* overwritten by PM_StateResetTuned */
    st->onGround = false;
    st->jumpHeld = false;
    st->duckHeld = false;
    st->airTicks = 0;
    st->groundGraceTicks = 0;
}

void PM_StateResetTuned(PM_State *st, const PM_Tunables *t)
{
    PM_StateReset(st);
    st->duckSpeedQ12 = t->duckSpeedIdealQ12;
}

/* ------------------------------------------------------------------------ *
 * Queries
 * ------------------------------------------------------------------------ */

int32_t PM_HorizontalSpeed(const PM_State *st)
{
    uint64_t sq = (uint64_t)((int64_t)st->vx * (int64_t)st->vx) +
                  (uint64_t)((int64_t)st->vz * (int64_t)st->vz);
    return (int32_t)PM_Sqrt64(sq);
}

int32_t PM_HorizontalSpeedCS(const PM_State *st)
{
    return pm_div_round((int64_t)PM_HorizontalSpeed(st) * PM_CS_VEL_DEN,
                        PM_CS_VEL_NUM);
}

int32_t PM_StaminaFactorQ12(const PM_State *st, const PM_Tunables *t)
{
    if (t->staminaRangeQ12 <= 0)
        return PM_Q12_ONE;

    int32_t ratio = (int32_t)(((int64_t)st->staminaQ12 << PM_Q12_SHIFT) /
                              t->staminaRangeQ12);
    return pm_clamp(PM_Q12_ONE - ratio, 0, PM_Q12_ONE);
}

/* ------------------------------------------------------------------------ *
 * The move
 * ------------------------------------------------------------------------ */

/**
 * @brief Bleed off ground speed. Source's Friction().
 *
 * This is what makes counter-strafing a skill rather than a key release:
 * letting go of the d-pad costs several ticks of slide, and the `stopspeed`
 * floor means the last of the speed goes all at once rather than asymptotically.
 */
static void pm_friction(PM_State *st, const PM_Tunables *t)
{
    int32_t speed = PM_HorizontalSpeed(st);
    if (speed < PM_MIN_FRICTION_SPEED)
    {
        /* Source returns here and leaves the velocity, because in float it is
         * already indistinguishable from zero. In fixed point it is a value
         * that nothing can ever remove: every later tick takes this same early
         * return, so the residue is permanent and the player reads as moving
         * forever -- which silently blocks planting and defusing. Snap it. */
        st->vx = 0;
        st->vz = 0;
        return;
    }

    int32_t control = (speed < t->stopSpeedQ8) ? t->stopSpeedQ8 : speed;
    int32_t drop = PM_MulQ16(control, t->frictionQ16);

    int32_t newspeed = speed - drop;
    if (newspeed < 0)
        newspeed = 0;

    if (newspeed == speed)
        return;

    if (newspeed == 0)
    {
        st->vx = 0;
        st->vz = 0;
        return;
    }

    pm_scale_horizontal(st, (int32_t)(((int64_t)newspeed << PM_Q16_SHIFT) / speed));
}

/** @brief Source's Accelerate(). */
static void pm_accelerate(PM_State *st, int32_t wishXQ12, int32_t wishZQ12,
                          int32_t wishSpeedQ8, int32_t accelQ16)
{
    int32_t addspeed = wishSpeedQ8 - pm_dot(st, wishXQ12, wishZQ12);
    if (addspeed <= 0)
        return;

    int32_t accelspeed = PM_MulQ16(wishSpeedQ8, accelQ16);
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    st->vx += PM_MulQ12(accelspeed, wishXQ12);
    st->vz += PM_MulQ12(accelspeed, wishZQ12);
}

/**
 * @brief Source's AirAccelerate().
 *
 * The asymmetry below is not a typo and must not be "fixed": the 30 u/s cap
 * limits how much speed you may *add along the wish direction*, but the
 * uncapped wish speed still scales how fast you add it. Turning the view while
 * holding strafe therefore keeps presenting a direction you are not yet moving
 * in, and the cap never binds. That single inconsistency, inherited from Quake,
 * is the whole of air-strafing, and removing it makes jumps feel like a
 * platformer instead of like Counter-Strike.
 */
static void pm_air_accelerate(PM_State *st, int32_t wishXQ12, int32_t wishZQ12,
                              int32_t wishSpeedQ8, const PM_Tunables *t)
{
    int32_t wishspd = wishSpeedQ8;
    if (wishspd > t->airSpeedCapQ8)
        wishspd = t->airSpeedCapQ8;

    int32_t addspeed = wishspd - pm_dot(st, wishXQ12, wishZQ12);
    if (addspeed <= 0)
        return;

    int32_t accelspeed = PM_MulQ16(wishSpeedQ8, t->airAccelQ16);
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    st->vx += PM_MulQ12(accelspeed, wishXQ12);
    st->vz += PM_MulQ12(accelspeed, wishZQ12);
}

/** @brief Advance the duck transition. CS:GO's m_flDuckAmount / m_flDuckSpeed. */
static void pm_duck(PM_State *st, const PM_Input *in, const PM_Tunables *t)
{
    st->duckSpeedQ12 = pm_approach(t->duckSpeedIdealQ12, st->duckSpeedQ12,
                                   t->duckSpeedRecoverQ12);

    /* CS:GO charges the penalty on every press AND every release, not just on
     * the press -- releasing is half of the camp exploit it exists to stop. */
    if (in->duck != st->duckHeld)
    {
        st->duckSpeedQ12 -= t->duckSpamPenaltyQ12;
        if (st->duckSpeedQ12 < 0)
            st->duckSpeedQ12 = 0;
    }
    st->duckHeld = in->duck;

    /* Once duck speed falls below 1.5 the button is ignored outright rather
     * than merely made sluggish: CS:GO's DuckingEnabled() returns false and the
     * player stands up, which is what stops crouch-mashing being used to hover
     * at a chosen half height in front of a target. */
    bool duckWanted = in->duck && st->duckSpeedQ12 >= PM_DUCK_SPEED_MIN_Q12;

    if (duckWanted)
    {
        /* Ducking runs at 0.8 of the current duck speed. 3277 is 0.8 in Q12. */
        int32_t step = PM_MulQ12(st->duckSpeedQ12, 3277) / PM_TICKS_PER_SECOND;
        st->duckAmountQ12 = pm_approach(PM_Q12_ONE, st->duckAmountQ12, step);
    }
    else if (st->duckAmountQ12 > 0 && in->canUnduck)
    {
        /* Standing up never goes below 1.5/s however hard the player spammed. */
        int32_t rate = st->duckSpeedQ12;
        if (rate < PM_DUCK_SPEED_MIN_Q12)
            rate = PM_DUCK_SPEED_MIN_Q12;
        st->duckAmountQ12 = pm_approach(0, st->duckAmountQ12,
                                        rate / PM_TICKS_PER_SECOND);
    }
}

/**
 * @brief CS:GO's PreventBunnyJumping(): clamp horizontal speed at the moment of
 *        the jump so a chain of hops cannot compound speed.
 *
 * Uses the weapon's unmodified max speed, not the stamina/duck-scaled one --
 * otherwise a stamina-slowed player would be clamped twice.
 */
static void pm_prevent_bunny_jumping(PM_State *st, int32_t maxSpeedRawQ8,
                                     const PM_Tunables *t)
{
    int32_t maxScaled = PM_MulQ12(maxSpeedRawQ8, t->bhopFactorQ12);
    if (maxScaled <= 0)
        return;

    int32_t speed = PM_HorizontalSpeed(st);
    if (speed <= maxScaled)
        return;

    pm_scale_horizontal(st, (int32_t)(((int64_t)maxScaled << PM_Q16_SHIFT) / speed));
}

/** @brief Source's CheckJumpButton(), with CS:GO's stamina charge. */
static bool pm_check_jump(PM_State *st, const PM_Input *in, const PM_Tunables *t)
{
    if (!in->jump)
    {
        st->jumpHeld = false;
        return false;
    }

    /* Holding the button does not pogo, and holding it through a landing does
     * not re-jump: the press has to be released and taken again. */
    if (st->jumpHeld)
        return false;
    st->jumpHeld = true;

    if (!st->onGround)
        return false;

    pm_prevent_bunny_jumping(st, in->maxSpeedQ8, t);
    return true;
}

void PM_BeginTick(PM_State *st, const PM_Input *in, const PM_Tunables *t,
                  PM_Events *ev)
{
    ev->jumped = false;
    ev->landed = false;
    ev->headBumped = false;
    ev->landSpeedQ8 = 0;
    ev->airTicksAtLand = 0;
    ev->wishSpeedQ8 = 0;

    /* ReduceTimers: stamina recovers whether grounded or airborne. */
    st->staminaQ12 -= t->staminaRecoverQ12;
    if (st->staminaQ12 < 0)
        st->staminaQ12 = 0;

    pm_duck(st, in, t);
    ev->halfExtentQ12 = PM_HalfExtentQ12(st->duckAmountQ12);

    /* StartGravity: half now, half after the move. Source applies this BEFORE
     * the jump check, so a jump overwrites it rather than being damped by it --
     * which is why the apex works out to exactly sv_jump_impulse^2 / 2g. */
    st->vy -= t->gravityQ8 >> 1;

    if (in->frozen)
    {
        st->vx = 0;
        st->vz = 0;
        ev->maxSpeedQ8 = 0;
        return;
    }

    /* CheckParameters: stamina squared, then the duck taper. */
    int32_t staminaF = PM_StaminaFactorQ12(st, t);
    int32_t maxSpeed = PM_MulQ12(in->maxSpeedQ8, PM_MulQ12(staminaF, staminaF));
    int32_t duckMod = PM_Q12_ONE + PM_MulQ12(t->duckModifierQ12 - PM_Q12_ONE,
                                             st->duckAmountQ12);
    maxSpeed = PM_MulQ12(maxSpeed, duckMod);
    ev->maxSpeedQ8 = maxSpeed;

    if (pm_check_jump(st, in, t))
    {
        int32_t impulse = PM_MulQ12(t->jumpImpulseQ8, staminaF);

        /* Source ADDS the impulse when standing and SETS it when ducked, then
         * calls FinishGravity() there and then -- so the jump tick pays a whole
         * frame of gravity before it moves. That is not a rounding artefact: it
         * is why a CS:GO jump clears 54.6 units rather than the 57 that
         * sqrt(2*g*h) implies from sv_jump_impulse. Reproducing the ordering
         * reproduces the height. */
        if (st->duckAmountQ12 > 0)
            st->vy = impulse;
        else
            st->vy += impulse;
        st->vy -= t->gravityQ8 - (t->gravityQ8 >> 1);

        st->staminaQ12 = pm_clamp(st->staminaQ12 + t->staminaJumpQ12, 0,
                                  t->staminaMaxQ12);
        st->onGround = false;
        st->groundGraceTicks = 0;
        ev->jumped = true;
    }

    bool hasInput = (in->wishXQ12 != 0 || in->wishZQ12 != 0);
    int32_t wishSpeed = hasInput ? maxSpeed : 0;
    ev->wishSpeedQ8 = wishSpeed;

    if (st->onGround)
    {
        pm_friction(st, t);
        if (hasInput)
            pm_accelerate(st, in->wishXQ12, in->wishZQ12, wishSpeed, t->accelQ16);
    }
    else if (hasInput)
    {
        pm_air_accelerate(st, in->wishXQ12, in->wishZQ12, wishSpeed, t);
    }
}

void PM_EndTick(PM_State *st, const PM_Tunables *t, const PM_MoveResult *mr,
                PM_Events *ev)
{
    /* Absorb the engine's clipping. A zeroed component means that axis was
     * resolved against geometry, which for axis-aligned walls is exactly what
     * ClipVelocity would have produced. */
    if (mr->vxBeforeF32 != 0 && mr->vxAfterF32 == 0)
        st->vx = 0;
    if (mr->vzBeforeF32 != 0 && mr->vzAfterF32 == 0)
        st->vz = 0;

    bool yBlocked = (mr->vyBeforeF32 != 0 && mr->vyAfterF32 == 0);

    if (!yBlocked)
    {
        if (st->groundGraceTicks > 0)
        {
            /* Still standing on a ramp the engine knows nothing about. */
            st->groundGraceTicks--;
            st->vy -= t->gravityQ8 - (t->gravityQ8 >> 1);
            return;
        }
        st->onGround = false;
        st->airTicks++;
        /* FinishGravity: the other half. */
        st->vy -= t->gravityQ8 - (t->gravityQ8 >> 1);
        return;
    }

    if (mr->vyBeforeF32 > 0)
    {
        /* Head hit something on the way up. Still airborne. */
        ev->headBumped = true;
        st->vy = 0;
        st->onGround = false;
        st->airTicks++;
        return;
    }

    int32_t landSpeed = pm_abs32(st->vy);
    st->vy = 0;

    if (!st->onGround)
    {
        ev->landed = true;
        ev->landSpeedQ8 = landSpeed;
        ev->airTicksAtLand = st->airTicks;
        st->staminaQ12 = pm_clamp(
            st->staminaQ12 + PM_MulQ16(landSpeed, t->staminaLandQ16), 0,
            t->staminaMaxQ12);
    }

    st->onGround = true;
    st->airTicks = 0;
}

/** @brief Tell the caller the ground state without exposing the struct layout. */
void PM_ForceGrounded(PM_State *st)
{
    st->onGround = true;
    st->airTicks = 0;
    if (st->vy < 0)
        st->vy = 0;
    /* Hold the grounded state open for a few ticks. A ramp only snaps on the
     * frames where the player has sunk below its surface, which while running
     * downhill is a small minority of them; without the grace window a jump
     * input lands on an airborne frame and is swallowed. */
    st->groundGraceTicks = PM_GROUND_GRACE_TICKS;
}
