// SPDX-License-Identifier: MIT
//
// Host tests for the CS:GO movement core.
//
// playermove_core.c has no Nitro Engine or libnds dependency, so it compiles
// for the Mac and can be driven against real CS:GO reference numbers. This is
// the only verification of movement that does not require the console.
//
// Build and run:  make test-movement

#include "movement_cfg.h"
#include "playermove_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do                                                                         \
    {                                                                          \
        g_checks++;                                                            \
        if (!(cond))                                                           \
        {                                                                      \
            g_failures++;                                                      \
            printf("  FAIL  " fmt "\n", ##__VA_ARGS__);                        \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected, what)                                       \
    do                                                                         \
    {                                                                          \
        long a_ = (long)(actual);                                              \
        long e_ = (long)(expected);                                            \
        g_checks++;                                                            \
        if (a_ != e_)                                                          \
        {                                                                      \
            g_failures++;                                                      \
            printf("  FAIL  %s: got %ld, want %ld\n", what, a_, e_);           \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, what)                                \
    do                                                                         \
    {                                                                          \
        double a_ = (double)(actual);                                          \
        double e_ = (double)(expected);                                        \
        double t_ = (double)(tol);                                             \
        g_checks++;                                                            \
        if (a_ < e_ - t_ || a_ > e_ + t_)                                      \
        {                                                                      \
            g_failures++;                                                      \
            printf("  FAIL  %s: got %.3f, want %.3f +/- %.3f\n", what, a_, e_, \
                   t_);                                                        \
        }                                                                      \
    } while (0)

static void section(const char *name)
{
    printf("\n%s\n", name);
}

/* ---------------------------------------------------------------------- *
 * A minimal stand-in for Nitro Engine's integrator: position advances by the
 * f32 velocity, a flat floor stops downward motion and zeroes yspeed, exactly
 * as NE_PhysicsUpdate does for an axis-aligned surface with bounce energy 0.
 * ---------------------------------------------------------------------- */

typedef struct
{
    PM_State st;
    PM_Tunables t;
    int32_t feetY; /* f32, floor is 0            */
    int32_t posX;  /* f32                        */
    int32_t posZ;  /* f32                        */
    int32_t ceilingFeetY; /* f32; INT32_MAX for none */
} Sim;

/** @brief Q8 velocity to f32, rounded to nearest, as the glue hands to NE. */
static int32_t q8_to_f32(int32_t q8)
{
    return (q8 + (1 << (PM_Q8_SHIFT - 1))) >> PM_Q8_SHIFT;
}

static void sim_init(Sim *s)
{
    PM_CSTunables cs;
    PM_CSTunablesDefaults(&cs);
    PM_TunablesFromCS(&cs, &s->t);
    PM_StateReset(&s->st);
    s->feetY = 0;
    s->posX = 0;
    s->posZ = 0;
    s->ceilingFeetY = INT32_MAX;
    s->st.onGround = true;
}

static PM_Events sim_tick(Sim *s, const PM_Input *in)
{
    PM_Events ev;
    memset(&ev, 0, sizeof(ev));

    PM_BeginTick(&s->st, in, &s->t, &ev);

    PM_MoveResult mr;
    mr.vxBeforeF32 = q8_to_f32(s->st.vx);
    mr.vyBeforeF32 = q8_to_f32(s->st.vy);
    mr.vzBeforeF32 = q8_to_f32(s->st.vz);

    s->posX += mr.vxBeforeF32;
    s->posZ += mr.vzBeforeF32;
    s->feetY += mr.vyBeforeF32;

    mr.vxAfterF32 = mr.vxBeforeF32;
    mr.vzAfterF32 = mr.vzBeforeF32;
    mr.vyAfterF32 = mr.vyBeforeF32;

    if (s->feetY <= 0)
    {
        s->feetY = 0;
        mr.vyAfterF32 = 0;
    }
    else if (s->ceilingFeetY != INT32_MAX && s->feetY >= s->ceilingFeetY)
    {
        s->feetY = s->ceilingFeetY;
        mr.vyAfterF32 = 0;
    }

    PM_EndTick(&s->st, &s->t, &mr, &ev);
    return ev;
}

/** @brief f32 world units to CS units. 1 world unit = 40 CS units. */
static double f32_to_cs(int32_t f32)
{
    return (double)f32 / 4096.0 * 40.0;
}

/** @brief Weapon WalkSpeed (the game's table) to the core's Q8 max speed. */
static int32_t walkspeed_to_q8(int walkSpeed)
{
    return walkSpeed << 9; /* WalkSpeed * 2 f32/tick, in Q8 */
}

static PM_Input input_none(int32_t maxSpeedQ8)
{
    PM_Input in;
    memset(&in, 0, sizeof(in));
    in.maxSpeedQ8 = maxSpeedQ8;
    in.canUnduck = true;
    return in;
}

/* forward: +Z in the core's terms; any unit direction works. */
static PM_Input input_forward(int32_t maxSpeedQ8)
{
    PM_Input in = input_none(maxSpeedQ8);
    in.wishXQ12 = 0;
    in.wishZQ12 = PM_Q12_ONE;
    return in;
}

/* ---------------------------------------------------------------------- *
 * Tests
 * ---------------------------------------------------------------------- */

static void test_conversions(void)
{
    section("Unit conversions against CS:GO convar defaults");

    PM_CSTunables cs;
    PM_Tunables t;
    PM_CSTunablesDefaults(&cs);
    PM_TunablesFromCS(&cs, &t);

    printf("  gravity      %6d Q8  (%.2f CS u/s^2)\n", t.gravityQ8,
           (double)t.gravityQ8 * PM_CS_ACC_DEN / PM_CS_ACC_NUM);
    printf("  jumpImpulse  %6d Q8  (%.2f CS u/s)\n", t.jumpImpulseQ8,
           (double)t.jumpImpulseQ8 * PM_CS_VEL_DEN / PM_CS_VEL_NUM);
    printf("  stopSpeed    %6d Q8\n", t.stopSpeedQ8);
    printf("  airSpeedCap  %6d Q8\n", t.airSpeedCapQ8);
    printf("  friction     %6d Q16\n", t.frictionQ16);
    printf("  accel        %6d Q16\n", t.accelQ16);
    printf("  airAccel     %6d Q16\n", t.airAccelQ16);
    printf("  staminaJump  %6d Q12 (%.3f)\n", t.staminaJumpQ12,
           (double)t.staminaJumpQ12 / PM_Q12_ONE);
    printf("  staminaLand  %6d Q16\n", t.staminaLandQ16);

    /* Round-trip each constant back to CS units. */
    CHECK_NEAR((double)t.gravityQ8 * PM_CS_ACC_DEN / PM_CS_ACC_NUM, 800.0, 0.2,
               "gravity round-trip");
    CHECK_NEAR((double)t.jumpImpulseQ8 * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
               301.993377, 0.02, "jump impulse round-trip");
    CHECK_NEAR((double)t.stopSpeedQ8 * PM_CS_VEL_DEN / PM_CS_VEL_NUM, 80.0, 0.02,
               "stop speed round-trip");
    CHECK_NEAR((double)t.airSpeedCapQ8 * PM_CS_VEL_DEN / PM_CS_VEL_NUM, 30.0,
               0.02, "air speed cap round-trip");

    CHECK_NEAR((double)t.frictionQ16 * 60.0 / PM_Q16_ONE, 5.2, 0.002,
               "sv_friction round-trip");
    CHECK_NEAR((double)t.accelQ16 * 60.0 / PM_Q16_ONE, 5.5, 0.002,
               "sv_accelerate round-trip");
    CHECK_NEAR((double)t.airAccelQ16 * 60.0 / PM_Q16_ONE, 12.0, 0.002,
               "sv_airaccelerate round-trip");

    CHECK_EQ(t.staminaMaxQ12, 80 * PM_Q12_ONE, "sv_staminamax");
    CHECK_EQ(t.staminaRangeQ12, 100 * PM_Q12_ONE, "STAMINA_RANGE");
    CHECK_EQ(t.staminaRecoverQ12, PM_Q12_ONE, "stamina recovery per tick");
    CHECK_NEAR((double)t.staminaJumpQ12 / PM_Q12_ONE, 24.159, 0.01,
               "stamina cost of one jump");

    /* The published weapon speeds should land on the CS values they mirror. */
    printf("  weapon speeds: knife %.1f  light %.1f  heavy %.1f CS u/s\n",
           (double)walkspeed_to_q8(220) * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
           (double)walkspeed_to_q8(210) * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
           (double)walkspeed_to_q8(175) * PM_CS_VEL_DEN / PM_CS_VEL_NUM);
    CHECK_NEAR((double)walkspeed_to_q8(220) * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
               258.0, 1.0, "knife speed near CS:GO 250");
    CHECK_NEAR((double)walkspeed_to_q8(175) * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
               205.0, 1.0, "heavy speed near CS:GO 200 (AWP)");
}

static void test_hull_geometry(void)
{
    section("Hull and eye height");

    /* Standing: 1.8 units = 72 CS units tall, eye 1.6 above feet = 64. */
    double standHalf = (double)PM_HalfExtentQ12(0) / 4096.0;
    double standEye = (double)PM_EyeOffsetQ12(0) / 4096.0;
    double duckHalf = (double)PM_HalfExtentQ12(PM_Q12_ONE) / 4096.0;
    double duckEye = (double)PM_EyeOffsetQ12(PM_Q12_ONE) / 4096.0;

    printf("  standing hull %.1f CS tall, eye %.1f CS above feet\n",
           standHalf * 2 * 40, (standHalf + standEye) * 40);
    printf("  ducked   hull %.1f CS tall, eye %.1f CS above feet\n",
           duckHalf * 2 * 40, (duckHalf + duckEye) * 40);

    CHECK_NEAR(standHalf * 2 * 40, 72.0, 0.2, "standing hull height (CS 72)");
    CHECK_NEAR((standHalf + standEye) * 40, 64.0, 0.2, "standing eye (CS 64)");
    CHECK_NEAR(duckHalf * 2 * 40, 54.0, 0.2, "ducked hull height (CS 54)");
    CHECK_NEAR((duckHalf + duckEye) * 40, 46.0, 0.2, "ducked eye (CS 46)");
}

static void test_jump_apex(void)
{
    section("Jump apex (CS:GO: 57 units from sv_jump_impulse 301.993)");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    /* Settle on the ground for a few ticks. */
    for (int i = 0; i < 5; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
    }
    CHECK(s.st.onGround, "player should be grounded before jumping");

    PM_Input jumpIn = input_none(maxSpeed);
    jumpIn.jump = true;
    PM_Events ev = sim_tick(&s, &jumpIn);
    CHECK(ev.jumped, "jump should fire on a fresh press while grounded");

    int32_t apex = s.feetY;
    int airTicks = 1;
    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.jump = true; /* held, must not pogo */
        PM_Events e = sim_tick(&s, &in);
        if (s.feetY > apex)
            apex = s.feetY;
        if (e.landed)
            break;
        airTicks++;
    }

    printf("  apex %.2f CS units, airborne %d ticks (%.2f s)\n",
           f32_to_cs(apex), airTicks, airTicks / 60.0);

    /* NOT 57. sv_jump_impulse 301.993 is sqrt(2*800*57), but Source calls
     * FinishGravity() inside CheckJumpButton, so the jump tick pays a whole
     * frame of gravity before it moves and the real height comes out at ~54.6
     * units -- the number CS:GO players actually measure against boxes. */
    CHECK_NEAR(f32_to_cs(apex), 54.6, 1.0, "jump apex in CS units");
    CHECK_NEAR(airTicks / 60.0, 0.74, 0.05, "hang time in seconds");
    CHECK(s.st.onGround, "player should be grounded again after landing");
}

static void test_no_pogo(void)
{
    section("Holding jump must not pogo");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 5; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
    }

    int jumps = 0;
    for (int i = 0; i < 400; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.jump = true; /* never released */
        PM_Events e = sim_tick(&s, &in);
        if (e.jumped)
            jumps++;
    }
    printf("  jumps while holding for 400 ticks: %d\n", jumps);
    CHECK_EQ(jumps, 1, "exactly one jump from a held button");
}

static void test_ground_top_speed(void)
{
    section("Ground acceleration reaches exactly max speed");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    int ticksTo99 = -1;
    for (int i = 0; i < 300; i++)
    {
        PM_Input in = input_forward(maxSpeed);
        sim_tick(&s, &in);
        if (ticksTo99 < 0 && PM_HorizontalSpeed(&s.st) >= (int32_t)((int64_t)maxSpeed * 99 / 100))
            ticksTo99 = i + 1;
    }

    int32_t top = PM_HorizontalSpeed(&s.st);
    printf("  top speed %d Q8 (%.1f CS u/s), 99%% reached in %d ticks (%.2f s)\n",
           top, (double)top * PM_CS_VEL_DEN / PM_CS_VEL_NUM, ticksTo99,
           ticksTo99 / 60.0);

    /* Friction (5.2) is below acceleration (5.5), so the cap is reachable. */
    CHECK_NEAR((double)top, (double)maxSpeed, (double)maxSpeed * 0.01,
               "steady-state speed equals max speed");
    CHECK(ticksTo99 > 0 && ticksTo99 < 60, "should reach 99%% within a second");
}

static void test_diagonal_is_not_faster(void)
{
    section("Diagonal movement must not exceed max speed");

    int32_t maxSpeed = walkspeed_to_q8(220);

    Sim straight;
    sim_init(&straight);
    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_forward(maxSpeed);
        sim_tick(&straight, &in);
    }

    Sim diag;
    sim_init(&diag);
    /* A normalised 45-degree wish direction: 0.70710678 in Q12 is 2896. */
    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.wishXQ12 = 2896;
        in.wishZQ12 = 2896;
        sim_tick(&diag, &in);
    }

    int32_t sSpeed = PM_HorizontalSpeed(&straight.st);
    int32_t dSpeed = PM_HorizontalSpeed(&diag.st);
    printf("  straight %.1f CS u/s, diagonal %.1f CS u/s, ratio %.4f\n",
           (double)sSpeed * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
           (double)dSpeed * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
           (double)dSpeed / (double)sSpeed);

    /* The old MovePlayer() summed the two axes unclamped and ran 1.414x here. */
    CHECK_NEAR((double)dSpeed / (double)sSpeed, 1.0, 0.01,
               "diagonal speed ratio (the old code was 1.414)");
}

