// SPDX-License-Identifier: MIT
//
// Host tests for the character animation core.
//
// character_anim_core.c has no Nitro Engine or libnds dependency, so the rig file and
// every pose the console will draw can be checked here, without the console.
//
// Build and run:  make test-anim

#include "character_anim_core.h"
#include "remote_lerp.h"
#include "tombstone_core.h"

#include <math.h>
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

static const double TURN_RAD = 2.0 * 3.14159265358979323846;

static double toDegrees(int32_t angle)
{
    return angle * 360.0 / ANIM_TURN;
}

/* ---------------------------------------------------------------------- *
 * Fixed-point helpers
 * ---------------------------------------------------------------------- */

static void testTrig(void)
{
    section("fixed-point trigonometry");

    int worst = 0;
    for (int32_t a = -ANIM_TURN; a < 2 * ANIM_TURN; a++)
    {
        int expected = (int)lround(sin(a * TURN_RAD / ANIM_TURN) * ANIM_ONE);
        int err = abs(AnimSin(a) - expected);
        if (err > worst)
            worst = err;
    }
    CHECK(worst <= 1, "sin is off by %d Q12 steps somewhere (want <= 1)", worst);
    CHECK(AnimSin(0) == 0 && AnimSin(ANIM_TURN / 4) == ANIM_ONE && AnimSin(ANIM_TURN / 2) == 0 &&
              AnimSin(3 * ANIM_TURN / 4) == -ANIM_ONE,
          "sin at the quarter turns: %d %d %d %d", AnimSin(0), AnimSin(ANIM_TURN / 4),
          AnimSin(ANIM_TURN / 2), AnimSin(3 * ANIM_TURN / 4));
    CHECK(AnimCos(0) == ANIM_ONE && AnimCos(ANIM_TURN / 2) == -ANIM_ONE, "cos at 0 and a half turn");

    double worstAtan = 0;
    for (int y = -40; y <= 40; y++)
    {
        for (int x = -40; x <= 40; x++)
        {
            if (x == 0 && y == 0)
                continue;
            int32_t got = AnimAtan2(y * 997, x * 1013);
            double want = atan2(y * 997.0, x * 1013.0) * ANIM_TURN / TURN_RAD;
            double err = abs(AnimAngleDelta((int32_t)lround(want), got));
            if (err > worstAtan)
                worstAtan = err;
        }
    }
    CHECK(toDegrees((int32_t)worstAtan) <= 0.15, "atan2 worst error %.3f degrees (want <= 0.15)",
          toDegrees((int32_t)worstAtan));
    CHECK(AnimAtan2(0, 0) == 0, "atan2(0, 0) is 0");

    CHECK(AnimAngleDelta(ANIM_TURN - 100, 100) == 200, "delta across the wrap goes the short way up");
    CHECK(AnimAngleDelta(100, ANIM_TURN - 100) == -200, "delta across the wrap goes the short way down");
    CHECK(AnimAngleDelta(0, ANIM_TURN / 2) == ANIM_TURN / 2, "a half turn is +half");
    CHECK(AnimAngleDelta(5 * ANIM_TURN + 10, -ANIM_TURN + 20) == 10, "delta ignores whole turns");

    int32_t v = 0;
    bool overshot = false;
    int steps = 0;
    while (v != 1000 && steps < 1000)
    {
        v = AnimApproach(v, 1000, 300);
        overshot |= v > 1000;
        steps++;
    }
    CHECK(v == 1000 && !overshot, "approach arrives without overshooting (%d after %d steps)", v, steps);
    CHECK(AnimApproach(-5, -5, 4096) == -5, "approach at the target stays put");

    CHECK(AnimEaseInOut(0) == 0 && AnimEaseInOut(ANIM_ONE) == ANIM_ONE && AnimEaseInOut(ANIM_ONE / 2) == ANIM_ONE / 2,
          "ease in-out endpoints and midpoint");
    CHECK(AnimEaseOut(0) == 0 && AnimEaseOut(ANIM_ONE) == ANIM_ONE && AnimEaseOut(ANIM_ONE / 2) > ANIM_ONE / 2,
          "ease out endpoints, front-loaded");
    CHECK(AnimEaseOut(-50) == 0 && AnimEaseOut(ANIM_ONE + 50) == ANIM_ONE, "eases clamp their input");
}

/* ---------------------------------------------------------------------- *
 * Rig file
 * ---------------------------------------------------------------------- */

static uint8_t *g_rigData;
static uint32_t g_rigSize;
static CharacterRig g_rig;

