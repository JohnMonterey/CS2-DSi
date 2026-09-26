// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Procedural character animation: the skinned player rig and the poses that drive it.

#include "character_anim_core.h"

#include <stddef.h>
#include <string.h>

#define RIG_MAGIC 0x47525343u // 'CSRG'
#define RIG_VERSION 1
#define RIG_HEADER_BYTES 12
#define RIG_BONE_BYTES 12
#define GX_MTX_RESTORE 0x14

/*
 * Tuning. World units are the engine's (1 = 40 CS units); per-frame values assume the
 * 60 Hz logic tick. Angles use ANIM_DEG, so they read in degrees.
 */
#define SPEED_STILL 40    // Q12 world units per frame: below this a character is standing
#define SPEED_WALK 200    // 3 units/s, about a shift-walk: the full walk cycle
#define SPEED_RUN 380     // 5.7 units/s, a rifle at full speed: the full run cycle
#define TELEPORT_DIST (3 * ANIM_ONE) // moved further than this in one update: respawned

#define CYCLE_WALK 5325   // Q12 world units per stride pair (1.3) at a walk...
#define CYCLE_RUN 8602    // ...and 2.1 at a run

#define RATE_SPEED 1024   // Q12 per frame: how fast each blend follows its target
#define RATE_MOVE 614
#define RATE_HIPS 410
#define RATE_AIM 1024
#define RATE_YAW 1434     // facing; about 3 frames to close most of a turn

#define DEATH_FRAMES 40   // the fall
#define IDLE_PERIOD_BREATH 200
#define IDLE_PERIOD_SWAY 460
#define IDLE_PERIOD_GLANCE 610

static int32_t mulQ12(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 12);
}

static int32_t lerpQ12(int32_t a, int32_t b, int32_t t)
{
    return a + mulQ12(b - a, t);
}

int32_t AnimClamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// sin over a quarter turn, Q14, every 32 angle units (257 entries, 514 bytes). Between
// entries the curve is interpolated linearly, which is within 1/20000 of the true sine;
// a table keeps this to a lookup and one multiply, where a series needs 64-bit divisions
// the ARM9 has to do in software.
static const int16_t quarterSine[257] = {
    0, 101, 201, 302, 402, 503, 603, 704, 804, 904, 1005, 1105,
    1205, 1306, 1406, 1506, 1606, 1706, 1806, 1906, 2006, 2105, 2205, 2305,
    2404, 2503, 2603, 2702, 2801, 2900, 2999, 3098, 3196, 3295, 3393, 3492,
    3590, 3688, 3786, 3883, 3981, 4078, 4176, 4273, 4370, 4467, 4563, 4660,
    4756, 4852, 4948, 5044, 5139, 5235, 5330, 5425, 5520, 5614, 5708, 5803,
    5897, 5990, 6084, 6177, 6270, 6363, 6455, 6547, 6639, 6731, 6823, 6914,
    7005, 7096, 7186, 7276, 7366, 7456, 7545, 7635, 7723, 7812, 7900, 7988,
    8076, 8163, 8250, 8337, 8423, 8509, 8595, 8680, 8765, 8850, 8935, 9019,
    9102, 9186, 9269, 9352, 9434, 9516, 9598, 9679, 9760, 9841, 9921, 10001,
    10080, 10159, 10238, 10316, 10394, 10471, 10549, 10625, 10702, 10778, 10853, 10928,
    11003, 11077, 11151, 11224, 11297, 11370, 11442, 11514, 11585, 11656, 11727, 11797,
    11866, 11935, 12004, 12072, 12140, 12207, 12274, 12340, 12406, 12472, 12537, 12601,
    12665, 12729, 12792, 12854, 12916, 12978, 13039, 13100, 13160, 13219, 13279, 13337,
    13395, 13453, 13510, 13567, 13623, 13678, 13733, 13788, 13842, 13896, 13949, 14001,
    14053, 14104, 14155, 14206, 14256, 14305, 14354, 14402, 14449, 14497, 14543, 14589,
    14635, 14680, 14724, 14768, 14811, 14854, 14896, 14937, 14978, 15019, 15059, 15098,
    15137, 15175, 15213, 15250, 15286, 15322, 15357, 15392, 15426, 15460, 15493, 15525,
    15557, 15588, 15619, 15649, 15679, 15707, 15736, 15763, 15791, 15817, 15843, 15868,
    15893, 15917, 15941, 15964, 15986, 16008, 16029, 16049, 16069, 16088, 16107, 16125,
    16143, 16160, 16176, 16192, 16207, 16221, 16235, 16248, 16261, 16273, 16284, 16295,
    16305, 16315, 16324, 16332, 16340, 16347, 16353, 16359, 16364, 16369, 16373, 16376,
    16379, 16381, 16383, 16384, 16384,
};