static void test_stopping_distance(void)
{
    section("Counter-strafe: stopping distance and time");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_forward(maxSpeed);
        sim_tick(&s, &in);
    }

    int32_t startZ = s.posZ;
    int ticks = 0;
    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_none(maxSpeed); /* released */
        sim_tick(&s, &in);
        ticks++;
        if (PM_HorizontalSpeed(&s.st) == 0)
            break;
    }

    printf("  slid %.1f CS units over %d ticks (%.3f s) after release\n",
           f32_to_cs(s.posZ - startZ), ticks, ticks / 60.0);

    CHECK(PM_HorizontalSpeed(&s.st) == 0, "player should come to a full stop");
    /* Source bleeds ~8.7%% of speed per tick at 60Hz, so a full stop takes a
     * few tenths of a second -- not instant, not a long slide. */
    CHECK(ticks >= 3 && ticks <= 40, "stop should take between 3 and 40 ticks");
    CHECK(f32_to_cs(s.posZ - startZ) > 10.0,
          "there should be real momentum, not an instant stop");
}

static void test_stamina(void)
{
    section("Stamina: the reason bunnyhopping does not work in CS:GO");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 5; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
    }

    PM_Input jumpIn = input_none(maxSpeed);
    jumpIn.jump = true;
    sim_tick(&s, &jumpIn);

    double afterJump = (double)s.st.staminaQ12 / PM_Q12_ONE;
    printf("  stamina right after jumping: %.2f (CS:GO charges 24.16)\n",
           afterJump);
    /* Recovery runs before the jump charge, and stamina was already 0, so the
     * full cost stands. */
    CHECK_NEAR(afterJump, 24.159, 0.2, "stamina after one jump");

    /* Fall to the ground and observe the landing charge. */
    double atLanding = -1.0;
    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_none(maxSpeed);
        PM_Events e = sim_tick(&s, &in);
        if (e.landed)
        {
            atLanding = (double)s.st.staminaQ12 / PM_Q12_ONE;
            printf("  landed at %.1f CS u/s, stamina now %.2f\n",
                   (double)e.landSpeedQ8 * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
                   atLanding);
            break;
        }
    }
    CHECK(atLanding > 0, "should have landed");

    /* Max speed is scaled by the SQUARE of the stamina factor, so right after
     * landing the player is dramatically slower. That is the CS:GO feel. */
    PM_Input probe = input_forward(maxSpeed);
    PM_Events e = sim_tick(&s, &probe);
    double speedFrac = (double)e.maxSpeedQ8 / (double)maxSpeed;
    printf("  max speed right after landing: %.1f%% of normal\n",
           speedFrac * 100.0);
    CHECK(speedFrac < 0.75,
          "landing should bite hard into max speed (squared factor)");

    /* And it recovers within about a second. */
    int recoverTicks = 0;
    for (int i = 0; i < 300; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
        recoverTicks++;
        if (s.st.staminaQ12 == 0)
            break;
    }
    printf("  stamina fully recovered in %d ticks (%.2f s)\n", recoverTicks,
           recoverTicks / 60.0);
    CHECK(recoverTicks > 10 && recoverTicks < 90,
          "stamina should clear in well under 1.5 s");
}