static bool loadRig(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    // malloc's alignment covers the 4 bytes bin2o gives the file on the console. (Not
    // aligned_alloc: macOS refuses alignments below the size of a pointer.)
    g_rigData = malloc((size_t)size + 16);
    g_rigSize = (uint32_t)size;
    bool ok = g_rigData != NULL && fread(g_rigData, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}

static bool parseCopy(void (*damage)(uint8_t *copy, uint32_t *size))
{
    uint32_t size = g_rigSize;
    uint8_t *copy = malloc((size_t)size + 16);
    if (copy == NULL)
        return false;
    memcpy(copy, g_rigData, size);
    damage(copy, &size);
    CharacterRig rig;
    bool ok = CharacterRig_Parse(&rig, copy, size);
    free(copy);
    return ok;
}

static void noDamage(uint8_t *c, uint32_t *s) { (void)c; (void)s; }
static void badMagic(uint8_t *c, uint32_t *s) { (void)s; c[0] ^= 1; }
static void badVersion(uint8_t *c, uint32_t *s) { (void)s; c[4] = 9; }
static void badSlotBase(uint8_t *c, uint32_t *s) { (void)s; c[8] = 25; }
static void truncated(uint8_t *c, uint32_t *s) { (void)c; *s -= 8; }
static void headerOnly(uint8_t *c, uint32_t *s) { (void)c; *s = 40; }
static void wrongParent(uint8_t *c, uint32_t *s) { (void)s; c[12 + 12 * RIG_HEAD] = RIG_PELVIS; }
static void slotOverflow(uint8_t *c, uint32_t *s) { (void)s; c[12 + 12 * RIG_SHIN_R + 1] = 9; }

// The display list starts after the header and the bone table: a word count, then the first
// packed word, BEGIN_VTXS MTX_RESTORE TEXCOORD NORMAL, then their parameters.
#define LIST_OFFSET (12 + 12 * RIG_BONE_COUNT)
static uint32_t *listWords(uint8_t *c) { return (uint32_t *)(void *)(c + LIST_OFFSET); }
static void hugeCount(uint8_t *c, uint32_t *s) { (void)s; listWords(c)[0] = 0xFFFFFFFFu; }
static void restoreRootSlot(uint8_t *c, uint32_t *s) { (void)s; listWords(c)[3] = RIG_FIRST_SLOT + RIG_NO_SLOT; }
static void restoreUnfilledSlot(uint8_t *c, uint32_t *s) { (void)s; listWords(c)[3] = RIG_FIRST_SLOT - 1; }
static void unknownCommand(uint8_t *c, uint32_t *s) { (void)s; listWords(c)[1] = (listWords(c)[1] & ~0xFFu) | 0x42; }

static void testRigFile(void)
{
    section("rig file");

    CHECK(CharacterRig_Parse(&g_rig, g_rigData, g_rigSize), "data/player_rig.bin parses");
    CHECK(g_rig.bones[RIG_ROOT].parent == -1 && g_rig.bones[RIG_ROOT].slot == RIG_NO_SLOT,
          "the root has no parent and no vertices");
    for (int b = 1; b < RIG_BONE_COUNT; b++)
    {
        CHECK(g_rig.bones[b].parent >= 0 && g_rig.bones[b].parent < b, "bone %d's parent comes first", b);
        CHECK(g_rig.bones[b].slot != RIG_NO_SLOT && RIG_FIRST_SLOT + g_rig.bones[b].slot <= 30,
              "bone %d owns a stack slot below 31", b);
        for (int o = 1; o < b; o++)
            CHECK(g_rig.bones[b].slot != g_rig.bones[o].slot, "bones %d and %d share a slot", o, b);
    }
    // The legs: forward and back in the rest pose, and long enough to be legs.
    CHECK(g_rig.bones[RIG_THIGH_L].restPitch > ANIM_DEG(15), "the left thigh starts forward (%.1f deg)",
          toDegrees(g_rig.bones[RIG_THIGH_L].restPitch));
    CHECK(g_rig.bones[RIG_THIGH_R].restPitch < 0, "the right thigh starts back (%.1f deg)",
          toDegrees(g_rig.bones[RIG_THIGH_R].restPitch));
    for (int b = RIG_THIGH_L; b <= RIG_SHIN_R; b++)
        CHECK(g_rig.bones[b].length > ANIM_ONE / 4 && g_rig.bones[b].length < ANIM_ONE,
              "leg bone %d is a plausible length (%d)", b, g_rig.bones[b].length);
    // Each foot's contact with the ground is found from its shin's own vertices.
    CHECK(g_rig.feet[0].count >= 4 && g_rig.feet[1].count >= 4, "both shins' outlines were collected (%d and %d points)",
          g_rig.feet[0].count, g_rig.feet[1].count);
    CHECK(g_rig.displayListWords > 1000 && g_rigSize == LIST_OFFSET + 4 * g_rig.displayListWords,
          "the display list is the rest of the file (%u bytes, list of %u words)", g_rigSize, g_rig.displayListWords);
    CHECK(listWords(g_rigData)[1] == 0x21221440u,
          "the list opens with BEGIN_VTXS, MTX_RESTORE, TEXCOORD, NORMAL (the damage cases rely on it)");

    CHECK(parseCopy(noDamage), "an intact copy parses");
    CHECK(!parseCopy(badMagic), "a bad magic is refused");
    CHECK(!parseCopy(badVersion), "an unknown version is refused");
    CHECK(!parseCopy(badSlotBase), "a different stack slot base is refused");
    CHECK(!parseCopy(truncated), "a truncated display list is refused");
    CHECK(!parseCopy(headerOnly), "a file without its display list is refused");
    CHECK(!parseCopy(wrongParent), "a rearranged hierarchy is refused");
    CHECK(!parseCopy(slotOverflow), "a slot past the top of the stack is refused");
    CHECK(!parseCopy(hugeCount), "a word count that would wrap is refused");
    CHECK(!parseCopy(restoreRootSlot), "a restore of the root's no-slot marker is refused");
    CHECK(!parseCopy(restoreUnfilledSlot), "a restore of a slot no bone fills is refused");
    CHECK(!parseCopy(unknownCommand), "a byte that is not a geometry command is refused");
    CharacterRig rig;
    CHECK(!CharacterRig_Parse(&rig, g_rigData + 2, g_rigSize - 2), "unaligned data is refused");
    CHECK(!CharacterRig_Parse(&rig, NULL, 100), "NULL is refused");
}

/* ---------------------------------------------------------------------- *
 * Gait
 * ---------------------------------------------------------------------- */

static CharacterAnimInput standing(int32_t x, int32_t z, int32_t yaw)
{
    CharacterAnimInput in;
    memset(&in, 0, sizeof(in));
    in.x = x;
    in.z = z;
    in.yaw = yaw;
    in.frames = 1;
    return in;
}

// Walks straight along the facing at `speed` (Q12 units per frame) for `frames` frames.
static void walk(CharacterAnimState *st, CharacterAnimInput *in, int32_t speed, int frames, int32_t heading)
{
    for (int f = 0; f < frames; f++)
    {
        in->x += (int32_t)lround(-sin(heading * TURN_RAD / ANIM_TURN) * speed);
        in->z += (int32_t)lround(-cos(heading * TURN_RAD / ANIM_TURN) * speed);
        CharacterAnim_Update(st, in);
    }
}

/* The drawn mesh, skinned the way character_anim.c sets up the geometry engine: the pose's
 * offset, then each bone's matrix is its parent's turned about the bone's pivot (roll, then
 * pitch, then yaw, as glRotate*i builds them), and every vertex in the display list is sent
 * after its bone's matrix is restored. Independent of the pose code's own reasoning about
 * the legs, so it can catch that reasoning being wrong. */
typedef struct
{
    double m[3][4];
} Affine;

static Affine affMul(const Affine *a, const Affine *b)
{
    Affine r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
        {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
            if (j == 3)
                r.m[i][j] += a->m[i][3];
        }
    return r;
}

static Affine affTranslate(double x, double y, double z)
{
    Affine r = {{{1, 0, 0, x}, {0, 1, 0, y}, {0, 0, 1, z}}};
    return r;
}

static Affine affRotate(int axis, int32_t angle)
{
    double c = cos(angle * TURN_RAD / ANIM_TURN), s = sin(angle * TURN_RAD / ANIM_TURN);
    Affine r = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    int a = (axis + 1) % 3, b = (axis + 2) % 3; // x: (y, z); y: (z, x); z: (x, y)
    r.m[a][a] = c;
    r.m[a][b] = -s;
    r.m[b][a] = s;
    r.m[b][b] = c;
    return r;
}

// The lowest point of the drawn mesh for this pose, model units.
static double lowestVertex(const RigPose *pose)
{
    Affine bones[RIG_BONE_COUNT];
    for (int b = 0; b < RIG_BONE_COUNT; b++)
    {
        const RigBoneInfo *info = &g_rig.bones[b];
        const RigBonePose *p = &pose->bones[b];
        double px = info->pivot[0] / 4096.0, py = info->pivot[1] / 4096.0, pz = info->pivot[2] / 4096.0;
        Affine m = b == 0 ? affTranslate(pose->offset[0] / 4096.0, pose->offset[1] / 4096.0, pose->offset[2] / 4096.0)
                          : bones[info->parent];
        Affine t = affTranslate(px, py, pz), r;
        m = affMul(&m, &t);
        r = affRotate(1, p->yaw);
        m = affMul(&m, &r);
        r = affRotate(0, p->pitch);
        m = affMul(&m, &r);
        r = affRotate(2, p->roll);
        m = affMul(&m, &r);
        t = affTranslate(-px, -py, -pz);
        bones[b] = affMul(&m, &t);
    }

    const uint32_t *list = g_rig.displayList;
    double lowest = 1e9;
    int bone = -1;
    uint32_t i = 1;
    while (i < g_rig.displayListWords)
    {
        uint32_t packed = list[i++];
        for (int k = 0; k < 4; k++)
        {
            uint32_t cmd = (packed >> (8 * k)) & 0xFF;
            if (cmd == 0x14) // MTX_RESTORE
                for (int b = 0; b < RIG_BONE_COUNT; b++)
                    if (g_rig.bones[b].slot == list[i] - RIG_FIRST_SLOT)
                        bone = b;
            if (cmd == 0x23 && bone >= 0) // VTX_16
            {
                double x = (int16_t)(list[i] & 0xFFFF) / 4096.0, y = (int16_t)(list[i] >> 16) / 4096.0,
                       z = (int16_t)(list[i + 1] & 0xFFFF) / 4096.0;
                const double *row = bones[bone].m[1];
                double wy = row[0] * x + row[1] * y + row[2] * z + row[3];
                if (wy < lowest)
                    lowest = wy;
            }
            // Parameter counts of the commands the rig uses (checked by the parse tests).
            i += cmd == 0x23 ? 2 : (cmd == 0x00 || cmd == 0x41 ? 0 : 1);
        }
    }
    return lowest;
}

static void testGait(void)
{
    section("gait");

    CharacterAnimState st;
    CharacterAnimInput in = standing(0, 0, 0);
    CharacterAnim_Reset(&st, &in, 1);
    for (int f = 0; f < 120; f++)
        CharacterAnim_Update(&st, &in);
    CHECK(st.moveBlend == 0 && st.speed == 0, "standing still stays still (move %d speed %d)", st.moveBlend, st.speed);

    // Bots move 0.087 units per tick.
    walk(&st, &in, 356, 120, 0);
    CHECK(st.moveBlend == ANIM_ONE, "a bot's pace is a full walk (move %d)", st.moveBlend);
    CHECK(st.runBlend > ANIM_ONE / 2, "and most of a run (run %d)", st.runBlend);
    CHECK(abs(st.hipYaw) < ANIM_DEG(1), "walking forward keeps the hips square (%.2f deg)", toDegrees(st.hipYaw));

    // The phase follows the distance: one stride pair per cycle length.
    int32_t before = st.phase;
    walk(&st, &in, 356, 60, 0);
    double cycles = AnimAngleDelta(before, st.phase) / (double)ANIM_TURN;
    double expected = 356.0 * 60 / (5325 + (8602 - 5325) * st.runBlend / 4096.0);
    double wrapped = expected - floor(expected + 0.5);
    CHECK_NEAR(cycles, wrapped, 0.02, "phase advance per distance (fraction of a cycle)");

    // Backpedalling runs the cycle in reverse and keeps the hips square.
    before = st.phase;
    walk(&st, &in, 200, 3, ANIM_TURN / 2);
    CHECK(AnimAngleDelta(before, st.phase) < 0, "walking backwards turns the phase back");
    walk(&st, &in, 200, 60, ANIM_TURN / 2);
    CHECK(abs(st.hipYaw) < ANIM_DEG(1), "backpedalling keeps the hips square (%.2f deg)", toDegrees(st.hipYaw));

    // A diagonal just past square does not flip the gait either way: it keeps whichever
    // direction it had.
    before = st.phase;
    walk(&st, &in, 200, 3, ANIM_DEG(100));
    CHECK(AnimAngleDelta(before, st.phase) < 0, "after backpedalling, 100 degrees off still backpedals");
    walk(&st, &in, 200, 60, 0);
    before = st.phase;
    walk(&st, &in, 200, 3, ANIM_DEG(100));
    CHECK(AnimAngleDelta(before, st.phase) > 0, "after walking forward, 100 degrees off still walks forward");

    // Strafing left turns the hips left, within their limit.
    walk(&st, &in, 300, 90, ANIM_TURN / 4);
    CHECK(st.hipYaw > ANIM_DEG(40) && st.hipYaw <= ANIM_DEG(50), "strafing left turns the hips left (%.1f deg)",
          toDegrees(st.hipYaw));

    // Stopping settles back to standing.
    for (int f = 0; f < 90; f++)
        CharacterAnim_Update(&st, &in);
    CHECK(st.moveBlend == 0 && abs(st.hipYaw) < ANIM_DEG(1), "stopping settles (move %d, hips %.1f deg)",
          st.moveBlend, toDegrees(st.hipYaw));

    // A respawn across the map does not spin the legs.
    before = st.phase;
    in.x += 40 * ANIM_ONE;
    CharacterAnim_Update(&st, &in);
    CHECK(st.phase == before && st.speed == 0, "a teleport resets rather than strides");
}

static void testFrameRate(void)
{
    section("frame-rate independence");

    // The same motion drawn at 60 Hz and at 30 Hz (dual-screen mode) ends in the same state.
    CharacterAnimState a, b;
    CharacterAnimInput ia = standing(0, 0, 0), ib = standing(0, 0, 0);
    CharacterAnim_Reset(&a, &ia, 2);
    CharacterAnim_Reset(&b, &ib, 2);
    for (int f = 0; f < 240; f++)
    {
        ia.z -= 300;
        ia.yaw = (int32_t)(ANIM_DEG(60) * sin(f / 15.0)); // turning, but never backpedalling
        CharacterAnim_Update(&a, &ia);
        if (f % 2 == 1)
        {
            ib.z = ia.z;
            ib.yaw = ia.yaw;
            ib.frames = 2;
            CharacterAnim_Update(&b, &ib);
        }
    }
    CHECK(abs(AnimAngleDelta(a.phase, b.phase)) < ANIM_TURN / 200, "phase agrees (%d vs %d)", a.phase, b.phase);
    CHECK(abs(a.moveBlend - b.moveBlend) < 80, "move blend agrees (%d vs %d)", a.moveBlend, b.moveBlend);
    CHECK(abs(AnimAngleDelta(a.renderYaw, b.renderYaw)) < ANIM_DEG(3), "facing agrees (%.1f vs %.1f deg)",
          toDegrees(a.renderYaw), toDegrees(b.renderYaw));
}

static void testFacing(void)
{
    section("facing");

    CharacterAnimState st;
    CharacterAnimInput in = standing(0, 0, ANIM_TURN - ANIM_DEG(10));
    CharacterAnim_Reset(&st, &in, 3);
    in.yaw = ANIM_DEG(10); // 20 degrees across the wrap
    bool wentLong = false;
    for (int f = 0; f < 30; f++)
    {
        CharacterAnim_Update(&st, &in);
        int32_t off = AnimAngleDelta(st.renderYaw, ANIM_DEG(10));
        wentLong |= off > ANIM_DEG(21) || off < 0;
    }
    CHECK(!wentLong, "turning across 0 takes the short way");
    CHECK(abs(AnimAngleDelta(st.renderYaw, ANIM_DEG(10))) <= 1, "and arrives (%.2f deg off)",
          toDegrees(AnimAngleDelta(st.renderYaw, ANIM_DEG(10))));

    // A bot that snaps 180 degrees turns over a few frames instead.
    in.yaw = ANIM_DEG(190);
    CharacterAnim_Update(&st, &in);
    int32_t first = abs(AnimAngleDelta(ANIM_DEG(10), st.renderYaw));
    CHECK(first > ANIM_DEG(20) && first < ANIM_DEG(120), "a snapped turn is spread out (first frame %.1f deg)",
          toDegrees(first));
    for (int f = 0; f < 20; f++)
        CharacterAnim_Update(&st, &in);
    CHECK(abs(AnimAngleDelta(st.renderYaw, ANIM_DEG(190))) < ANIM_DEG(1), "and completes within 20 frames");
}

static int32_t maxBoneJump(const RigPose *a, const RigPose *b)
{
    int32_t worst = 0;
    for (int i = 0; i < RIG_BONE_COUNT; i++)
    {
        int32_t d[3] = {a->bones[i].pitch - b->bones[i].pitch, a->bones[i].yaw - b->bones[i].yaw,
                        a->bones[i].roll - b->bones[i].roll};
        for (int k = 0; k < 3; k++)
            if (abs(d[k]) > worst)
                worst = abs(d[k]);
    }
    return worst;
}

static void testPoses(void)
{
    section("poses");

    CharacterAnimState st;
    CharacterAnimInput in = standing(0, 0, 0);
    CharacterAnim_Reset(&st, &in, 4);
    RigPose pose, prev;

    // The ground is where the mesh's soles are: the lowest point of the posed mesh belongs
    // there. The mesh stands mid-stride, so straightening the legs to stand raises the hips a
    // little.
    CharacterAnim_Pose(&st, &g_rig, &pose);
    RigPose rest;
    memset(&rest, 0, sizeof(rest));
    double ground = lowestVertex(&rest);
    CHECK(ground < -1.4 && ground > -1.5, "the mesh's soles are at %.3f", ground);
    CHECK(pose.offset[1] > 0 && pose.offset[1] < ANIM_ONE / 5, "standing raises the hips a little (%.3f model units)",
          pose.offset[1] / 4096.0);
    CHECK_NEAR(lowestVertex(&pose), ground, 0.005, "standing: the lower foot is on the ground");

    // Walking and running, cycle after cycle, speeding up and slowing down: the lower foot always
    // touches the ground, and nothing pops. Fast legs move a lot per frame, so a pop is
    // judged by the change in that motion (the second difference), not the motion itself.
    int32_t worstJump = 0, worstStep = 0;
    double worstGround = 0;
    RigPose older;
    CharacterAnim_Pose(&st, &g_rig, &prev);
    older = prev;
    for (int f = 0; f < 600; f++)
    {
        int32_t speed = f < 300 ? f * 2 : 600 - f;
        if (speed > 400)
            speed = 400;
        in.z -= speed;
        in.yaw = (int32_t)(ANIM_DEG(40) * sin(f / 25.0));
        CharacterAnim_Update(&st, &in);
        CharacterAnim_Pose(&st, &g_rig, &pose);
        int32_t step = maxBoneJump(&pose, &prev);
        if (step > worstStep)
            worstStep = step;
        RigPose velocityNow, velocityBefore;
        for (int b = 0; b < RIG_BONE_COUNT; b++)
        {
            velocityNow.bones[b].pitch = (int16_t)(pose.bones[b].pitch - prev.bones[b].pitch);
            velocityNow.bones[b].yaw = (int16_t)(pose.bones[b].yaw - prev.bones[b].yaw);
            velocityNow.bones[b].roll = (int16_t)(pose.bones[b].roll - prev.bones[b].roll);
            velocityBefore.bones[b].pitch = (int16_t)(prev.bones[b].pitch - older.bones[b].pitch);
            velocityBefore.bones[b].yaw = (int16_t)(prev.bones[b].yaw - older.bones[b].yaw);
            velocityBefore.bones[b].roll = (int16_t)(prev.bones[b].roll - older.bones[b].roll);
        }
        int32_t jump = f < 2 ? 0 : maxBoneJump(&velocityNow, &velocityBefore);
        if (jump > worstJump)
            worstJump = jump;
        older = prev;
        double err = fabs(lowestVertex(&pose) - ground);
        if (err > worstGround)
            worstGround = err;
        prev = pose;
    }
    CHECK(worstStep < ANIM_DEG(25), "no bone moves more than 25 degrees in a frame (worst %.2f)", toDegrees(worstStep));
    CHECK(worstJump < ANIM_DEG(8), "no bone's motion changes by more than 8 degrees a frame (worst %.2f)",
          toDegrees(worstJump));
    CHECK(worstGround < 0.005, "the planted foot stays on the ground (worst %.3f model units)", worstGround);

    // The mirror half of the cycle mirrors the legs.
    CharacterAnimState run;
    memset(&run, 0, sizeof(run));
    run.moveBlend = ANIM_ONE;
    run.runBlend = ANIM_ONE / 2;
    run.deathFrames = -1;
    run.phase = ANIM_TURN / 8;
    RigPose p1, p2;
    CharacterAnim_Pose(&run, &g_rig, &p1);
    run.phase += ANIM_TURN / 2;
    CharacterAnim_Pose(&run, &g_rig, &p2);
    int32_t l1 = g_rig.bones[RIG_THIGH_L].restPitch + p1.bones[RIG_THIGH_L].pitch;
    int32_t r2 = g_rig.bones[RIG_THIGH_R].restPitch + p2.bones[RIG_THIGH_R].pitch;
    CHECK(abs(l1 - r2) < ANIM_DEG(14), "half a cycle later the right leg is where the left was (%.1f vs %.1f deg)",
          toDegrees(l1), toDegrees(r2));

    // Death: the body falls back over about 40 frames and then stays down.
    in.dead = true;
    int32_t lastFall = -1;
    bool monotonic = true;
    for (int f = 0; f < 80; f++)
    {
        in.z -= 200; // the model may be moved under a body; it must not walk
        CharacterAnim_Update(&st, &in);
        CharacterAnim_Pose(&st, &g_rig, &pose);
        monotonic &= pose.bones[RIG_ROOT].pitch >= lastFall;
        lastFall = pose.bones[RIG_ROOT].pitch;
    }
    CHECK(monotonic, "the fall only goes one way");
    CHECK(lastFall > ANIM_DEG(75) && lastFall <= ANIM_DEG(90), "and ends lying down (%.1f deg)", toDegrees(lastFall));
    CHECK(st.moveBlend == 0, "a body does not walk");

    // Respawned close by (2 units, under the teleport distance) and facing elsewhere: standing
    // at once, not striding across or turning round from where the body lay.
    in.dead = false;
    in.x += 2 * ANIM_ONE;
    in.yaw = ANIM_DEG(200);
    int32_t phaseBefore = st.phase;
    CharacterAnim_Update(&st, &in);
    CharacterAnim_Pose(&st, &g_rig, &pose);
    CHECK(pose.bones[RIG_ROOT].pitch == 0 && st.deathFrames == -1, "a respawn stands up");
    CHECK(st.speed == 0 && st.moveBlend == 0 && st.phase == phaseBefore, "a nearby respawn does not stride");
    CHECK(st.renderYaw == (in.yaw & (ANIM_TURN - 1)), "a respawn faces its new way at once");

    // Lowering the pistol: all the way down is well below the aim, and the ramp is smooth.
    CharacterAnimState idle;
    CharacterAnimInput still = standing(0, 0, 0);
    CharacterAnim_Reset(&idle, &still, 5);
    RigPose aiming, low;
    CharacterAnim_Pose(&idle, &g_rig, &aiming);
    low = aiming;
    CharacterAnim_LowerWeapon(&low, ANIM_ONE);
    CHECK(aiming.bones[RIG_ARMS].pitch - low.bones[RIG_ARMS].pitch > ANIM_DEG(30), "lowering drops the pistol");
    worstJump = 0;
    for (int f = 1; f <= 60; f++)
    {
        RigPose a = aiming, b = aiming;
        CharacterAnim_LowerWeapon(&a, (f - 1) * ANIM_ONE / 60);
        CharacterAnim_LowerWeapon(&b, f * ANIM_ONE / 60);
        int32_t jump = maxBoneJump(&a, &b);
        if (jump > worstJump)
            worstJump = jump;
    }
    CHECK(worstJump < ANIM_DEG(1), "raising over a second moves under a degree a frame (worst %.2f)", toDegrees(worstJump));

    // Idle: breathing and glancing never pop.
    worstJump = 0;
    CharacterAnim_Pose(&idle, &g_rig, &prev);
    for (int f = 0; f < 2000; f++)
    {
        CharacterAnim_Update(&idle, &still);
        CharacterAnim_Pose(&idle, &g_rig, &pose);
        int32_t jump = maxBoneJump(&pose, &prev);
        if (jump > worstJump)
            worstJump = jump;
        prev = pose;
    }
    CHECK(worstJump < ANIM_DEG(1), "the idle is smooth (worst %.2f deg per frame)", toDegrees(worstJump));
}

/* ---------------------------------------------------------------------- *
 * Online players between snapshots
 * ---------------------------------------------------------------------- */

static void testRemoteLerp(void)
{
    section("online players between snapshots");

    // A player running straight at 5 units/s, relayed every 6 frames (10 Hz).
    RemoteLerp lerp;
    memset(&lerp, 0, sizeof(lerp));
    float drawn[3] = {0, 0, 0};
    float snapshot[3] = {0, 0, 0};
    RemoteLerp_Snap(&lerp, snapshot, 0);
    const float perFrame = 5.0f / 60.0f;
    float lastX = 0, minStep = 1e9f, maxStep = 0, worstJump = 0;
    bool backwards = false, overshot = false;
    for (int frame = 1; frame <= 240; frame++)
    {
        if (frame % 6 == 0)
        {
            snapshot[0] = frame * perFrame;
            float before[3];
            RemoteLerp_Sample(&lerp, frame, before);
            RemoteLerp_Push(&lerp, drawn, snapshot, frame);
            float after[3];
            RemoteLerp_Sample(&lerp, frame, after);
            // Where the old segment and the new one put the player this frame.
            float jump = fabsf(after[0] - before[0]);
            if (frame > 30 && jump > worstJump)
                worstJump = jump;
        }
        RemoteLerp_Sample(&lerp, frame, drawn);
        float step = drawn[0] - lastX;
        backwards |= step < -1e-6f;
        overshot |= drawn[0] > snapshot[0] + 1e-4f;
        if (frame > 30)
        {
            if (step < minStep)
                minStep = step;
            if (step > maxStep)
                maxStep = step;
        }
        lastX = drawn[0];
    }
    CHECK(worstJump < perFrame / 4, "a new snapshot never jumps the player (worst %.4f units)", worstJump);
    CHECK(!backwards && !overshot, "steady running never goes backwards or past the snapshot");
    CHECK(minStep > perFrame * 0.8f && maxStep < perFrame * 1.2f,
          "steady running is drawn at a steady pace (%.4f..%.4f units a frame, want about %.4f)", minStep,
          maxStep, perFrame);
    // Just after a snapshot the player is drawn one relay interval (6 frames) plus the frame of
    // slack behind it.
    CHECK(snapshot[0] - drawn[0] < perFrame * 8, "and about one snapshot behind (%.3f units)", snapshot[0] - drawn[0]);

    // The relay's own rhythm: snapshots 6 and 8 frames apart in turn. The player must not
    // stop and start between them.
    memset(&lerp, 0, sizeof(lerp));
    snapshot[0] = drawn[0] = 0;
    RemoteLerp_Snap(&lerp, snapshot, 0);
    lastX = 0;
    int stalls = 0;
    for (int frame = 1, next = 6, k = 0; frame <= 300; frame++)
    {
        if (frame == next)
        {
            snapshot[0] = frame * perFrame;
            RemoteLerp_Push(&lerp, drawn, snapshot, frame);
            next += (k++ & 1) ? 8 : 6;
        }
        RemoteLerp_Sample(&lerp, frame, drawn);
        if (frame > 30 && drawn[0] - lastX < perFrame * 0.25f)
            stalls++;
        lastX = drawn[0];
    }
    CHECK(stalls == 0, "snapshots 6 and 8 frames apart in turn never stall the player (%d stalls)", stalls);

    // Irregular packets (4 to 9 frames apart): still never backwards, never past the target.
    memset(&lerp, 0, sizeof(lerp));
    snapshot[0] = drawn[0] = 0;
    RemoteLerp_Snap(&lerp, snapshot, 0);
    backwards = overshot = false;
    int next = 5, gapIndex = 0;
    static const int gaps[] = {4, 9, 6, 5, 8, 6, 4, 7};
    lastX = 0;
    for (int frame = 1; frame <= 300; frame++)
    {
        if (frame == next)
        {
            snapshot[0] = frame * perFrame;
            RemoteLerp_Push(&lerp, drawn, snapshot, frame);
            next += gaps[gapIndex++ % 8];
        }
        RemoteLerp_Sample(&lerp, frame, drawn);
        backwards |= drawn[0] < lastX - 1e-6f;
        overshot |= drawn[0] > snapshot[0] + 1e-4f;
        lastX = drawn[0];
    }
    CHECK(!backwards && !overshot, "irregular snapshots never go backwards or past the target");

    // The frame counter is reset at the start of a match: a segment from before counts as done.
    RemoteLerp_Push(&lerp, drawn, (float[3]){50, 1, 2}, 1000);
    float out[3];
    RemoteLerp_Sample(&lerp, 3, out);
    CHECK(out[0] == 50 && out[1] == 1 && out[2] == 2, "a counter reset lands on the snapshot");

    // A long pause (no packets while standing) does not make the next move crawl.
    memset(&lerp, 0, sizeof(lerp));
    RemoteLerp_Snap(&lerp, (float[3]){0, 0, 0}, 0);
    RemoteLerp_Push(&lerp, (float[3]){0, 0, 0}, (float[3]){0.5f, 0, 0}, 6);
    RemoteLerp_Push(&lerp, (float[3]){0.5f, 0, 0}, (float[3]){0.5f, 0, 0}, 12);
    RemoteLerp_Push(&lerp, (float[3]){0.5f, 0, 0}, (float[3]){1.0f, 0, 0}, 400);
    CHECK(lerp.duration == REMOTE_LERP_FIRST_FRAMES, "after a pause the pace is the usual one (%d frames)", lerp.duration);

    // Knowing when the game moved the destination itself.
    CHECK(RemoteLerp_Targets(&lerp, (float[3]){1.0f, 0, 0}), "the latest snapshot is the target");
    CHECK(!RemoteLerp_Targets(&lerp, (float[3]){7, 0, 0}), "a destination set elsewhere is noticed");
    RemoteLerp_Snap(&lerp, (float[3]){7, 0, 0}, 401);
    RemoteLerp_Sample(&lerp, 401, out);
    CHECK(out[0] == 7 && RemoteLerp_Targets(&lerp, (float[3]){7, 0, 0}), "a snap lands at once");
}

/* ---------------------------------------------------------------------- *
 * Tombstones
 * ---------------------------------------------------------------------- */

static int32_t mulQ12Test(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 12);
}