int32_t AnimSin(int32_t angle)
{
    uint32_t a = (uint32_t)angle & (ANIM_TURN - 1);
    int quadrant = a >> 13; // ANIM_TURN / 4 = 8192
    int32_t q = a & 8191;
    if (quadrant & 1)
        q = 8192 - q;
    int i = q >> 5, f = q & 31;
    int32_t v = quarterSine[i];
    if (f)
        v += ((quarterSine[i + 1] - v) * f) >> 5;
    int32_t r = (v + 2) >> 2; // Q14 -> Q12, rounded
    return (quadrant & 2) ? -r : r;
}

int32_t AnimCos(int32_t angle)
{
    return AnimSin(angle + ANIM_TURN / 4);
}

/*
 * atan on [0, 1] as pi/4 x - x (x - 1)(0.2447 + 0.0663 x), worst error about 0.09 degrees,
 * spread over the octants.
 */
int32_t AnimAtan2(int32_t y, int32_t x)
{
    if (x == 0 && y == 0)
        return 0;
    uint32_t ax = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
    uint32_t ay = y < 0 ? 0u - (uint32_t)y : (uint32_t)y;
    bool swap = ay > ax;
    uint32_t num = swap ? ax : ay, den = swap ? ay : ax;
    // Only the ratio matters: scale both down until num << 12 fits in 32 bits.
    while (den >= (1u << 19))
    {
        num >>= 1;
        den >>= 1;
    }
    int32_t t = den ? (int32_t)((num << 12) / den) : 0; // Q12, 0..4096
    // pi/4 is ANIM_TURN / 8 = 4096, so pi/4 x is t itself; 0.2447 and 0.0663 rad are
    // 1276 and 346 in ANIM_TURN units.
    int32_t hump = (int32_t)(((int64_t)t * (ANIM_ONE - t)) >> 12);
    int32_t a = t + (int32_t)(((int64_t)hump * (1276 + ((346 * t) >> 12))) >> 12);
    if (swap)
        a = ANIM_TURN / 4 - a;
    if (x < 0)
        a = ANIM_TURN / 2 - a;
    if (y < 0)
        a = -a;
    return a;
}

int32_t AnimAngleDelta(int32_t from, int32_t to)
{
    int32_t d = (to - from) & (ANIM_TURN - 1);
    return d > ANIM_TURN / 2 ? d - ANIM_TURN : d;
}

int32_t AnimApproach(int32_t current, int32_t target, int32_t rate)
{
    int32_t diff = target - current;
    int32_t step = (int32_t)(((int64_t)diff * rate) >> 12);
    if (step == 0 && diff != 0)
        step = diff > 0 ? 1 : -1; // always arrive
    return current + step;
}

int32_t AnimEaseInOut(int32_t t)
{
    t = AnimClamp(t, 0, ANIM_ONE);
    return mulQ12(mulQ12(t, t), 3 * ANIM_ONE - 2 * t);
}