static void test_bhop_does_not_build_speed(void)
{
    section("Chained jumps must not compound speed");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 200; i++)
    {
        PM_Input in = input_forward(maxSpeed);
        sim_tick(&s, &in);
    }
    int32_t runSpeed = PM_HorizontalSpeed(&s.st);

    /* Perfect-timed hops: press jump only on the tick after landing. */
    int32_t peak = runSpeed;
    bool pressNext = true;
    for (int i = 0; i < 900; i++)
    {
        PM_Input in = input_forward(maxSpeed);
        in.jump = pressNext;
        PM_Events e = sim_tick(&s, &in);
        pressNext = e.landed; /* release, then press again the tick after */
        int32_t sp = PM_HorizontalSpeed(&s.st);
        if (sp > peak)
            peak = sp;
    }

    printf("  running %.1f CS u/s, peak over 900 ticks of hopping %.1f CS u/s\n",
           (double)runSpeed * PM_CS_VEL_DEN / PM_CS_VEL_NUM,
           (double)peak * PM_CS_VEL_DEN / PM_CS_VEL_NUM);

    CHECK((double)peak <= (double)runSpeed * 1.15,
          "bunnyhopping must not build meaningful speed");
}

static void test_air_strafing_works(void)
{
    section("Air control: steering mid-jump must do something");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 5; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
    }

    PM_Input jumpIn = input_forward(maxSpeed);
    jumpIn.jump = true;
    sim_tick(&s, &jumpIn);

    /* Hold a pure sideways wish direction while airborne. */
    int32_t startX = s.posX;
    for (int i = 0; i < 30; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.wishXQ12 = PM_Q12_ONE;
        in.wishZQ12 = 0;
        sim_tick(&s, &in);
        if (s.st.onGround)
            break;
    }

    double drift = f32_to_cs(s.posX - startX);
    printf("  sideways drift over 30 airborne ticks: %.1f CS units\n", drift);
    CHECK(drift > 5.0, "air acceleration should move the player sideways");

    /* But the cap must bind: pure sideways air speed stops near 30 u/s. */
    int32_t sideSpeedCS =
        (int32_t)((int64_t)s.st.vx * PM_CS_VEL_DEN / PM_CS_VEL_NUM);
    printf("  sideways air speed: %d CS u/s (cap is 30)\n", sideSpeedCS);
    CHECK(sideSpeedCS <= 35, "air speed cap should bind on a pure strafe");
}