static void tombNormal(const TombPolygon *p, double n[3])
{
    double ux = p->v[1].x - p->v[0].x, uy = p->v[1].y - p->v[0].y, uz = p->v[1].z - p->v[0].z;
    double vx = p->v[2].x - p->v[0].x, vy = p->v[2].y - p->v[0].y, vz = p->v[2].z - p->v[0].z;
    n[0] = uy * vz - uz * vy;
    n[1] = uz * vx - ux * vz;
    n[2] = ux * vy - uy * vx;
}

static bool sameVertex(const TombVertex *a, const TombVertex *b)
{
    return a->x == b->x && a->y == b->y && a->z == b->z;
}

static void testTombMesh(bool ct)
{
    TombMesh mesh;
    Tomb_BuildMesh(&mesh, ct, ANIM_ONE);
    const char *name = ct ? "CT" : "T";

    int stone = 0, letters = 0, unsorted = 0, nonPlanar = 0, inward = 0, outOfRange = 0;
    bool engraving = false;
    int32_t minY = 1 << 30, maxY = -(1 << 30);
    double centre[3] = {0, (TOMB_TOP - TOMB_SINK) / 2.0, 0};
    for (int i = 0; i < mesh.count; i++)
    {
        const TombPolygon *p = &mesh.polygons[i];
        // The stone's quads, its triangles, then the engraving.
        if (engraving && !p->letter)
            unsorted++;
        engraving |= p->letter;
        if (i > 0 && !p->letter && p->count > mesh.polygons[i - 1].count)
            unsorted++;
        double n[3];
        tombNormal(p, n);
        if (p->count == 4)
        {
            double d = (p->v[3].x - p->v[0].x) * n[0] + (p->v[3].y - p->v[0].y) * n[1] + (p->v[3].z - p->v[0].z) * n[2];
            if (fabs(d) > 0.5)
                nonPlanar++;
        }
        double c[3] = {0, 0, 0};
        for (int k = 0; k < p->count; k++)
        {
            c[0] += p->v[k].x / (double)p->count;
            c[1] += p->v[k].y / (double)p->count;
            c[2] += p->v[k].z / (double)p->count;
            if (abs(p->v[k].x) >= 8 * 4096 || abs(p->v[k].y) >= 8 * 4096 || abs(p->v[k].z) >= 8 * 4096)
                outOfRange++;
            if (!p->letter)
            {
                if (p->v[k].y < minY)
                    minY = p->v[k].y;
                if (p->v[k].y > maxY)
                    maxY = p->v[k].y;
            }
        }
        if (p->letter)
        {
            letters++;
            if (n[2] >= 0)
                inward++;
        }
        else
        {
            stone++;
            // The stone is convex, so every face points away from its middle.
            if (n[0] * (c[0] - centre[0]) + n[1] * (c[1] - centre[1]) + n[2] * (c[2] - centre[2]) <= 0)
                inward++;
        }
    }
    CHECK(mesh.count <= TOMB_MAX_POLYGONS && stone == 17 && letters == (ct ? 5 : 2),
          "%s stone: %d polygons, %d of them engraving", name, stone, letters);
    CHECK(unsorted == 0, "%s stone: quads, triangles, engraving (%d out of order)", name, unsorted);
    CHECK(nonPlanar == 0 && outOfRange == 0, "%s stone: flat quads (%d not), vertices in VTX_16 range (%d not)", name,
          nonPlanar, outOfRange);
    CHECK(inward == 0, "%s stone: every face wound counter-clockwise from outside, as the rig (%d not)", name, inward);
    CHECK(minY == -TOMB_SINK && maxY == TOMB_TOP, "%s stone: from %.3f below the ground to %.3f above", name,
          -minY / 4096.0, maxY / 4096.0);

    // Closed: every edge of the stone is shared by exactly two faces, once in each direction,
    // so from any side, above or below the ground, there is no hole to see through.
    int open = 0, doubled = 0;
    for (int i = 0; i < mesh.count; i++)
    {
        const TombPolygon *p = &mesh.polygons[i];
        if (p->letter)
            continue;
        for (int k = 0; k < p->count; k++)
        {
            const TombVertex *a = &p->v[k], *b = &p->v[(k + 1) % p->count];
            int reverse = 0, same = 0;
            for (int j = 0; j < mesh.count; j++)
            {
                const TombPolygon *q = &mesh.polygons[j];
                if (q->letter)
                    continue;
                for (int m = 0; m < q->count; m++)
                {
                    const TombVertex *c = &q->v[m], *d = &q->v[(m + 1) % q->count];
                    if (sameVertex(a, d) && sameVertex(b, c))
                        reverse++;
                    if (sameVertex(a, c) && sameVertex(b, d))
                        same++;
                }
            }
            if (reverse != 1)
                open++;
            if (same != 1)
                doubled++;
        }
    }
    CHECK(open == 0 && doubled == 0, "%s stone: closed, every edge shared once each way (%d open, %d repeated)", name,
          open, doubled);

    // The engraving: raised on the front face, on its upright part, reading left to right for
    // someone facing it (the reader's left is the stone's +x).
    int stray = 0;
    double minX[5], maxX[5], minV[5], maxV[5];
    int l = 0;
    for (int i = 0; i < mesh.count; i++)
    {
        const TombPolygon *p = &mesh.polygons[i];
        if (!p->letter)
            continue;
        minX[l] = minV[l] = 1e9;
        maxX[l] = maxV[l] = -1e9;
        for (int k = 0; k < p->count; k++)
        {
            double x = p->v[k].x / 4096.0, y = p->v[k].y / 4096.0;
            double w = TOMB_HALF_WIDTH / 4096.0, h = TOMB_SHOULDER / 4096.0;
            bool onFace = fabs(x) < w - 0.02 && y > 0.1 && y <= h - 0.02;
            // Clear of the face by at least 0.04: the depth buffer tells that apart to ~10 units.
            if (p->v[k].z > -TOMB_HALF_THICKNESS - 164 || p->v[k].z < -TOMB_HALF_THICKNESS - 410 || !onFace)
                stray++;
            minX[l] = fmin(minX[l], x);
            maxX[l] = fmax(maxX[l], x);
            minV[l] = fmin(minV[l], y);
            maxV[l] = fmax(maxV[l], y);
        }
        l++;
    }
    CHECK(stray == 0, "%s stone: the engraving lies on the front, below the round top (%d vertices elsewhere)", name,
          stray);
    if (ct)
    {
        // C (three bars) then T (two); the C to the reader's left, its upright bar at its left.
        CHECK(fmax(maxX[3], maxX[4]) < fmin(fmin(minX[0], minX[1]), minX[2]), "CT reads C then T");
        CHECK(maxX[0] >= maxX[1] && maxX[0] >= maxX[2] && maxV[0] - minV[0] > maxV[1] - minV[1],
              "the C opens to the reader's right");
    }
    else
    {
        CHECK(fabs((minX[0] + maxX[0]) / 2) < 0.01 && fabs((minX[1] + maxX[1]) / 2) < 0.01, "T is centred");
    }
}