int32_t AnimEaseOut(int32_t t)
{
    int32_t u = ANIM_ONE - AnimClamp(t, 0, ANIM_ONE);
    return ANIM_ONE - mulQ12(mulQ12(u, u), u);
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit)
    {
        if (v >= r + bit)
        {
            v -= r + bit;
            r = (r >> 1) + bit;
        }
        else
            r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

// ----------------------------------------------------------------------------------------
// Rig
// ----------------------------------------------------------------------------------------

static uint32_t read16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t read32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Parameter words of each geometry command (GBATEK), -1 for bytes that are not commands.
static int gxParameterCount(uint32_t cmd)
{
    switch (cmd)
    {
    case 0x00: case 0x11: case 0x15: case 0x41:
        return 0;
    case 0x10: case 0x12: case 0x13: case 0x14:
    case 0x20: case 0x21: case 0x22: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
    case 0x29: case 0x2A: case 0x2B: case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x40: case 0x50: case 0x60: case 0x72:
        return 1;
    case 0x23: case 0x71:
        return 2;
    case 0x1B: case 0x1C: case 0x70:
        return 3;
    case 0x1A:
        return 9;
    case 0x17: case 0x19:
        return 12;
    case 0x16: case 0x18:
        return 16;
    case 0x34:
        return 32;
    default:
        return -1;
    }
}

// The hierarchy the pose code is written for; the file has to agree with it.
static const int8_t expectedParents[RIG_BONE_COUNT] = {
    -1, RIG_ROOT, RIG_PELVIS, RIG_CHEST, RIG_CHEST, RIG_PELVIS, RIG_THIGH_L, RIG_PELVIS, RIG_THIGH_R,
};

bool CharacterRig_Parse(CharacterRig *rig, const uint8_t *data, uint32_t size)
{
    memset(rig, 0, sizeof(*rig));
    if (data == NULL || ((uintptr_t)data & 3) != 0 || size < RIG_HEADER_BYTES)
        return false;
    if (read32(data) != RIG_MAGIC || read16(data + 4) != RIG_VERSION ||
        read16(data + 6) != RIG_BONE_COUNT || read16(data + 8) != RIG_FIRST_SLOT)
        return false;

    uint32_t listOffset = RIG_HEADER_BYTES + RIG_BONE_COUNT * RIG_BONE_BYTES;
    if (size < listOffset + 4)
        return false;

    for (int b = 0; b < RIG_BONE_COUNT; b++)
    {
        const uint8_t *p = data + RIG_HEADER_BYTES + b * RIG_BONE_BYTES;
        RigBoneInfo *bone = &rig->bones[b];
        bone->parent = (int8_t)p[0];
        bone->slot = p[1];
        bone->restPitch = (int16_t)read16(p + 2);
        bone->length = (int16_t)read16(p + 4);
        for (int k = 0; k < 3; k++)
            bone->pivot[k] = (int16_t)read16(p + 6 + 2 * k);
        if (bone->parent != expectedParents[b])
            return false;
        // Stack slots 0..30 exist; 31 would set the overflow flag.
        if (bone->slot != RIG_NO_SLOT && RIG_FIRST_SLOT + bone->slot > 30)
            return false;
    }
    if (rig->bones[RIG_ROOT].slot != RIG_NO_SLOT)
        return false;

    const uint32_t *list = (const uint32_t *)(const void *)(data + listOffset);
    // Checked before adding the count word, so a count near 2^32 cannot wrap to a small one.
    if (list[0] == 0 || list[0] > (size - listOffset) / 4 - 1)
        return false;
    uint32_t words = list[0] + 1;

    // Walk the commands, which must all be real ones: an unknown byte would leave the walk
    // out of step with the hardware's. Every matrix restore must name a slot a bone fills.
    uint32_t i = 1;
    while (i < words)
    {
        uint32_t packed = list[i++];
        for (int k = 0; k < 4; k++)
        {
            uint32_t cmd = (packed >> (8 * k)) & 0xFF;
            int params = gxParameterCount(cmd);
            if (params < 0 || i + (uint32_t)params > words)
                return false;
            if (cmd == GX_MTX_RESTORE)
            {
                uint32_t slot = list[i] - RIG_FIRST_SLOT;
                bool found = false;
                for (int b = 0; b < RIG_BONE_COUNT; b++)
                    found |= rig->bones[b].slot != RIG_NO_SLOT && rig->bones[b].slot == slot;
                if (!found)
                    return false;
            }
            i += (uint32_t)params;
        }
    }

    rig->displayList = list;
    rig->displayListWords = words;

    // How far below the hips the mesh's own feet are: poses keep the lower foot there.
    for (int thigh = RIG_THIGH_L; thigh <= RIG_THIGH_R; thigh += 2)
    {
        const RigBoneInfo *upper = &rig->bones[thigh], *lower = &rig->bones[thigh + 1];
        int32_t reach = (int32_t)(((int64_t)upper->length * AnimCos(upper->restPitch) +
                                   (int64_t)lower->length * AnimCos(lower->restPitch)) >> 12);
        if (reach > rig->restReach)
            rig->restReach = reach;
    }
    return true;
}

// ----------------------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------------------

void CharacterAnim_Reset(CharacterAnimState *state, const CharacterAnimInput *input, uint32_t seed)
{
    memset(state, 0, sizeof(*state));
    state->lastX = input->x;
    state->lastZ = input->z;
    state->renderYaw = input->yaw & (ANIM_TURN - 1);
    state->aimPitch = input->aimPitch;
    state->deathFrames = input->dead ? DEATH_FRAMES : -1; // already down: no fall to replay
    state->seed = seed;
    state->time = seed * 97;
}

void CharacterAnim_Update(CharacterAnimState *state, const CharacterAnimInput *input)
{
    int32_t frames = AnimClamp(input->frames, 1, 8);
    int32_t dx = input->x - state->lastX;
    int32_t dz = input->z - state->lastZ;
    state->lastX = input->x;
    state->lastZ = input->z;

    int32_t dist = (int32_t)isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dz * dz));
    // Back from the dead is a respawn wherever it lands; otherwise a long jump is one.
    bool respawned = state->deathFrames >= 0 && !input->dead;
    if (dist > TELEPORT_DIST || respawned)
    {
        // Start the gait over, facing the new way, rather than stride or turn into it.
        dist = 0;
        dx = dz = 0;
        state->speed = 0;
        state->moveBlend = 0;
        state->runBlend = 0;
        state->hipYaw = 0;
        state->backward = 0;
        state->renderYaw = input->yaw & (ANIM_TURN - 1);
    }
    if (input->dead)
    {
        dist = 0; // a body does not walk, even if its model is moved under it
        dx = dz = 0;
    }

    int32_t instant = dist / frames;
    int32_t hipTarget = 0;
    if (dist > 0)
    {
        // The model faces -z rotated by yaw: forward is (-sin yaw, -cos yaw). Heading is the
        // yaw that would face the motion, relative to where the character looks.
        int32_t heading = AnimAtan2(-dx, -dz);
        int32_t relative = AnimAngleDelta(state->renderYaw, heading);
        int32_t away = relative < 0 ? -relative : relative;
        // Backpedalling runs the legs in reverse. The band between 90 and 110 degrees keeps
        // a diagonal that wavers across the line from flipping the stride every frame.
        if (away > ANIM_DEG(110))
            state->backward = 1;
        else if (away < ANIM_DEG(90))
            state->backward = 0;
        if (state->backward)
            relative = AnimAngleDelta(0, relative + ANIM_TURN / 2);
        hipTarget = AnimClamp(relative, -ANIM_DEG(50), ANIM_DEG(50));
    }

    for (int f = 0; f < frames; f++)
    {
        state->speed = AnimApproach(state->speed, instant, RATE_SPEED);
        int32_t moveTarget = AnimClamp((state->speed - SPEED_STILL) * ANIM_ONE / (SPEED_WALK - SPEED_STILL), 0, ANIM_ONE);
        int32_t runTarget = AnimClamp((state->speed - SPEED_WALK) * ANIM_ONE / (SPEED_RUN - SPEED_WALK), 0, ANIM_ONE);
        if (input->dead)
            moveTarget = runTarget = 0;
        state->moveBlend = AnimApproach(state->moveBlend, moveTarget, RATE_MOVE);
        state->runBlend = AnimApproach(state->runBlend, runTarget, RATE_MOVE);
        state->hipYaw = AnimApproach(state->hipYaw, mulQ12(hipTarget, state->moveBlend), RATE_HIPS);
        state->aimPitch = AnimApproach(state->aimPitch, input->dead ? 0 : input->aimPitch, RATE_AIM);
        if (!input->dead)
        {
            int32_t turn = AnimAngleDelta(state->renderYaw, input->yaw);
            state->renderYaw = (state->renderYaw + AnimApproach(0, turn, RATE_YAW)) & (ANIM_TURN - 1);
        }
        state->time++;
    }

    state->recoil = input->dead ? 0 : AnimClamp(input->recoil, 0, ANIM_ONE);
    if (input->dead)
        state->deathFrames = state->deathFrames < 0 ? frames : AnimClamp(state->deathFrames + frames, 0, 1 << 20);
    else
        state->deathFrames = -1;

    // Stride length grows with speed, as a person's does; the phase follows the ground
    // actually covered, so feet keep pace with the body at any speed and frame rate.
    int32_t cycle = lerpQ12(CYCLE_WALK, CYCLE_RUN, state->runBlend);
    // dist is at most TELEPORT_DIST here, so dist * ANIM_TURN fits in 32 bits.
    int32_t advance = dist * ANIM_TURN / cycle;
    state->phase = (state->phase + (state->backward ? -advance : advance)) & (ANIM_TURN - 1);
}