static void test_duck(void)
{
    section("Crouch timing and speed");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 5; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
    }

    int duckTicks = 0;
    for (int i = 0; i < 120; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.duck = true;
        sim_tick(&s, &in);
        duckTicks++;
        if (s.st.duckAmountQ12 >= PM_Q12_ONE)
            break;
    }
    printf("  full duck in %d ticks (%.3f s)\n", duckTicks, duckTicks / 60.0);
    CHECK_NEAR(duckTicks / 60.0, 0.16, 0.06, "time to duck");

    /* Ducked speed should be 34%% of standing. */
    PM_Input probe = input_forward(maxSpeed);
    probe.duck = true;
    PM_Events e = sim_tick(&s, &probe);
    double frac = (double)e.maxSpeedQ8 / (double)maxSpeed;
    printf("  ducked max speed: %.1f%% of standing (CS:GO: 34%%)\n",
           frac * 100.0);
    CHECK_NEAR(frac, 0.34, 0.02, "ducked speed modifier");

    int standTicks = 0;
    for (int i = 0; i < 120; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
        standTicks++;
        if (s.st.duckAmountQ12 == 0)
            break;
    }
    printf("  full stand in %d ticks (%.3f s)\n", standTicks, standTicks / 60.0);
    /* Slower than the nominal 8.0/s: the press charged 2.0 of duck speed and
     * the release charged another 2.0, so standing up runs at about 4.0/s. */
    CHECK_NEAR(standTicks / 60.0, 0.22, 0.06, "time to unduck");
}