static void testTombstone(void)
{
    section("tombstones");
    testTombMesh(true);
    testTombMesh(false);

    // In the map's shadow the stone is darker, and the engraving still stands out: dark
    // letters on the face around them, by at least 2:1.
    TombMesh lit, dim;
    Tomb_BuildMesh(&lit, true, ANIM_ONE);
    Tomb_BuildMesh(&dim, true, TOMB_SHADOW_LIGHT);
    bool darker = lit.count == dim.count;
    int letterLevel = 0, faceLevel = 0;
    for (int i = 0; i < lit.count && darker; i++)
        for (int k = 0; k < lit.polygons[i].count; k++)
        {
            int a = lit.polygons[i].v[k].color & 31, b = dim.polygons[i].v[k].color & 31;
            if (b > a || (a > 3 && b >= a))
                darker = false;
            if (lit.polygons[i].letter)
                letterLevel = a;
            else if (lit.polygons[i].v[k].z == -TOMB_HALF_THICKNESS && lit.polygons[i].v[k].y == -TOMB_SINK)
                faceLevel = faceLevel ? faceLevel : a;
        }
    // The face at the letters' height, between its buried bottom and its shoulder.
    int faceAtLetters = faceLevel + (18 - faceLevel) * (TOMB_SINK + 1475) / (TOMB_SINK + TOMB_SHOULDER);
    CHECK(darker, "a stone in shadow is darker all over");
    CHECK(faceAtLetters >= 2 * letterLevel, "the engraving stands out (face %d, letters %d)", faceAtLetters, letterLevel);

    // The engraving faces the way the player faced: glRotateYi(yaw) takes the front (-z) to
    // the character's forward, (-sin yaw, -cos yaw).
    double worst = 0;
    for (int32_t yaw = -ANIM_TURN; yaw < ANIM_TURN; yaw += 1237)
    {
        Affine r = affRotate(1, yaw);
        double fx = -r.m[0][2], fz = -r.m[2][2]; // the rotated (0, 0, -1)
        double ex = -sin(yaw * TURN_RAD / ANIM_TURN), ez = -cos(yaw * TURN_RAD / ANIM_TURN);
        worst = fmax(worst, fabs(fx - ex) + fabs(fz - ez));
    }
    CHECK(worst < 1e-9, "the engraving faces the player's last facing (off by %.2g)", worst);
    // It stands a little ahead of the spot, clear of the fallen feet, but inside the hull
    // (0.35 either way), so not in a wall the player stood against.
    CHECK(TOMB_AHEAD + TOMB_HALF_THICKNESS <= 1434 && TOMB_AHEAD - TOMB_HALF_THICKNESS >= 600,
          "the stone stands inside the player's hull, ahead of its feet");

    // The body: whole through the fall, then fading, never alpha 0 while drawn, then gone.
    bool monotonic = true, zeroEarly = false;
    int last = 31;
    for (int32_t f = -1; f < TOMB_FADE_START + TOMB_FADE_FRAMES + 30; f++)
    {
        int a = Tomb_BodyAlpha(f);
        if (a > last)
            monotonic = false;
        if (a == 0 && f < TOMB_FADE_START + TOMB_FADE_FRAMES)
            zeroEarly = true;
        last = a;
    }
    CHECK(Tomb_BodyAlpha(-1) == 31 && Tomb_BodyAlpha(0) == 31 && Tomb_BodyAlpha(TOMB_FADE_START - 1) == 31,
          "alive, and falling, the body is solid");
    CHECK(TOMB_FADE_START >= 40, "the fade waits for the fall (40 frames) to end");
    CHECK(Tomb_BodyAlpha(TOMB_FADE_START) < 31 && Tomb_BodyAlpha(TOMB_FADE_START + TOMB_FADE_FRAMES / 4) <= 18,
          "then it leaves full strength quickly (%d a quarter of the way)",
          Tomb_BodyAlpha(TOMB_FADE_START + TOMB_FADE_FRAMES / 4));
    CHECK(monotonic && !zeroEarly, "the body only fades, and stays drawable until it is gone");
    CHECK(Tomb_BodyAlpha(TOMB_FADE_START + TOMB_FADE_FRAMES - 1) <= 2 &&
              Tomb_BodyAlpha(TOMB_FADE_START + TOMB_FADE_FRAMES) == 0 && Tomb_BodyAlpha(1 << 20) == 0,
          "the body fades all the way out, and stays gone");
    CHECK(TOMB_FADE_FRAMES >= 60 && TOMB_FADE_START + TOMB_FADE_FRAMES >= 100,
          "it goes slowly: fading for %.2f s, gone %.2f s after the death", TOMB_FADE_FRAMES / 60.0,
          (TOMB_FADE_START + TOMB_FADE_FRAMES) / 60.0);
    // Training, deathmatch and gun game respawn after 2 s: gone by then, not popped away.
    CHECK(TOMB_FADE_START + TOMB_FADE_FRAMES < 120, "the body is gone before a 2 s respawn (%d frames)",
          TOMB_FADE_START + TOMB_FADE_FRAMES);

    // The stone: hidden, then growing out of its buried bottom, then standing; and, when its
    // player dies again, the same backwards. However deep it is buried, no vertex ever goes
    // below its bottom at rest, so it never reaches through a floor it clears standing.
    TombMesh stoneMesh;
    Tomb_BuildMesh(&stoneMesh, true, ANIM_ONE);
    const int32_t sinks[] = {TOMB_SINK_FLAT, TOMB_SINK_SLOPE, 2 * TOMB_SINK_SLOPE};
    bool rises = true, folded = true, rests = true, neverBelow = true, sinksBack = true;
    int32_t worstStep = 0;
    for (int n = 0; n < 3; n++)
    {
        int32_t sink = sinks[n];
        int32_t prevTop = INT32_MIN;
        for (int32_t age = TOMB_RISE_START; age <= TOMB_RISE_START + TOMB_RISE_FRAMES + 10; age++)
        {
            int32_t o = Tomb_RiseOffset(age, sink), top = INT32_MIN, bottom = INT32_MAX;
            for (int i = 0; i < stoneMesh.count; i++)
                for (int k = 0; k < stoneMesh.polygons[i].count; k++)
                {
                    int32_t y = Tomb_VertexY(stoneMesh.polygons[i].v[k].y, sink, o);
                    top = y > top ? y : top;
                    bottom = y < bottom ? y : bottom;
                }
            if (bottom < -sink)
                neverBelow = false;
            if (age == TOMB_RISE_START && top != -sink)
                folded = false;
            if (top < prevTop || o > 0)
                rises = false;
            if (prevTop != INT32_MIN && top > 0 && top - prevTop > worstStep)
                worstStep = top - prevTop;
            prevTop = top;
            if (age >= TOMB_RISE_START + TOMB_RISE_FRAMES && (top != TOMB_TOP || bottom != -sink))
                rests = false;
        }
        int32_t before = 0;
        for (int32_t age = 0; age <= TOMB_SINK_FRAMES; age++)
        {
            int32_t o = Tomb_SinkOffset(age, sink);
            if (o > before || Tomb_VertexY(TOMB_TOP, sink, o) < -sink)
                sinksBack = false;
            before = o;
        }
        if (Tomb_VertexY(TOMB_TOP, sink, Tomb_SinkOffset(TOMB_SINK_FRAMES, sink)) != -sink)
            sinksBack = false;
    }
    CHECK(!Tomb_Visible(TOMB_RISE_START - 1) && Tomb_Visible(TOMB_RISE_START), "the stone appears at its time");
    CHECK(folded, "it starts folded into its buried bottom, top and all");
    CHECK(rises && rests, "it only grows, and comes to rest standing with its bottom buried");
    CHECK(neverBelow, "no part of it ever goes below its buried bottom");
    CHECK(worstStep < TOMB_TOP / 4, "it grows smoothly (at most %.3f a frame)", worstStep / 4096.0);
    CHECK(sinksBack && !Tomb_Sunk(TOMB_SINK_FRAMES - 1) && Tomb_Sunk(TOMB_SINK_FRAMES),
          "an old stone folds back into the ground the same way");
    CHECK(Tomb_BodyAlpha(TOMB_RISE_START) <= 8 && Tomb_BodyAlpha(TOMB_RISE_START) > 0,
          "it rises once the body is faint (alpha %d), not through a solid one", Tomb_BodyAlpha(TOMB_RISE_START));
    CHECK(abs(TOMB_RISE_START + TOMB_RISE_FRAMES - (TOMB_FADE_START + TOMB_FADE_FRAMES)) <= 2,
          "and stands as the body vanishes");

    // The ground under a death spot.
    const int32_t U = 4096;
    TombGroundProbe probe;
    Tomb_ProbeBegin(&probe, 0, 0, 0);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 0);
    CHECK(Tomb_ProbeGround(&probe) == 0, "on a floor: the floor");

    Tomb_ProbeBegin(&probe, 0, 0, 3 * U);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 0);
    CHECK(Tomb_ProbeGround(&probe) == 0, "killed in mid-air: the floor below");

    Tomb_ProbeBegin(&probe, 0, 0, 0);
    Tomb_ProbeBox(&probe, -U, U, -U, U, U);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 0);
    CHECK(Tomb_ProbeGround(&probe) == 0, "a surface well above the feet is not the ground");

    Tomb_ProbeBegin(&probe, 0, 0, 0);
    Tomb_ProbeBox(&probe, -U, U, -U, U, U * 3 / 10);
    CHECK(Tomb_ProbeGround(&probe) == U * 3 / 10, "a body sunk a little into its floor still finds it");

    Tomb_ProbeBegin(&probe, 0, 0, 2 * U);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 2 * U);
    Tomb_ProbeBox(&probe, -4 * U, 4 * U, -4 * U, 4 * U, 0);
    CHECK(Tomb_ProbeGround(&probe) == 2 * U, "on an upper level: that level, not the one under it");

    Tomb_ProbeBegin(&probe, U + 1, 0, 2 * U);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 2 * U);
    Tomb_ProbeBox(&probe, -4 * U, 4 * U, -4 * U, 4 * U, 0);
    CHECK(Tomb_ProbeGround(&probe) == 0, "past a ledge: the floor below it");

    Tomb_ProbeBegin(&probe, U, U, 0);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 0);
    CHECK(probe.found, "the edge of a box counts as on it");

    Tomb_ProbeBegin(&probe, 0, U / 2, U);
    Tomb_ProbeBox(&probe, -4 * U, 4 * U, -4 * U, 4 * U, -U / 10);
    Tomb_ProbeRamp(&probe, -U, U, 0, U, false, 0, U);
    CHECK(abs(Tomb_ProbeGround(&probe) - U / 2) <= 1, "on a ramp along z: its surface there (%.3f)",
          Tomb_ProbeGround(&probe) / 4096.0);

    Tomb_ProbeBegin(&probe, U / 4, 0, U);
    Tomb_ProbeRamp(&probe, 0, U, -U, U, true, U, 0);
    CHECK(abs(Tomb_ProbeGround(&probe) - U * 3 / 4) <= 1, "on a ramp along x, falling: its surface there (%.3f)",
          Tomb_ProbeGround(&probe) / 4096.0);

    Tomb_ProbeBegin(&probe, 10 * U, 10 * U, U);
    Tomb_ProbeBox(&probe, -U, U, -U, U, 0);
    CHECK(!probe.found && Tomb_ProbeGround(&probe) == U, "nothing under the point: the feet");
}