// ----------------------------------------------------------------------------------------
// Poses
// ----------------------------------------------------------------------------------------

typedef struct
{
    int32_t thigh; // from straight down, + forward
    int32_t knee;  // shin relative to thigh, - bends back
} LegAngles;

static void setLeg(RigPose *pose, const CharacterRig *rig, int thighBone, LegAngles leg)
{
    const RigBoneInfo *thigh = &rig->bones[thighBone];
    const RigBoneInfo *shin = &rig->bones[thighBone + 1];
    pose->bones[thighBone].pitch = (int16_t)(leg.thigh - thigh->restPitch);
    // The shin's own rotation is relative to its rest angle, which already includes the
    // thigh's rest angle.
    pose->bones[thighBone + 1].pitch = (int16_t)(leg.knee - (shin->restPitch - thigh->restPitch));
}

// How far below the hip a leg reaches, Q12 model units.
static int32_t legReach(const CharacterRig *rig, int thighBone, LegAngles leg)
{
    const RigBoneInfo *thigh = &rig->bones[thighBone];
    const RigBoneInfo *shin = &rig->bones[thighBone + 1];
    return mulQ12(thigh->length, AnimCos(leg.thigh)) + mulQ12(shin->length, AnimCos(leg.thigh + leg.knee));
}

// Drops the body so the lower foot stays on the ground as the legs bend.
static int32_t groundOffset(const CharacterRig *rig, LegAngles left, LegAngles right)
{
    int32_t reachL = legReach(rig, RIG_THIGH_L, left);
    int32_t reachR = legReach(rig, RIG_THIGH_R, right);
    return (reachL > reachR ? reachL : reachR) - rig->restReach;
}