static void test_unduck_blocked(void)
{
    section("Crouch: cannot stand up without headroom");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 40; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.duck = true;
        sim_tick(&s, &in);
    }
    CHECK_EQ(s.st.duckAmountQ12, PM_Q12_ONE, "should be fully ducked");

    for (int i = 0; i < 60; i++)
    {
        PM_Input in = input_none(maxSpeed);
        in.duck = false;
        in.canUnduck = false; /* something overhead */
        sim_tick(&s, &in);
    }
    CHECK_EQ(s.st.duckAmountQ12, PM_Q12_ONE,
             "must stay ducked while blocked overhead");
}

static void test_crouch_spam_penalty(void)
{
    section("Crouch spam gets progressively slower");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    int32_t before = s.st.duckSpeedQ12;
    for (int i = 0; i < 6; i++)
    {
        PM_Input down = input_none(maxSpeed);
        down.duck = true;
        sim_tick(&s, &down);
        PM_Input up = input_none(maxSpeed);
        sim_tick(&s, &up);
    }
    printf("  duck speed %.2f -> %.2f after 6 rapid crouches\n",
           (double)before / PM_Q12_ONE, (double)s.st.duckSpeedQ12 / PM_Q12_ONE);
    CHECK(s.st.duckSpeedQ12 < before, "spamming crouch should slow it down");
}