/* Where a stone stands, on small made-up maps. */
static void testTombPlacement(void)
{
    section("where a tombstone stands");
    const int32_t U = 4096, FEET_REACH = 1843;
    TombPlacement at;

    // Flat floor, facing -z: on the floor, just ahead, buried a little.
    TombBox floor0 = {-8 * U, 8 * U, -8 * U, 8 * U, 0};
    TombMap flat = {&floor0, 1, NULL, 0};
    Tomb_Place(&flat, 0, 0, 0, FEET_REACH, 0, &at);
    CHECK(at.ground == 0 && at.sink == TOMB_SINK_FLAT && at.x == 0 && at.z < 0 && at.z >= -TOMB_AHEAD,
          "on a floor: on it, a little ahead, buried %.2f", at.sink / 4096.0);

    // It stays inside the hull whichever way the player faced, so out of any wall it touched.
    bool inside = true;
    double worstOut = 0;
    for (int32_t yaw = 0; yaw < ANIM_TURN; yaw += 1111)
    {
        Tomb_Place(&flat, 0, 0, 0, FEET_REACH, yaw, &at);
        double s = sin(yaw * TURN_RAD / ANIM_TURN), c = cos(yaw * TURN_RAD / ANIM_TURN);
        for (int k = 0; k < 4; k++)
        {
            double a = (k & 1) ? 1 : -1, b = (k & 2) ? 1 : -1;
            double w = TOMB_HALF_WIDTH / 4096.0, d = TOMB_HALF_THICKNESS / 4096.0;
            double x = at.x / 4096.0 + a * w * c - b * d * s, z = at.z / 4096.0 - a * w * s - b * d * c;
            worstOut = fmax(worstOut, fmax(fabs(x), fabs(z)));
            if (fabs(x) > 0.3501 || fabs(z) > 0.3501)
                inside = false;
        }
    }
    CHECK(inside, "at any facing the stone stays inside the player's hull (reaches %.4f of 0.35)", worstOut);

    // Killed in mid-air: on the floor below.
    Tomb_Place(&flat, 0, 0, 3 * U, FEET_REACH, 0, &at);
    CHECK(at.ground == 0, "killed in mid-air: on the floor below");

    // No floor at all: at the feet, buried deep.
    TombMap none = {NULL, 0, NULL, 0};
    Tomb_Place(&none, 0, 0, U, FEET_REACH, 0, &at);
    CHECK(at.ground == U && at.sink >= TOMB_SINK_SLOPE, "no floor known: at the feet, buried deep");

    // The seam at a ramp's top: a 0.33 gap between the ramp and its platform, with the floor
    // far below. The hull stands on both; so does the stone.
    TombBox seamBoxes[2] = {{-8 * U, 8 * U, -8 * U, 16 * U, -3 * U}, {-2 * U, 2 * U, 4 * U + 1352, 8 * U, 2 * U}};
    TombRamp seamRamp = {-2 * U, 2 * U, 0, 4 * U, false, 0, 2 * U};
    TombMap seam = {seamBoxes, 2, &seamRamp, 1};
    Tomb_Place(&seam, 0, 4 * U + 676, 2 * U, FEET_REACH, 0, &at);
    CHECK(at.ground == 2 * U, "on the seam at a ramp's top: on the platform (%.2f), not the floor below",
          at.ground / 4096.0);

    // A ledge: a platform ending at x = 0, a floor 2 below. Centred 0.1 inside the edge, the
    // stone (0.54 wide across x) would hang over it: moved back on. Centred 0.2 past the
    // edge, the hull still stands on the platform: so does the stone, moved back onto it.
    TombBox ledgeBoxes[2] = {{-8 * U, 8 * U, -8 * U, 8 * U, -U}, {-4 * U, 0, -4 * U, 4 * U, U}};
    TombMap ledge = {ledgeBoxes, 2, NULL, 0};
    Tomb_Place(&ledge, -U / 10, 0, U, FEET_REACH, 0, &at);
    CHECK(at.ground == U && at.x + TOMB_HALF_WIDTH <= 0 && at.sink == TOMB_SINK_FLAT,
          "0.1 inside a ledge: on it, moved back so no corner hangs over (edge at %.3f)",
          (at.x + TOMB_HALF_WIDTH) / 4096.0);
    Tomb_Place(&ledge, U / 5, 0, U, FEET_REACH, 0, &at);
    CHECK(at.ground == U && at.x + TOMB_HALF_WIDTH <= 0, "0.2 past a ledge: back on the ledge it stood on (edge at %.3f)",
          (at.x + TOMB_HALF_WIDTH) / 4096.0);

    // On a ramp rising along z at 0.5, turned side on: its ends stand at different heights,
    // and it goes deep enough below the lower one for the steps a ramp line can hide.
    TombRamp slopeRamp = {-4 * U, 4 * U, -4 * U, 4 * U, false, -2 * U, 2 * U};
    TombMap slope = {NULL, 0, &slopeRamp, 1};
    Tomb_Place(&slope, 0, 0, 0, FEET_REACH, ANIM_TURN / 4, &at);
    CHECK(abs(at.ground) <= U / 8 && at.sink >= TOMB_SINK_SLOPE + mulQ12Test(TOMB_HALF_WIDTH, U / 2) - 8,
          "on a slope: on its line, buried %.2f below it", at.sink / 4096.0);

    // A thin slab (0.2) over a room: buried less than its thickness.
    TombBox slab = {-4 * U, 4 * U, -4 * U, 4 * U, 3 * U};
    TombMap slabMap = {&slab, 1, NULL, 0};
    Tomb_Place(&slabMap, 0, 0, 3 * U, FEET_REACH, 0, &at);
    CHECK(at.ground == 3 * U && at.sink < U / 5, "on a thin slab: not through it into the room below");

    // A bot walking beside a curb 0.3 high, its hull overlapping it: on the floor, not the curb.
    TombBox curbBoxes[2] = {{-8 * U, 8 * U, -8 * U, 8 * U, 0}, {U / 5, U, -4 * U, 4 * U, U * 3 / 10}};
    TombMap curb = {curbBoxes, 2, NULL, 0};
    Tomb_Place(&curb, 0, 0, 0, 3686, 0, &at);
    CHECK(at.ground == 0, "beside a curb: on the floor, not lifted onto the curb");

    // A bot whose waypoint line cut 0.6 into its floor still finds it.
    Tomb_Place(&flat, 0, 0, -U * 6 / 10, 3686, 0, &at);
    CHECK(at.ground == 0, "a bot sunk 0.6 into its floor: on the floor");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "Counter-Strike-nds/data/player_rig.bin";
    if (!loadRig(path))
    {
        printf("cannot read %s\n", path);
        return 2;
    }
    testTrig();
    testRigFile();
    testGait();
    testFrameRate();
    testFacing();
    testPoses();
    testRemoteLerp();
    testTombstone();
    testTombPlacement();
    if (g_failures)
    {
        printf("\nFAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("\nOK: %d checks passed\n", g_checks);
    return 0;
}
