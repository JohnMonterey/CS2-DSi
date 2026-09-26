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
    if (g_failures)
    {
        printf("\nFAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("\nOK: %d checks passed\n", g_checks);
    return 0;
}