static void test_frozen(void)
{
    section("Freeze time pins the player");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    for (int i = 0; i < 60; i++)
    {
        PM_Input in = input_forward(maxSpeed);
        in.frozen = true;
        in.jump = true;
        sim_tick(&s, &in);
    }
    CHECK_EQ(PM_HorizontalSpeed(&s.st), 0, "frozen player must not move");
    CHECK_EQ(s.posZ, 0, "frozen player must not travel");
    CHECK(s.st.onGround, "frozen player should still rest on the ground");
}

static void test_sqrt(void)
{
    section("Fixed-point helpers");

    CHECK_EQ(PM_Sqrt64(0), 0, "sqrt(0)");
    CHECK_EQ(PM_Sqrt64(1), 1, "sqrt(1)");
    CHECK_EQ(PM_Sqrt64(144), 12, "sqrt(144)");
    CHECK_EQ(PM_Sqrt64(145), 12, "sqrt(145) truncates");
    CHECK_EQ(PM_Sqrt64(1000000ULL * 1000000ULL), 1000000, "sqrt(1e12)");
    CHECK_EQ(PM_Sqrt64(4294967295ULL * 4294967295ULL), 4294967295ULL,
             "sqrt of the largest 32-bit square");

    /* The speed helper must survive the fastest velocity the game can produce. */
    PM_State st;
    PM_StateReset(&st);
    st.vx = 200000;
    st.vz = 200000;
    int32_t sp = PM_HorizontalSpeed(&st);
    CHECK_NEAR((double)sp, 282842.7, 2.0, "speed of a large diagonal velocity");
}

/**
 * @brief Stopping must leave EXACTLY zero speed, every time.
 *
 * pm_friction early-returns below 0.1 CS u/s. In float that is indistinguishable
 * from zero; in fixed point it is a residue nothing can ever remove, because
 * every later tick takes the same early return. PlayerMove_IsMoving() then reads
 * true forever, which silently blocks bomb planting and defusing.
 */
static void test_stopping_always_reaches_exact_zero(void)
{
    section("Stopping leaves no residual velocity");

    int stuck = 0;
    int trials = 0;
    /* Sweep hold durations and directions: the residue only appears when the
     * final friction step lands the speed in a narrow window. */
    for (int walk = 175; walk <= 220; walk += 5)
    {
        for (int hold = 1; hold <= 90; hold++)
        {
            for (int dir = 0; dir < 4; dir++)
            {
                Sim s;
                sim_init(&s);
                int32_t maxSpeed = walkspeed_to_q8(walk);
                static const int32_t kDirs[4][2] = {
                    {PM_Q12_ONE, 0}, {0, PM_Q12_ONE}, {2896, 2896}, {-2896, 2896}};

                for (int i = 0; i < hold; i++)
                {
                    PM_Input in = input_none(maxSpeed);
                    in.wishXQ12 = kDirs[dir][0];
                    in.wishZQ12 = kDirs[dir][1];
                    sim_tick(&s, &in);
                }
                for (int i = 0; i < 200; i++)
                {
                    PM_Input in = input_none(maxSpeed);
                    sim_tick(&s, &in);
                }
                trials++;
                if (PM_HorizontalSpeed(&s.st) != 0)
                    stuck++;
            }
        }
    }
    printf("  %d stop trials, %d left a residual speed\n", trials, stuck);
    CHECK_EQ(stuck, 0, "every stop must reach exactly zero speed");
}

/**
 * @brief A ramp snap has to hold the grounded state open for a few ticks.
 *
 * Ramps are not physics geometry, so Nitro Engine never reports a collision and
 * PM_EndTick would clear onGround on the very same frame. Running downhill, the
 * snap only fires on the minority of frames where the player has sunk below the
 * surface, so without a grace window a jump input almost always lands on an
 * airborne frame and is swallowed.
 */
