// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Source-engine (CS:GO) player movement, in fixed point.

#ifndef PLAYERMOVE_CORE_H_ /* Include guard */
#define PLAYERMOVE_CORE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * This file has no dependency on Nitro Engine, on libnds, or on the console. It
 * is pure arithmetic: the same inputs produce the same outputs on the DS and on
 * the host test runner, bit for bit. That is deliberate -- it is the only part
 * of the movement system that can be verified without the hardware, so it holds
 * every decision that could be wrong about *feel*, and none of the plumbing.
 *
 * Units
 * -----
 * The world scale is 1 unit = 40 CS units: the player hull is 1.8 units tall
 * where Source's is 72. Nitro Engine holds positions in f32 (1.0 = 4096), and a
 * tick is one game frame, nominally 1/60 s. So:
 *
 *     CS u/s    -> f32/tick    : * 1.706667
 *     CS u/s^2  -> f32/tick^2  : * 0.028444
 *     f32/tick  -> CS u/s      : * 0.585938
 *
 * Velocity is held in Q8 (f32 << 8) so friction, acceleration and gravity
 * accumulate without truncation drift; it is rounded to plain f32 only at the
 * moment it is handed to the physics engine.
 *
 * Fixed-point conventions:
 *     Q8   velocities and speeds, in f32/tick
 *     Q12  unit vectors, factors in [0,1], stamina, duck amount
 *     Q16  per-tick rates (friction * dt, accelerate * dt, ...)
 */

#define PM_Q8_SHIFT  8
#define PM_Q8_ONE    (1 << PM_Q8_SHIFT)
#define PM_Q12_SHIFT 12
#define PM_Q12_ONE   (1 << PM_Q12_SHIFT)
#define PM_Q16_SHIFT 16
#define PM_Q16_ONE   (1 << PM_Q16_SHIFT)

/* Source leaves Friction() early below 0.1 u/s. That is 0.1 CS u/s in Q8. */
#define PM_MIN_FRICTION_SPEED 44

/* How long a ramp snap keeps the player grounded, matching the old canJump=10. */
#define PM_GROUND_GRACE_TICKS 10

/* Exact conversion factors as rationals, so the config loader, the game and the
 * tests agree to the last bit. CS u/s -> Q8 f32/tick is 1048576/2400. */
#define PM_CS_VEL_NUM 1048576
#define PM_CS_VEL_DEN 2400
/* CS u/s^2 -> Q8 f32/tick^2 is 1048576/144000. */
#define PM_CS_ACC_NUM 1048576
#define PM_CS_ACC_DEN 144000

/* One tick, as a fraction of a second, as a rational. */
#define PM_TICKS_PER_SECOND 60

/**
 * @brief Tuning constants in CS:GO's own units. This is what movement.cfg holds
 *        and what the tests are written against, so the numbers stay readable
 *        against the convars they came from.
 */
typedef struct
{
    float gravity;         /* sv_gravity              800        u/s^2 */
    float jumpImpulse;     /* sv_jump_impulse         301.993377 u/s   */
    float stopSpeed;       /* sv_stopspeed            80         u/s   */
    float airSpeedCap;     /* GetAirSpeedCap()        30         u/s   */
    float friction;        /* sv_friction             5.2              */
    float accelerate;      /* sv_accelerate           5.5              */
    float airAccelerate;   /* sv_airaccelerate        12               */
    float staminaMax;      /* sv_staminamax           80               */
    float staminaRange;    /* STAMINA_RANGE           100              */
    float staminaRecovery; /* sv_staminarecoveryrate  60        /s     */
    float staminaJumpCost; /* sv_staminajumpcost      0.080            */
    float staminaLandCost; /* sv_staminalandcost      0.050            */
    float duckModifier;    /* CS_PLAYER_SPEED_DUCK_MODIFIER  0.34      */
    float bhopFactor;      /* BUNNYJUMP_MAX_SPEED_FACTOR     1.1       */
    float duckSpeedIdeal;  /* CS_PLAYER_DUCK_SPEED_IDEAL     8.0       */
    float duckSpamPenalty; /* crouch spam penalty            2.0       */
} PM_CSTunables;

/**
 * @brief The same constants, pre-converted into the fixed-point domains the
 *        core works in. Built once by PM_TunablesFromCS().
 */
typedef struct
{
    int32_t gravityQ8;           /* f32/tick^2. sv_gravity 800 -> 5825   */
    int32_t jumpImpulseQ8;       /* f32/tick.   301.993 u/s   -> 131943  */
    int32_t stopSpeedQ8;         /* f32/tick.   80 u/s        -> 34952   */
    int32_t airSpeedCapQ8;       /* f32/tick.   30 u/s        -> 13107   */

    int32_t frictionQ16;         /* sv_friction      * dt. 5.2 -> 5681   */
    int32_t accelQ16;            /* sv_accelerate    * dt. 5.5 -> 6007   */
    int32_t airAccelQ16;         /* sv_airaccelerate * dt. 12  -> 13107  */

    int32_t staminaMaxQ12;       /* 80.0                  -> 327680      */
    int32_t staminaRangeQ12;     /* 100.0                 -> 409600      */
    int32_t staminaRecoverQ12;   /* per tick, 60/s        -> 4096        */
    int32_t staminaJumpQ12;      /* jumpcost * impulse    -> 98957       */
    int32_t staminaLandQ16;      /* multiplies a Q8 speed into a Q12 cost */

    int32_t duckModifierQ12;     /* 0.34 -> 1392                         */
    int32_t bhopFactorQ12;       /* 1.1  -> 4505                         */
    int32_t duckSpeedIdealQ12;   /* 8.0  -> 32768                        */
    int32_t duckSpamPenaltyQ12;  /* 2.0  -> 8192                         */
    int32_t duckSpeedRecoverQ12; /* dt * 3.0 -> 204                      */
} PM_Tunables;