static int32_t wave(uint32_t time, uint32_t period, uint32_t offset)
{
    return AnimSin((int32_t)(((time + offset) % period) * ANIM_TURN / period));
}

void CharacterAnim_Pose(const CharacterAnimState *state, const CharacterRig *rig, RigPose *pose)
{
    memset(pose, 0, sizeof(*pose));

    int32_t move = state->moveBlend;
    int32_t still = ANIM_ONE - move;
    int32_t run = state->runBlend;
    int32_t s = AnimSin(state->phase);
    int32_t c = AnimCos(state->phase);
    uint32_t t = state->time;

    // Legs. Standing: a relaxed stagger. Moving: they swing opposite each other, each knee
    // folding most as its leg passes under the body going forward, further at a run. The
    // fold is ((1 + cos)/2)^2: smooth all the way round, no kink where it lets go.
    int32_t swing = mulQ12(lerpQ12(ANIM_DEG(24), ANIM_DEG(38), run), move);
    int32_t fold = mulQ12(lerpQ12(ANIM_DEG(40), ANIM_DEG(80), run), move);
    int32_t bend = mulQ12(ANIM_DEG(8), move);
    int32_t liftL = (ANIM_ONE + c) / 2, liftR = (ANIM_ONE - c) / 2;
    LegAngles left = {
        mulQ12(ANIM_DEG(7), still) + mulQ12(swing, s),
        -ANIM_DEG(4) - bend - mulQ12(fold, mulQ12(liftL, liftL)),
    };
    LegAngles right = {
        -mulQ12(ANIM_DEG(5), still) - mulQ12(swing, s),
        -ANIM_DEG(4) - bend - mulQ12(fold, mulQ12(liftR, liftR)),
    };

    setLeg(pose, rig, RIG_THIGH_L, left);
    setLeg(pose, rig, RIG_THIGH_R, right);

    // The body sits on its lower foot. Other players' crouches and jumps are not known here
    // (nothing sends them); their hull height already carries both.
    pose->offset[1] = groundOffset(rig, left, right);

    // Hips turn toward the direction of travel, and sway with the steps; the chest turns
    // back so the aim stays where the character looks.
    int32_t sway = mulQ12(ANIM_DEG(2.5), mulQ12(s, move)) + mulQ12(ANIM_DEG(1.5), mulQ12(wave(t, IDLE_PERIOD_SWAY, state->seed * 131), still));
    pose->bones[RIG_PELVIS].yaw = (int16_t)state->hipYaw;
    pose->bones[RIG_PELVIS].roll = (int16_t)sway;

    // Chest: leans into a run, breathes when still, counter-twists the hips.
    int32_t lean = mulQ12(ANIM_DEG(5) + mulQ12(ANIM_DEG(6), run), move);
    int32_t breath = mulQ12(ANIM_DEG(1.2), mulQ12(wave(t, IDLE_PERIOD_BREATH, state->seed * 71), still));
    int32_t bob = mulQ12(ANIM_DEG(1.5), mulQ12(AnimSin(state->phase * 2), move));
    pose->bones[RIG_CHEST].pitch = (int16_t)(-lean + breath + bob);
    pose->bones[RIG_CHEST].yaw = (int16_t)(-state->hipYaw - mulQ12(ANIM_DEG(5), mulQ12(s, move)));
    pose->bones[RIG_CHEST].roll = (int16_t)(-sway / 2);

    // Arms carry the pistol to the aim; the chest's lean is taken back out of them. A shot
    // kicks them up and the chest back a little.
    int32_t aim = AnimClamp(state->aimPitch, -ANIM_DEG(60), ANIM_DEG(60));
    int32_t kick = mulQ12(ANIM_DEG(9), state->recoil);
    pose->bones[RIG_ARMS].pitch = (int16_t)(aim + lean - breath - bob + kick);
    pose->bones[RIG_CHEST].pitch = (int16_t)(pose->bones[RIG_CHEST].pitch + kick / 4);

    // Head: follows half the aim, stays level against the lean, glances around when still.
    int32_t glance = mulQ12(ANIM_DEG(7), wave(t, IDLE_PERIOD_GLANCE, state->seed * 211)) +
                     mulQ12(ANIM_DEG(3), wave(t, IDLE_PERIOD_GLANCE / 3, state->seed * 53));
    pose->bones[RIG_HEAD].pitch = (int16_t)(aim / 2 + lean - breath);
    pose->bones[RIG_HEAD].yaw = (int16_t)mulQ12(glance, still);

    // Death: knees give, then the body falls back about its feet and settles.
    if (state->deathFrames >= 0)
    {
        int32_t d = AnimClamp(state->deathFrames * ANIM_ONE / DEATH_FRAMES, 0, ANIM_ONE);
        int32_t fall = mulQ12(d, d); // accelerating, like gravity
        int32_t buckle = AnimSin(mulQ12(d, ANIM_TURN / 2));
        LegAngles dl = {left.thigh + mulQ12(ANIM_DEG(30), buckle), left.knee - mulQ12(ANIM_DEG(55), buckle)};
        LegAngles dr = {right.thigh + mulQ12(ANIM_DEG(20), buckle), right.knee - mulQ12(ANIM_DEG(45), buckle)};
        setLeg(pose, rig, RIG_THIGH_L, dl);
        setLeg(pose, rig, RIG_THIGH_R, dr);
        int32_t side = (state->seed & 1) ? ANIM_DEG(14) : -ANIM_DEG(14);
        pose->bones[RIG_ROOT].pitch = (int16_t)mulQ12(ANIM_DEG(84), fall);
        pose->bones[RIG_ROOT].roll = (int16_t)mulQ12(side, fall);
        pose->bones[RIG_CHEST].pitch = (int16_t)(pose->bones[RIG_CHEST].pitch - mulQ12(ANIM_DEG(18), d));
        // Lying on its back, "forward" is up: fold the arms down across the body instead.
        pose->bones[RIG_ARMS].pitch = (int16_t)-mulQ12(ANIM_DEG(80), d);
        pose->bones[RIG_HEAD].pitch = (int16_t)mulQ12(ANIM_DEG(20), d);
        // Lying down, the back rests on the ground rather than sinking through it.
        pose->offset[1] = mulQ12(groundOffset(rig, dl, dr), ANIM_ONE - fall) + mulQ12(ANIM_ONE * 3 / 10, fall);
    }
}

void CharacterAnim_LowerWeapon(RigPose *pose, int32_t lowered)
{
    lowered = AnimClamp(lowered, 0, ANIM_ONE);
    pose->bones[RIG_ARMS].pitch = (int16_t)(pose->bones[RIG_ARMS].pitch - mulQ12(ANIM_DEG(38), lowered));
    pose->bones[RIG_HEAD].pitch = (int16_t)(pose->bones[RIG_HEAD].pitch - mulQ12(ANIM_DEG(6), lowered));
    pose->bones[RIG_CHEST].yaw = (int16_t)(pose->bones[RIG_CHEST].yaw + mulQ12(ANIM_DEG(10), lowered));
}