static void test_ramp_snap_keeps_the_player_grounded(void)
{
    section("Ramp snap grants coyote time");

    Sim s;
    sim_init(&s);
    int32_t maxSpeed = walkspeed_to_q8(220);

    /* Put the player in the air, far from the floor, then snap as a ramp does. */
    s.feetY = 40960;
    s.st.onGround = false;
    PM_ForceGrounded(&s.st);

    int groundedTicks = 0;
    for (int i = 0; i < PM_GROUND_GRACE_TICKS + 5; i++)
    {
        PM_Input in = input_none(maxSpeed);
        sim_tick(&s, &in);
        if (s.st.onGround)
            groundedTicks++;
    }
    printf("  grounded for %d ticks after one snap (grace is %d)\n",
           groundedTicks, PM_GROUND_GRACE_TICKS);
    CHECK(groundedTicks >= PM_GROUND_GRACE_TICKS,
          "one snap must keep the player grounded for the whole grace window");

    /* A jump must still work during the window, and must end it. */
    Sim j;
    sim_init(&j);
    j.feetY = 40960;
    j.st.onGround = false;
    PM_ForceGrounded(&j.st);
    PM_Input jump = input_none(maxSpeed);
    jump.jump = true;
    PM_Events ev = sim_tick(&j, &jump);
    CHECK(ev.jumped, "a jump during the grace window must fire");
    CHECK(!j.st.onGround, "and must leave the ground");
}

/** @brief Duck speed must start from the configured ideal, not a literal. */
static void test_duck_speed_comes_from_config(void)
{
    section("Duck speed is seeded from the config");

    PM_CSTunables cs;
    PM_Tunables t;
    PM_CSTunablesDefaults(&cs);
    cs.duckSpeedIdeal = 4.0f;
    PM_TunablesFromCS(&cs, &t);

    PM_State st;
    PM_StateResetTuned(&st, &t);
    CHECK_EQ(st.duckSpeedQ12, t.duckSpeedIdealQ12,
             "reset must use the configured duck speed");
}

static void test_config_parser(void)
{
    section("movement.cfg parsing");

    PM_CSTunables cs;
    MovementCfgResult res;
    PM_CSTunablesDefaults(&cs);

    static const char kText[] =
        "# a comment\n"
        "\n"
        "friction = 4.0\n"
        "   accelerate=6.25   ; trailing comment\n"
        "AIRACCELERATE = 15\n"
        "nonsense_key = 3\n"
        "this line has no equals sign\n"
        "duck_modifier =\n";

    MovementCfg_ParseBuffer(kText, sizeof(kText) - 1, &cs, &res);

    CHECK_NEAR(cs.friction, 4.0, 0.0001, "friction from config");
    CHECK_NEAR(cs.accelerate, 6.25, 0.0001, "accelerate, untrimmed and commented");
    CHECK_NEAR(cs.airAccelerate, 15.0, 0.0001, "key matching is case-insensitive");
    CHECK_NEAR(cs.duckModifier, 0.34, 0.0001, "empty value leaves the default");
    CHECK_EQ(res.applied, 3, "keys applied");
    CHECK_EQ(res.unknown, 1, "unknown keys counted, not fatal");
    CHECK_EQ(res.malformed, 2, "malformed lines counted, not fatal");

    /* Negative and exponent forms should survive the round trip. */
    PM_CSTunablesDefaults(&cs);
    static const char kMore[] = "gravity=8e2\nbhop_factor = 1.25\n";
    MovementCfg_ParseBuffer(kMore, sizeof(kMore) - 1, &cs, &res);
    CHECK_NEAR(cs.gravity, 800.0, 0.0001, "exponent notation");
    CHECK_NEAR(cs.bhopFactor, 1.25, 0.0001, "bhop factor from config");
    CHECK_EQ(res.malformed, 0, "no malformed lines here");
}

/**
 * @brief The shipped movement.cfg must reproduce the compiled-in defaults
 *        exactly, or the file and the fallback disagree about what "CS:GO
 *        default" means -- and only one of them would be in effect.
 */