/**
 * @brief Everything the movement model remembers between ticks.
 */
typedef struct
{
    int32_t vx, vy, vz;    /* velocity, Q8 f32/tick                         */
    int32_t staminaQ12;    /* 0 .. staminaMaxQ12                            */
    int32_t duckAmountQ12; /* 0 = standing, PM_Q12_ONE = fully ducked       */
    int32_t duckSpeedQ12;  /* CS:GO's m_flDuckSpeed; spamming shrinks it    */
    bool onGround;
    bool jumpHeld;         /* last tick's jump button, for edge detection   */
    bool duckHeld;         /* last tick's duck button, for the spam penalty */
    int32_t airTicks;      /* consecutive ticks off the ground              */
    int32_t groundGraceTicks; /* ramp snap keeps the player grounded briefly */
} PM_State;

/**
 * @brief One tick of player intent.
 */
typedef struct
{
    /* Unit wish direction in world XZ, Q12. Zero length means "no input". */
    int32_t wishXQ12;
    int32_t wishZQ12;
    /* The weapon's max speed, Q8 f32/tick, before stamina and duck scaling. */
    int32_t maxSpeedQ8;
    bool jump;
    bool duck;
    /* False when there is no headroom to stand up; the caller traces for it. */
    bool canUnduck;
    /* True while the player may not act at all (freeze time, dead). */
    bool frozen;
} PM_Input;

/**
 * @brief What the physics engine did with the velocity we handed it. The glue
 *        fills this in after NE_PhysicsUpdate() and passes it to PM_EndTick().
 *
 * Nitro Engine zeroes the speed component of any axis it resolved a collision
 * on, which for axis-aligned geometry is exactly what Source's ClipVelocity
 * does. Comparing before and after is therefore how we learn we hit something.
 */
typedef struct
{
    int32_t vxBeforeF32, vyBeforeF32, vzBeforeF32;
    int32_t vxAfterF32, vyAfterF32, vzAfterF32;
} PM_MoveResult;

/**
 * @brief What happened this tick, for sounds, animation and logging.
 */
typedef struct
{
    bool jumped;
    bool landed;
    bool headBumped;
    int32_t landSpeedQ8;   /* downward speed at the moment of landing     */
    int32_t airTicksAtLand;
    int32_t maxSpeedQ8;    /* the cap actually applied this tick          */
    int32_t wishSpeedQ8;
    int32_t halfExtentQ12; /* current hull half-height, Q12 world units   */
} PM_Events;

/* --- setup -------------------------------------------------------------- */

/** @brief Fill @p cs with CS:GO's shipping defaults. */
void PM_CSTunablesDefaults(PM_CSTunables *cs);

/** @brief Convert CS:GO units into the core's fixed-point domains. */
void PM_TunablesFromCS(const PM_CSTunables *cs, PM_Tunables *out);

/** @brief Reset all movement state, as on spawn or a teleport. */
void PM_StateReset(PM_State *st);

/** @brief The same, but seeding duck speed from the configured ideal. */
void PM_StateResetTuned(PM_State *st, const PM_Tunables *t);

/* --- the tick, in two halves around the physics step -------------------- */

/**
 * @brief Timers, duck, speed caps, jump, friction/acceleration, half gravity.
 *        Call from the game loop, before handing velocity to the engine.
 */
void PM_BeginTick(PM_State *st, const PM_Input *in, const PM_Tunables *t,
                  PM_Events *ev);

/**
 * @brief Absorb the engine's collision result, finish gravity, detect landing.
 *        Call after the physics step has run.
 */
void PM_EndTick(PM_State *st, const PM_Tunables *t, const PM_MoveResult *mr,
                PM_Events *ev);

/**
 * @brief Force the grounded state, for the ramp/stairs snap in collisions.c
 *        which moves the player onto a surface outside the physics engine.
 */
void PM_ForceGrounded(PM_State *st);

/* --- queries ------------------------------------------------------------ */

/** @brief Horizontal speed, Q8 f32/tick. */
int32_t PM_HorizontalSpeed(const PM_State *st);

/** @brief Horizontal speed in whole CS units per second, for the overlay. */
int32_t PM_HorizontalSpeedCS(const PM_State *st);

/**
 * @brief Hull half-height for a duck amount, Q12 world units.
 *        Standing 0.9, fully ducked 0.675 (72 and 54 CS units tall).
 */
int32_t PM_HalfExtentQ12(int32_t duckAmountQ12);

/**
 * @brief Eye height above the hull centre for a duck amount, Q12 world units.
 *        Standing 0.7, fully ducked 0.475 (eye 64 and 46 CS units above feet).
 */
int32_t PM_EyeOffsetQ12(int32_t duckAmountQ12);

/** @brief clamp(1 - stamina/range, 0, 1), Q12. Scales jump height directly. */
int32_t PM_StaminaFactorQ12(const PM_State *st, const PM_Tunables *t);

/* --- helpers shared with the tests -------------------------------------- */

int32_t PM_MulQ12(int32_t a, int32_t bQ12);
int32_t PM_MulQ16(int32_t a, int32_t bQ16);
uint32_t PM_Sqrt64(uint64_t v);

#endif // PLAYERMOVE_CORE_H_