static void test_shipped_config_matches_defaults(const char *path)
{
    section("shipped movement.cfg agrees with the compiled defaults");

    PM_CSTunables fromFile;
    PM_CSTunables defaults;
    MovementCfgResult res;

    PM_CSTunablesDefaults(&fromFile);
    PM_CSTunablesDefaults(&defaults);

    if (!MovementCfg_LoadPath(path, &fromFile, &res))
    {
        printf("  SKIP  could not read %s\n", path);
        return;
    }
    printf("  read %s: %d applied, %d unknown, %d malformed, truncated=%d\n",
           path, res.applied, res.unknown, res.malformed, (int)res.truncated);

    CHECK_EQ(res.unknown, 0, "shipped config has no unknown keys");
    CHECK_EQ(res.malformed, 0, "shipped config has no malformed lines");
    CHECK(!res.truncated, "shipped config fits in the read buffer");
    /* Every tunable must be present, or a key could silently drift from the
     * compiled default without anyone noticing. */
    CHECK_EQ(res.applied, 16, "shipped config sets every tunable");

    PM_Tunables a;
    PM_Tunables b;
    PM_TunablesFromCS(&fromFile, &a);
    PM_TunablesFromCS(&defaults, &b);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0,
          "shipped config converts to the same tunables as the defaults");
}

/**
 * @brief A hand-edited config must not be able to hang or overflow the tick.
 */
static void test_hostile_config(void)
{
    section("Hostile movement.cfg values are survivable");

    static const char *const kHostile[] = {
        "friction = -5.2",        /* would grow velocity without bound  */
        "stamina_range = 0",      /* divisor                            */
        "stamina_max = 0",        /* divisor in the percentage helper   */
        "duck_speed = 0",         /* rate; crouch would never finish    */
        "gravity = -800",         /* upward gravity                     */
        "accelerate = 1e30",      /* overflow bait                      */
        "jump_impulse = 999999",  /* overflow bait                      */
        "friction = 1000",        /* would reverse velocity every tick  */
    };

    for (int i = 0; i < (int)(sizeof(kHostile) / sizeof(kHostile[0])); i++)
    {
        PM_CSTunables cs;
        MovementCfgResult res;
        PM_CSTunablesDefaults(&cs);
        MovementCfg_ParseBuffer(kHostile[i], strlen(kHostile[i]), &cs, &res);

        Sim s;
        PM_TunablesFromCS(&cs, &s.t);
        PM_StateReset(&s.st);
        s.feetY = 0;
        s.posX = 0;
        s.posZ = 0;
        s.ceilingFeetY = INT32_MAX;
        s.st.onGround = true;

        int32_t maxSpeed = walkspeed_to_q8(220);
        for (int tick = 0; tick < 600; tick++)
        {
            PM_Input in = input_forward(maxSpeed);
            in.jump = (tick % 7) == 0;
            in.duck = (tick % 13) < 5;
            sim_tick(&s, &in);
        }

        int32_t speed = PM_HorizontalSpeed(&s.st);
        printf("  \"%s\" -> %d CS u/s, stamina %d%%\n", kHostile[i],
               PM_HorizontalSpeedCS(&s.st),
               s.t.staminaMaxQ12 > 0
                   ? (int)(((int64_t)s.st.staminaQ12 * 100) / s.t.staminaMaxQ12)
                   : 0);

        /* Nothing should reach a speed the fixed point cannot hold, and the
         * state must stay finite rather than saturating or going negative. */
        CHECK(speed >= 0 && speed < 4000000, "%s stayed in range", kHostile[i]);
        CHECK(s.st.duckAmountQ12 >= 0 && s.st.duckAmountQ12 <= PM_Q12_ONE,
              "duck amount stays in range");
        CHECK(s.st.staminaQ12 >= 0, "stamina stays non-negative");
    }
}

int main(int argc, char **argv)
{
    printf("CS:GO movement core -- host tests\n");
    printf("=================================\n");

    test_conversions();
    test_hull_geometry();
    test_sqrt();
    test_jump_apex();
    test_no_pogo();
    test_ground_top_speed();
    test_diagonal_is_not_faster();
    test_stopping_distance();
    test_stamina();
    test_bhop_does_not_build_speed();
    test_air_strafing_works();
    test_duck();
    test_unduck_blocked();
    test_crouch_spam_penalty();
    test_frozen();
    test_stopping_always_reaches_exact_zero();
    test_ramp_snap_keeps_the_player_grounded();
    test_duck_speed_comes_from_config();
    test_config_parser();
    test_hostile_config();
    if (argc > 1)
        test_shipped_config_matches_defaults(argv[1]);

    printf("\n=================================\n");
    if (g_failures == 0)
    {
        printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
